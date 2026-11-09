// On-screen display for VR: a small RGBA image (premultiplied alpha) drawn on the CPU with a built-in
// 5x7 bitmap font, shown as its own quad layer under the video. mpv's own OSD can't be used: it would
// be drawn over the side-by-side frame and end up split between the two eyes.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct OsdFont {
  // 5x7 glyphs, one string of 35 chars ('1' = pixel) per character
  static const char *glyph(char c) {
    switch (c) {
    case '0': return "01110100011001110101110011000101110";
    case '1': return "00100011000010000100001000010001110";
    case '2': return "01110100010000100010001000100011111";
    case '3': return "11111000100010000010000011000101110";
    case '4': return "00010001100101010010111110001000010";
    case '5': return "11111100001111000001000011000101110";
    case '6': return "00110010001000011110100011000101110";
    case '7': return "11111000010001000100010000100001000";
    case '8': return "01110100011000101110100011000101110";
    case '9': return "01110100011000101111000010001001100";
    case ':': return "00000011000110000000011000110000000";
    case '/': return "00000000010001000100010001000000000";
    case '.': return "00000000000000000000000000110001100";
    case '-': return "00000000000000011111000000000000000";
    case '+': return "00000001000010011111001000010000000";
    case '%': return "11000110010001000100010001001100011";
    case '<': return "00010001000100010000010000010000010";
    case '>': return "01000001000001000001000100010001000";
    case 'A': return "01110100011000111111100011000110001";
    case 'B': return "11110100011000111110100011000111110";
    case 'C': return "01110100011000010000100001000101110";
    case 'D': return "11110100011000110001100011000111110";
    case 'E': return "11111100001000011110100001000011111";
    case 'F': return "11111100001000011110100001000010000";
    case 'G': return "01110100011000010111100011000101111";
    case 'H': return "10001100011000111111100011000110001";
    case 'I': return "01110001000010000100001000010001110";
    case 'J': return "00111000100001000010000101001001100";
    case 'K': return "10001100101010011000101001001010001";
    case 'L': return "10000100001000010000100001000011111";
    case 'M': return "10001110111010110101100011000110001";
    case 'N': return "10001100011100110101100111000110001";
    case 'O': return "01110100011000110001100011000101110";
    case 'P': return "11110100011000111110100001000010000";
    case 'Q': return "01110100011000110001101011001001101";
    case 'R': return "11110100011000111110101001001010001";
    case 'S': return "01111100001000001110000010000111110";
    case 'T': return "11111001000010000100001000010000100";
    case 'U': return "10001100011000110001100011000101110";
    case 'V': return "10001100011000110001100010101000100";
    case 'W': return "10001100011000110101101011010101010";
    case 'X': return "10001100010101000100010101000110001";
    case 'Y': return "10001100011000101010001000010000100";
    case 'Z': return "11111000010001000100010001000011111";
    default: return nullptr;
    }
  }
};

class Osd {
public:
  static const int W = 1024, H = 256;
  std::vector<uint8_t> px = std::vector<uint8_t>(W * H * 4);

  // text1: message line (may be empty), text2: time line, progress 0..1 (<0: no bar), paused icon
  void draw(const std::string &text1, const std::string &text2, double progress, bool paused) {
    memset(px.data(), 0, px.size());
    roundRect(8, 8, W - 16, H - 16, 28, 0, 0, 0, 0.72f);
    int y = 34;
    if (!text1.empty()) {
      text(text1, W / 2, y, 5, 1.0f);
      y += 7 * 5 + 22;
    } else {
      y += 18;
    }
    text(text2, W / 2, y, 6, 1.0f);
    if (paused) {  // pause icon on the left
      rect(48, y, 12, 42, 1, 1, 1, 1);
      rect(70, y, 12, 42, 1, 1, 1, 1);
    }
    if (progress >= 0) {
      int bx = 48, bw = W - 96, by = H - 58, bh = 14;
      roundRect(bx, by, bw, bh, 7, 0.35f, 0.35f, 0.35f, 1);
      int fw = (int)(bw * (progress > 1 ? 1 : progress));
      if (fw > 0) roundRect(bx, by, fw < bh ? bh : fw, bh, 7, 0.95f, 0.95f, 0.95f, 1);
    }
  }

private:
  void blend(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t *p = &px[(y * W + x) * 4];
    // premultiplied "over"
    float ia = 1 - a;
    p[0] = (uint8_t)(r * a * 255 + p[0] * ia);
    p[1] = (uint8_t)(g * a * 255 + p[1] * ia);
    p[2] = (uint8_t)(b * a * 255 + p[2] * ia);
    p[3] = (uint8_t)(a * 255 + p[3] * ia);
  }
  void rect(int x, int y, int w, int h, float r, float g, float b, float a) {
    for (int j = y; j < y + h; j++)
      for (int i = x; i < x + w; i++) blend(i, j, r, g, b, a);
  }
  void roundRect(int x, int y, int w, int h, int rad, float r, float g, float b, float a) {
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    for (int j = y; j < y + h; j++)
      for (int i = x; i < x + w; i++) {
        int cx = i < x + rad ? x + rad : (i >= x + w - rad ? x + w - rad - 1 : i);
        int cy = j < y + rad ? y + rad : (j >= y + h - rad ? y + h - rad - 1 : j);
        float dx = (float)(i - cx), dy = (float)(j - cy);
        float d = dx * dx + dy * dy, r2 = (float)rad * rad;
        if (d > r2) continue;
        float edge = (r2 - d) / (2.0f * rad);  // ~1 px antialiasing
        blend(i, j, r, g, b, a * (edge < 1 ? edge : 1));
      }
  }
  void text(const std::string &s, int cx, int y, int scale, float a) {
    int adv = 6 * scale;
    int w = (int)s.size() * adv - scale;
    int x = cx - w / 2;
    for (char c : s) {
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      const char *g = OsdFont::glyph(c);
      if (g) {
        for (int gy = 0; gy < 7; gy++)
          for (int gx = 0; gx < 5; gx++)
            if (g[gy * 5 + gx] == '1') rect(x + gx * scale, y + gy * scale, scale, scale, 1, 1, 1, a);
      }
      x += adv;
    }
  }
};
