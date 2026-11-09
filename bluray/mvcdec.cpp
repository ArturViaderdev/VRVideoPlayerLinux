// vr-mvcdec (3dvision-mvcdec) - decodes 3D Blu-ray video (H.264 MVC, both eyes) to a full
// side-by-side YUV4MPEG2 stream for the VR player (vrvideo) and the 3D Vision player.
//
// Copyright (C) 2026 Artur Viader Mataix. GPLv3 (see LICENSE).
// Built on the MVC demuxers and the MVC-extended edge264 decoder of SyLC 3D Player
// (https://github.com/5ymph0en1x/SyLC, GPLv3; edge264 by Thibault Raffaillac, BSD-3).
//
//   3dvision-mvcdec [options] SEGMENT [SEGMENT...]  > out.y4m
//     SEGMENT        clip.ssif  |  base.m2ts:dependent.m2ts   (played in order)
//     --start-ms N   start N ms into the FIRST segment (next keyframe)
//     --right-first  the base view is the right eye (MPLS MVC_Base_view_R_flag)
//     --threads N    decoder worker threads (default: auto)
//     --out PATH     write to PATH (a FIFO) instead of stdout
//     --seek-table F frame-accurate seek tables of the first segment (from the .clpi, made by
//                    3dvision-bd): lines "B <pts_ms> <m2ts byte>" / "S <pts_ms> <ssif byte>"
//   stderr: "START <ms>" = timestamp of the first frame written (for the audio),
//           "SEGMENT <i>" when a segment starts, "END" at the end.
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <map>
#include <string>
#include <vector>

#include "mvc_decoder.h"
#include "mvc_ssif_demuxer.h"

using namespace mvc_demux;

namespace {

FILE* g_out = stdout;
bool g_headerDone = false;
int g_W = 0, g_H = 0;
std::vector<uint8_t> g_row;

bool WriteAll(const void* p, size_t n) { return fwrite(p, 1, n, g_out) == n; }

// left | right, row by row, for Y then Cb then Cr (4:2:0)
using View = DecodedMVCFrame::View;
bool WriteFrame(const View& base, const View& dep, bool rightFirst, double fps) {
  const View& L = rightFirst ? dep : base;
  const View& R = rightFirst ? base : dep;
  const int w = base.width, h = base.height;
  if (!g_headerDone) {
    g_W = w;
    g_H = h;
    // common Blu-ray rates as exact fractions
    int num = int(fps * 1000 + 0.5), den = 1000;
    if (std::abs(fps - 23.976) < 0.01) { num = 24000; den = 1001; }
    else if (std::abs(fps - 29.97) < 0.01) { num = 30000; den = 1001; }
    else if (std::abs(fps - 59.94) < 0.01) { num = 60000; den = 1001; }
    else if (fps <= 0) { num = 24000; den = 1001; }
    char hdr[160];
    int n = snprintf(hdr, sizeof hdr, "YUV4MPEG2 W%d H%d F%d:%d Ip A1:1 C420jpeg XCOLORRANGE=LIMITED\n", w * 2, h,
                     num, den);
    if (!WriteAll(hdr, size_t(n))) return false;
    g_headerDone = true;
  }
  if (w != g_W || h != g_H) return true;  // size change mid-stream: skip (never seen on BD3D)
  if (!WriteAll("FRAME\n", 6)) return false;
  auto plane = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int pw, int ph) {
    g_row.resize(size_t(pw) * 2);
    for (int y = 0; y < ph; ++y) {
      memcpy(g_row.data(), a.data() + size_t(y) * pw, size_t(pw));
      memcpy(g_row.data() + pw, b.data() + size_t(y) * pw, size_t(pw));
      if (!WriteAll(g_row.data(), g_row.size())) return false;
    }
    return true;
  };
  if (dep.width != w || dep.height != h) return true;
  const int cw = base.chroma_width, ch = base.chroma_height;
  return plane(L.y_plane, R.y_plane, w, h) && plane(L.cb_plane, R.cb_plane, cw, ch) &&
         plane(L.cr_plane, R.cr_plane, cw, ch);
}

struct Segment { std::string a, b; };

// edge264 with worker threads hands frames over slightly out of display order, and the second
// view of a frame often arrives with the NEXT frame. Keep a few frames, put them in display
// order (POC, monotone across IDRs in SyLC's edge264) and pair each base view with the
// dependent view of the SAME POC, as SyLC's player does.
class Reorder {
 public:
  static constexpr size_t kDepth = 6;
  void Add(DecodedMVCFrame& f) {
    const int pb = f.picture_order_cnt;
    if (pb > lastOut_ || !any_) {
      Slot& s = map_[pb];
      s.base = std::move(f.base_view);
      s.hasBase = true;
      if (f.has_mvc && !s.hasOwn) { s.own = f.dependent_view; s.hasOwn = true; }
    }
    if (f.has_mvc) {
      const int pd = f.picture_order_cnt_mvc;
      if (pd > lastOut_ || !any_) {
        Slot& d = map_[pd];
        d.dep = std::move(f.dependent_view);
        d.hasDep = true;
      }
    }
  }
  // next frame in display order (when enough are buffered, or all when flushing)
  bool Pop(bool flush, View& base, View& dep) {
    while (!map_.empty() && (flush || map_.size() > kDepth)) {
      auto it = map_.begin();
      Slot s = std::move(it->second);
      lastOut_ = it->first;
      any_ = true;
      map_.erase(it);
      if (!s.hasBase) continue;  // a lone dependent view (its base was dropped)
      base = std::move(s.base);
      if (s.hasDep) dep = std::move(s.dep);
      else if (s.hasOwn) dep = std::move(s.own);
      else dep = base;  // no second view: 2D for this frame rather than dropping it
      return true;
    }
    return false;
  }
  void Clear() { map_.clear(); any_ = false; lastOut_ = INT32_MIN; }

 private:
  struct Slot { View base, dep, own; bool hasBase = false, hasDep = false, hasOwn = false; };
  std::map<int, Slot> map_;
  int lastOut_ = INT32_MIN;
  bool any_ = false;
};

// does this base-view access unit start with an intra picture (IDR, or an I slice that a seek
// can start from: Blu-ray entry points are often non-IDR I frames)?
bool IsIntraAU(const std::vector<uint8_t>& d) {
  for (size_t i = 0; i + 4 < d.size(); ++i) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
    const uint8_t t = d[i + 3] & 0x1F;
    if (t == 5) return true;
    if (t != 1) continue;
    // exp-Golomb: first_mb_in_slice, slice_type
    size_t bit = (i + 4) * 8;
    auto rd = [&]() -> int {
      return (bit >> 3) < d.size() ? (d[bit >> 3] >> (7 - (bit & 7))) & 1 : 1;
    };
    auto ue = [&]() -> uint32_t {
      int z = 0;
      while (rd() == 0 && z < 31) { ++z; ++bit; }
      ++bit;
      uint32_t v = 0;
      for (int k = 0; k < z; ++k) { v = (v << 1) | uint32_t(rd()); ++bit; }
      return (1u << z) - 1 + v;
    };
    ue();
    uint32_t st = ue() % 5;
    return st == 2 || st == 4;  // I or SI
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  // the demuxer library prints diagnostics on stdout: keep the real stdout for the video only
  {
    int videoFd = dup(1);
    dup2(2, 1);
    g_out = fdopen(videoFd, "wb");
  }
  int64_t startMs = 0;
  bool rightFirst = false;
  int threads = -1;
  std::string outPath, seekTablePath;
  std::vector<Segment> segs;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--start-ms" && i + 1 < argc) startMs = atoll(argv[++i]);
    else if (a == "--right-first") rightFirst = true;
    else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
    else if (a == "--seek-table" && i + 1 < argc) seekTablePath = argv[++i];
    else if (a == "-h" || a == "--help") {
      fprintf(stderr, "usage: %s [--start-ms N] [--right-first] [--threads N] [--out FILE] "
                      "clip.ssif|base.m2ts:dep.m2ts ...\n", argv[0]);
      return 0;
    } else {
      size_t c = a.find(".m2ts:");
      if (c == std::string::npos) c = a.find(".M2TS:");
      if (c != std::string::npos) segs.push_back({a.substr(0, c + 5), a.substr(c + 6)});
      else segs.push_back({a, ""});
    }
  }
  if (segs.empty()) { fprintf(stderr, "no input\n"); return 2; }
  // the output (a FIFO) is opened only when the first frame is ready: the player opens its end
  // after reading "START" (it needs that timestamp to place the audio)
  auto openOut = [&]() {
    if (g_out && outPath.empty()) { setvbuf(g_out, nullptr, _IOFBF, 1 << 22); return true; }
    if (!outPath.empty()) {
      g_out = fopen(outPath.c_str(), "wb");
      if (!g_out) { fprintf(stderr, "ERROR cannot open %s: %s\n", outPath.c_str(), strerror(errno)); return false; }
    }
    setvbuf(g_out, nullptr, _IOFBF, 1 << 22);
    return true;
  };
  if (!outPath.empty()) g_out = nullptr;

  bool startPrinted = false;
  for (size_t si = 0; si < segs.size(); ++si) {
    MVCSSIFDemuxer dmx;
    bool ok = segs[si].b.empty() ? dmx.open(segs[si].a) : dmx.openDual(segs[si].a, segs[si].b);
    if (!ok) { fprintf(stderr, "ERROR cannot open %s\n", segs[si].a.c_str()); return 3; }
    auto vi = dmx.getVideoInfo();
    if (!vi.hasMVC) { fprintf(stderr, "ERROR no MVC (3D) video in %s\n", segs[si].a.c_str()); return 4; }
    fprintf(stderr, "SEGMENT %zu %ux%u %.3f fps\n", si, vi.width, vi.height, vi.fps);
    double fps = vi.fps;

    MVCDecoder dec;
    if (!dec.init(threads)) { fprintf(stderr, "ERROR decoder: %s\n", dec.getLastError()); return 5; }
    const auto& cp = dmx.getCodecPrivate();
    if (!cp.empty()) dec.decodeAnnexBStream(cp.data(), cp.size());

    MVCSSIFDemuxer::FramePair fp;
    bool started = false;
    int64_t seekTo = (si == 0) ? startMs : 0;
    if (seekTo > 0 && !seekTablePath.empty()) {
      std::vector<int64_t> bp, sp;
      std::vector<uint64_t> bb, sb;
      if (FILE* t = fopen(seekTablePath.c_str(), "r")) {
        char kind;
        long long pts;
        unsigned long long byte;
        while (fscanf(t, " %c %lld %llu", &kind, &pts, &byte) == 3) {
          if (kind == 'B') { bp.push_back(pts); bb.push_back(byte); }
          else if (kind == 'S') { sp.push_back(pts); sb.push_back(byte); }
        }
        fclose(t);
      }
      if (!bp.empty()) dmx.setBaseSeekTable(bp, bb);
      if (!sp.empty()) dmx.setSsifSeekTable(sp, sb);
    }
    if (seekTo > 0) {
      // the first pair sets the timestamp origin; then jump
      if (!dmx.readNextFramePair(fp)) { fprintf(stderr, "ERROR empty stream\n"); return 6; }
      if (!dmx.seek(seekTo)) fprintf(stderr, "seek to %lld ms failed, starting at the beginning\n", (long long)seekTo);
      dec.flush();
      if (!cp.empty()) dec.decodeAnnexBStream(cp.data(), cp.size());
    }
    DecodedMVCFrame frame;
    // After a seek decoding can start on a non-IDR I frame: the B frames right after it in decode
    // order but before it in display order ("leading" pictures) reference the previous GOP and
    // would be garbage. Their timestamps are lower than the I frame's: drop that many outputs.
    int64_t firstTs = -1;
    int skipOut = 0;
    Reorder ro;
    View vb, vd;
    auto emit = [&](bool flush = false) -> bool {  // false: the reader went away
      while (dec.getFrame(frame)) ro.Add(frame);
      while (ro.Pop(flush, vb, vd)) {
        if (skipOut > 0) { --skipOut; continue; }
        if (!WriteFrame(vb, vd, rightFirst, fps)) return false;
      }
      return true;
    };
    while (dmx.readNextFramePair(fp)) {
      if (started && seekTo > 0 && int64_t(fp.timestamp) < firstTs) ++skipOut;
      if (!started) {
        if (!(fp.isKeyframe || (seekTo > 0 && IsIntraAU(fp.baseData)))) continue;
        started = true;
        firstTs = int64_t(fp.timestamp);
        if (!startPrinted) {
          fprintf(stderr, "START %lld\n", (long long)fp.timestamp);
          fflush(stderr);
          startPrinted = true;
          if (!openOut()) return 2;
        }
      }
      int r = dec.decodeAccessUnitPair(fp.baseData.data(), fp.baseData.size(), fp.dependentData.data(),
                                       fp.dependentData.size());
      if (r != 0 && r != EAGAIN && r != ENOMSG && r != ENOBUFS) {
        static int warned = 0;
        if (warned++ < 5) fprintf(stderr, "decode warning %d: %s\n", r, dec.getLastError());
      }
      if (!emit()) return 0;  // reader closed (player quit / seek)
    }
    dec.bumpFrames();
    if (!emit(true)) return 0;
    if (g_out) fflush(g_out);
  }
  fprintf(stderr, "END\n");
  if (g_out) fclose(g_out);
  return 0;
}
