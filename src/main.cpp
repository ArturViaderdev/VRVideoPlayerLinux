// vrvideo - 3D video player for VR headsets (OpenXR: WiVRn, SteamVR, Monado...).
//
// Decoding, audio, seeking, subtitles filters... are done by libmpv (the same engine as
// 3dvision-play). mpv draws every frame with OpenGL into a texture whose memory is shared with
// Vulkan; the OpenXR side runs on Vulkan (supported by every PC runtime) and shows each eye's half of
// the frame on a virtual screen:
//
//  - "quad" mode (default): two OpenXR quad layers, one visible only to the left eye and one only to the
//    right eye. The runtime samples the video directly onto the screen: one resample, sharpest image.
//  - "render" mode: we draw the screen ourselves into the projection layer (fallback for runtimes that
//    ignore per-eye quad visibility).
//
// VR videos (equirectangular 180 / 360, 2D or 3D side-by-side / top-bottom) are drawn into the
// projection layer by a shader that, for every headset pixel, looks up the direction it sees in the
// video: exact (no sphere mesh, no seams), only the head's rotation counts, native resolution.
//
// mpv renders the frame already scaled to the pixel density the screen has in the headset (its
// high-quality scalers instead of the compositor's bilinear filter): full 1080p, 4K per eye or more.
//
// Copyright (C) 2026 Artur Viader Mataix. GPLv3.

#include "gl.h"
#include "osd.h"

#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------------------------------------------
// small utilities
// ------------------------------------------------------------------------------------------------
static void logf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "vrvideo: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

[[noreturn]] static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "vrvideo: ERROR: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::atomic<bool> g_sigint{false};
static void onSigint(int) { g_sigint = true; }

static std::string fmtTime(double s) {
  if (!(s >= 0)) s = 0;
  long t = (long)s;
  char b[32];
  snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
  return b;
}

static bool spanish() {
  const char *l = getenv("LANGUAGE");
  if (!l || !*l) l = getenv("LC_ALL");
  if (!l || !*l) l = getenv("LC_MESSAGES");
  if (!l || !*l) l = getenv("LANG");
  return l && (strncmp(l, "es", 2) == 0 || strncmp(l, "ca", 2) == 0);
}
static bool g_es = spanish();
static const char *tr(const char *es, const char *en) { return g_es ? es : en; }

// ------------------------------------------------------------------------------------------------
// math (column-major 4x4, OpenXR conventions: right-handed, -Z forward, +Y up)
// ------------------------------------------------------------------------------------------------
struct V3 {
  float x, y, z;
};
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static V3 qrot(XrQuaternionf q, V3 v) {
  V3 u{q.x, q.y, q.z};
  V3 t = cross(u, v) * 2.0f;
  return v + t * q.w + cross(u, t);
}

struct M4 {
  float m[16];
};
static M4 mmul(const M4 &a, const M4 &b) {
  M4 c{};
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[col * 4 + k];
      c.m[col * 4 + row] = s;
    }
  return c;
}
static M4 fromPose(const XrPosef &p, float sx = 1, float sy = 1, float sz = 1) {
  const XrQuaternionf &q = p.orientation;
  float x = q.x, y = q.y, z = q.z, w = q.w;
  M4 r{};
  r.m[0] = (1 - 2 * (y * y + z * z)) * sx;
  r.m[1] = (2 * (x * y + w * z)) * sx;
  r.m[2] = (2 * (x * z - w * y)) * sx;
  r.m[4] = (2 * (x * y - w * z)) * sy;
  r.m[5] = (1 - 2 * (x * x + z * z)) * sy;
  r.m[6] = (2 * (y * z + w * x)) * sy;
  r.m[8] = (2 * (x * z + w * y)) * sz;
  r.m[9] = (2 * (y * z - w * x)) * sz;
  r.m[10] = (1 - 2 * (x * x + y * y)) * sz;
  r.m[12] = p.position.x;
  r.m[13] = p.position.y;
  r.m[14] = p.position.z;
  r.m[15] = 1;
  return r;
}
static M4 viewFromPose(const XrPosef &p) {  // inverse of a rigid transform
  M4 r = fromPose(p), v{};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) v.m[j * 4 + i] = r.m[i * 4 + j];
  for (int i = 0; i < 3; i++)
    v.m[12 + i] = -(v.m[i] * r.m[12] + v.m[4 + i] * r.m[13] + v.m[8 + i] * r.m[14]);
  v.m[15] = 1;
  return v;
}
// flipY: the image is stored top row first (Vulkan order), so GL renders it upside down
static M4 projFromFov(const XrFovf &f, float n, float fa, bool flipY) {
  float l = tanf(f.angleLeft), r = tanf(f.angleRight), u = tanf(f.angleUp), d = tanf(f.angleDown);
  float w = r - l, h = u - d;
  M4 p{};
  p.m[0] = 2 / w;
  p.m[8] = (r + l) / w;
  p.m[5] = 2 / h;
  p.m[9] = (u + d) / h;
  p.m[10] = -(fa + n) / (fa - n);
  p.m[14] = -2 * fa * n / (fa - n);
  p.m[11] = -1;
  if (flipY) {
    p.m[5] = -p.m[5];
    p.m[9] = -p.m[9];
  }
  return p;
}
static XrQuaternionf qaxis(V3 ax, float ang) {
  float s = sinf(ang / 2);
  return {ax.x * s, ax.y * s, ax.z * s, cosf(ang / 2)};
}

static float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

// ------------------------------------------------------------------------------------------------
// configuration
// ------------------------------------------------------------------------------------------------
enum Layout { L_SBS, L_HSBS, L_TAB, L_HTAB, L_MONO };
enum Projection { P_FLAT, P_180, P_360, P_FISHEYE };

struct Config {
  Layout layout = L_SBS;
  bool swap = false;
  float screenW = 3.6f;  // meters
  float dist = 3.0f;     // meters
  float heightOff = 0;   // meters (screen center relative to the eyes)
  bool renderMode = false;
  Projection proj = P_FLAT;  // flat virtual screen, or a VR video around the viewer
  float fisheyeFov = 180;    // degrees covered by a fisheye image (equidistant lens)
  std::string scale = "auto";  // auto | native | <factor of native>
  float supersample = 1.25f;   // auto: pixels rendered per headset pixel covered by the screen
  int maxEye = 4096;           // auto: biggest eye width
  float bg[3] = {0.0f, 0.0f, 0.0f};
  float eyeAspect = 0;  // 0 = from the video
  bool mirror = true;
  bool screenFromArgs = false, distFromArgs = false;
};

static std::string configDir() {
#ifdef _WIN32
  const char *a = getenv("APPDATA");
  return std::string(a ? a : ".") + "\\vrvideoplayer";
#else
  const char *x = getenv("XDG_CONFIG_HOME");
  if (x && *x) return std::string(x) + "/vrvideoplayer";
  const char *h = getenv("HOME");
  return std::string(h ? h : ".") + "/.config/vrvideoplayer";
#endif
}

static void loadScreenConf(Config &c) {
  std::ifstream f(configDir() + "/screen.conf");
  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    float val = (float)atof(v.c_str());
    if (k == "screen_width" && !c.screenFromArgs && val > 0.2f) c.screenW = val;
    if (k == "distance" && !c.distFromArgs && val > 0.2f) c.dist = val;
  }
}
static void saveScreenConf(const Config &c) {
  std::string d = configDir();
#ifdef _WIN32
  CreateDirectoryA(d.c_str(), nullptr);
#else
  std::string parent = d.substr(0, d.rfind('/'));
  mkdir(parent.c_str(), 0755);
  mkdir(d.c_str(), 0755);
#endif
  std::ofstream f(d + "/screen.conf");
  f << "screen_width=" << c.screenW << "\n" << "distance=" << c.dist << "\n";
}

// ------------------------------------------------------------------------------------------------
// OpenXR / Vulkan
// ------------------------------------------------------------------------------------------------
static XrInstance g_inst = XR_NULL_HANDLE;

static void xrCheck(XrResult r, const char *what) {
  if (XR_SUCCEEDED(r)) return;
  char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
  if (g_inst) xrResultToString(g_inst, r, buf);
  die("%s -> %s (%d)", what, buf, (int)r);
}
#define XRC(x) xrCheck((x), #x)
#define VKC(x)                                                     \
  do {                                                             \
    VkResult r_ = (x);                                             \
    if (r_ != VK_SUCCESS) die("%s -> VkResult %d", #x, (int)r_); \
  } while (0)

template <typename T>
static void xrProc(const char *name, T &fn) {
  XRC(xrGetInstanceProcAddr(g_inst, name, (PFN_xrVoidFunction *)&fn));
}

struct Vk {
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  uint32_t qfam = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
#ifdef _WIN32
  PFN_vkGetMemoryWin32HandleKHR getMemHandle = nullptr;
#else
  PFN_vkGetMemoryFdKHR getMemFd = nullptr;
#endif
  uint8_t uuid[VK_UUID_SIZE] = {};

  uint32_t memType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    die("no hay un tipo de memoria Vulkan adecuado");
  }
  void begin() {
    VKC(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(cmd, &bi));
  }
  void submitWait() {
    VKC(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VKC(vkResetFences(dev, 1, &fence));
    VKC(vkQueueSubmit(queue, 1, &si, fence));
    VKC(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
  }
  void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  }
};
static Vk vk;

// An image allocated by Vulkan and imported into OpenGL (GL_EXT_memory_object): OpenGL writes it,
// Vulkan copies it into the OpenXR swapchains. Kept in VK_IMAGE_LAYOUT_GENERAL.
struct SharedImage {
  int w = 0, h = 0;
  VkImage img = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  GLuint glMem = 0, glTex = 0, glFbo = 0;

  void create(int width, int height) {
    w = width;
    h = height;
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
#ifdef _WIN32
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(vk.dev, &ci, nullptr, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, img, &req);
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = img;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = ext.handleTypes;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &exp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk.memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(vk.dev, &ai, nullptr, &mem));
    VKC(vkBindImageMemory(vk.dev, img, mem, 0));

    glCreateMemoryObjectsEXT(1, &glMem);
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(glMem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
#ifdef _WIN32
    VkMemoryGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    hi.memory = mem;
    hi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE hnd = nullptr;
    VKC(vk.getMemHandle(vk.dev, &hi, &hnd));
    glImportMemoryWin32HandleEXT(glMem, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hnd);
#else
    VkMemoryGetFdInfoKHR fi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    fi.memory = mem;
    fi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    VKC(vk.getMemFd(vk.dev, &fi, &fd));
    glImportMemoryFdEXT(glMem, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);  // GL owns the fd now
#endif
    glGenTextures(1, &glTex);
    glBindTexture(GL_TEXTURE_2D, glTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, w, h, glMem, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &glFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, glFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      die("no se pudo usar la memoria compartida Vulkan/OpenGL (framebuffer incompleto)");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // clear to black and move to GENERAL once
    glBindFramebuffer(GL_FRAMEBUFFER, glFbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();
    vk.begin();
    vk.barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_MEMORY_READ_BIT);
    vk.submitWait();
  }
  void destroy() {
    if (glFbo) glDeleteFramebuffers(1, &glFbo);
    if (glTex) glDeleteTextures(1, &glTex);
    if (glMem) glDeleteMemoryObjectsEXT(1, &glMem);
    if (img) vkDestroyImage(vk.dev, img, nullptr);
    if (mem) vkFreeMemory(vk.dev, mem, nullptr);
    *this = SharedImage();
  }
};

struct Swapchain {
  XrSwapchain sc = XR_NULL_HANDLE;
  int w = 0, h = 0;
  std::vector<XrSwapchainImageVulkan2KHR> imgs;
  bool released = false;  // at least one image was released (may be used in a layer)
};

// ------------------------------------------------------------------------------------------------
// the player
// ------------------------------------------------------------------------------------------------
class App {
public:
  Config cfg;
  std::vector<std::pair<std::string, std::string>> mpvOpts;
  std::vector<std::string> files;

  int run();

private:
  // window / GL
  GLFWwindow *win = nullptr;
  GLuint prog = 0, vao = 0;
  GLint uMvp = -1, uUv = -1, uTex = -1;
  GLuint sphProg = 0;
  GLint sRot = -1, sTans = -1, sUv = -1, sHalf = -1, sProj = -1, sBg = -1, sTex = -1;
  GLuint mpvTex = 0, mpvFbo = 0;
  int fw = 0, fh = 0;  // mpvTex size (frame as mpv renders it)
  int tw = 0, th = 0;  // eye size
  // xr
  XrSystemId sys = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool running = false, quitting = false, exitRequested = false;
  double quitAt = 0;
  std::vector<XrViewConfigurationView> viewCfg;
  int64_t colorFormat = 0;
  Swapchain proj[2], video, osdSc;
  SharedImage videoImg, eyeImg[2], osdImg;
  XrView views[2];
  bool viewsValid = false;
  // input
  XrActionSet actionSet = XR_NULL_HANDLE;
  XrAction aPlay = XR_NULL_HANDLE, aStick = XR_NULL_HANDLE, aGrip = XR_NULL_HANDLE, aRecenter = XR_NULL_HANDLE,
           aAudio = XR_NULL_HANDLE, aOsd = XR_NULL_HANDLE, aMenu = XR_NULL_HANDLE;
  bool hasPico = false;
  int stickDir = 0;
  double stickNext = 0, recenterDown = -1, menuDown = -1, lastFrameT = 0;
  // screen
  XrPosef screen{{0, 0, 0, 1}, {0, 0, -3}};
  bool centered = false, recenterPending = false, recenterFull = false, screenChanged = false;
  double sizeChangedAt = 0;
  // mpv
  mpv_handle *mpv = nullptr;
  mpv_render_context *rctx = nullptr;
  bool mpvDead = false, videoReady = false, forceRender = false, paused = false;
  int dw = 0, dh = 0;  // video display size
  double timeBase = 0, durationOverride = -1;  // set by vr-bd (Blu-ray: decoder restarts)
  // osd
  Osd osd;
  bool osdPinned = false, osdDirty = true, osdShown = false;
  double osdUntil = 0, msgUntil = 0, osdLastDraw = 0;
  std::string msg, osdKey;

  void initWindow();
  void initXr();
  void initVulkan();
  void initSession();
  void initActions();
  void initMpv();
  void loadFiles();
  void createSwapchain(Swapchain &s, int w, int h, int64_t fmt);
  void destroySwapchain(Swapchain &s);
  void pollXrEvents();
  void pollMpvEvents();
  void handleInput(XrTime t);
  void recenter(XrTime t, bool full);
  void eyeSize(int &ew, int &eh, bool native);
  void updateTargetSize();
  void resizeVideo(int ew, int eh);
  bool renderMpv();
  void arrangeVideo();
  void renderEyes();
  void renderSphere();
  bool updateOsd();
  void frame();
  void drawMirror();
  void mpvKey(const char *k);
  void mpvCmd(std::initializer_list<const char *> args);
  void showMsg(const std::string &m, double secs = 2.5);
  void cleanup();
  void eyeRect(int eye, int &x, int &y) const;
  static void keyCb(GLFWwindow *w, int key, int sc, int action, int mods);
  static void charCb(GLFWwindow *w, unsigned int cp);
};

static App *g_app = nullptr;

// ---- window + OpenGL ---------------------------------------------------------------------------
void App::initWindow() {
  glfwSetErrorCallback([](int c, const char *d) { logf("GLFW %d: %s", c, d); });
  if (!glfwInit()) die("no se pudo iniciar GLFW (hace falta un escritorio X11/Wayland)");
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, cfg.mirror ? GLFW_TRUE : GLFW_FALSE);
  win = glfwCreateWindow(960, 540, "VR Video Player", nullptr, nullptr);
  if (!win) die("no se pudo crear la ventana OpenGL 4.3");
  glfwMakeContextCurrent(win);
  glfwSwapInterval(0);
  if (!vrgl_load()) die("el driver OpenGL no tiene lo necesario (OpenGL 4.3 + GL_EXT_memory_object)");
#ifdef _WIN32
  const char *need[] = {"GL_EXT_memory_object", "GL_EXT_memory_object_win32"};
#else
  const char *need[] = {"GL_EXT_memory_object", "GL_EXT_memory_object_fd"};
#endif
  for (const char *e : need)
    if (!vrgl_has_extension(e)) die("el driver OpenGL no tiene %s (necesario para compartir la imagen con Vulkan)", e);
  logf("OpenGL: %s", (const char *)glGetString(GL_RENDERER));
  glfwSetWindowUserPointer(win, this);
  glfwSetKeyCallback(win, keyCb);
  glfwSetCharCallback(win, charCb);

  static const char *vs = R"(#version 330 core
uniform mat4 mvp;
uniform vec4 uvr;   // u0 v0 u1 v1 (v0 = top row of the eye in the texture)
out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID & 1) != 0 ? 0.5 : -0.5, (gl_VertexID & 2) != 0 ? 0.5 : -0.5);
  uv = vec2(mix(uvr.x, uvr.z, p.x + 0.5), mix(uvr.w, uvr.y, p.y + 0.5));
  gl_Position = mvp * vec4(p, 0.0, 1.0);
})";
  static const char *fs = R"(#version 330 core
uniform sampler2D tex;
in vec2 uv;
out vec4 color;
void main() { color = vec4(texture(tex, uv).rgb, 1.0); })";
  prog = vrgl_program(vs, fs);
  if (!prog) die("shader");
  uMvp = glGetUniformLocation(prog, "mvp");
  uUv = glGetUniformLocation(prog, "uvr");
  uTex = glGetUniformLocation(prog, "tex");

  // VR video: one full-screen triangle per eye; each pixel -> view direction -> longitude/latitude
  static const char *svs = R"(#version 330 core
out vec2 sv;
void main() {
  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  sv = p * 0.5 + 0.5;   // sv.y = 0 is the first stored row = top of the eye image
  gl_Position = vec4(p, 0.0, 1.0);
})";
  static const char *sfs = R"(#version 330 core
uniform sampler2D tex;
uniform mat4 rot;    // eye orientation relative to the video's front
uniform vec4 tans;   // tan of the fov: left right up down
uniform vec4 uvr;    // eye rectangle in the texture: u0 v0 u1 v1 (v0 = top)
uniform vec4 halfpx; // half texel (x, y), fisheye: half the lens field of view in radians (z)
uniform vec4 bg;
uniform int proj;    // 1 = 180, 2 = 360, 3 = fisheye
in vec2 sv;
out vec4 color;
const float PI = 3.14159265358979;
void main() {
  vec3 d = normalize(vec3(mix(tans.x, tans.y, sv.x), mix(tans.z, tans.w, sv.y), -1.0));
  d = normalize((rot * vec4(d, 0.0)).xyz);
  float lon = atan(d.x, -d.z);                 // 0 = front, + = right
  float lat = asin(clamp(d.y, -1.0, 1.0));     // + = up
  float u, v;
  if (proj == 3) {
    // equidistant fisheye: distance from the image center proportional to the angle from the front
    float th = acos(clamp(-d.z, -1.0, 1.0));
    if (th > halfpx.z) { color = vec4(bg.rgb, 1.0); return; }
    float r = 0.5 * th / halfpx.z;
    float ph = atan(d.y, d.x);
    u = 0.5 + r * cos(ph);
    v = 0.5 - r * sin(ph);
  } else {
    if (proj == 1) {
      if (abs(lon) > 0.5 * PI) { color = vec4(bg.rgb, 1.0); return; }
      u = 0.5 + lon / PI;
    } else {
      u = 0.5 + lon / (2.0 * PI);
    }
    v = 0.5 - lat / PI;
  }
  vec2 t = vec2(mix(uvr.x, uvr.z, u), mix(uvr.y, uvr.w, v));
  t = clamp(t, uvr.xy + halfpx.xy, uvr.zw - halfpx.xy);  // never bleed into the other eye
  color = vec4(texture(tex, t).rgb, 1.0);
})";
  sphProg = vrgl_program(svs, sfs);
  if (!sphProg) die("shader VR");
  sRot = glGetUniformLocation(sphProg, "rot");
  sTans = glGetUniformLocation(sphProg, "tans");
  sUv = glGetUniformLocation(sphProg, "uvr");
  sHalf = glGetUniformLocation(sphProg, "halfpx");
  sProj = glGetUniformLocation(sphProg, "proj");
  sBg = glGetUniformLocation(sphProg, "bg");
  sTex = glGetUniformLocation(sphProg, "tex");
  glGenVertexArrays(1, &vao);
}

// ---- OpenXR instance / system ------------------------------------------------------------------
void App::initXr() {
  uint32_t n = 0;
  XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
  if (XR_FAILED(r))
    die("no hay runtime OpenXR. Arranca wivrn-server (o SteamVR) o elige uno con XR_RUNTIME_JSON (error %d)", (int)r);
  std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
  XRC(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
  bool hasVk = false;
  for (auto &e : exts) {
    if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) hasVk = true;
    if (!strcmp(e.extensionName, "XR_BD_controller_interaction")) hasPico = true;
  }
  if (!hasVk) die("el runtime OpenXR no tiene XR_KHR_vulkan_enable2");
  std::vector<const char *> en = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  if (hasPico) en.push_back("XR_BD_controller_interaction");
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy(ci.applicationInfo.applicationName, "VR Video Player");
  strcpy(ci.applicationInfo.engineName, "vrvideo");
  ci.applicationInfo.applicationVersion = 1;
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = (uint32_t)en.size();
  ci.enabledExtensionNames = en.data();
  r = xrCreateInstance(&ci, &g_inst);
  if (XR_FAILED(r))
    die("xrCreateInstance fallo (%d). Esta en marcha wivrn-server / SteamVR?", (int)r);
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(g_inst, &ip);
  logf("runtime OpenXR: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
       XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

  XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
  gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  bool told = false;
  for (;;) {
    r = xrGetSystem(g_inst, &gi, &sys);
    if (XR_SUCCEEDED(r)) break;
    if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) xrCheck(r, "xrGetSystem");
    if (!told) logf("%s", tr("esperando a que se conecte el visor...", "waiting for the headset to connect..."));
    told = true;
    glfwPollEvents();
    if (g_sigint || glfwWindowShouldClose(win)) exit(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  xrGetSystemProperties(g_inst, sys, &sp);
  logf("visor: %s", sp.systemName);
}

// ---- Vulkan through XR_KHR_vulkan_enable2 ------------------------------------------------------
void App::initVulkan() {
  PFN_xrGetVulkanGraphicsRequirements2KHR getReq = nullptr;
  PFN_xrCreateVulkanInstanceKHR createInst = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR getDev = nullptr;
  PFN_xrCreateVulkanDeviceKHR createDev = nullptr;
  xrProc("xrGetVulkanGraphicsRequirements2KHR", getReq);
  xrProc("xrCreateVulkanInstanceKHR", createInst);
  xrProc("xrGetVulkanGraphicsDevice2KHR", getDev);
  xrProc("xrCreateVulkanDeviceKHR", createDev);
  XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  XRC(getReq(g_inst, sys, &req));

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "vrvideo";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  XrVulkanInstanceCreateInfoKHR xci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xci.systemId = sys;
  xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  xci.vulkanCreateInfo = &ici;
  VkResult vr = VK_SUCCESS;
  XRC(createInst(g_inst, &xci, &vk.inst, &vr));
  VKC(vr);
  XrVulkanGraphicsDeviceGetInfoKHR dgi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  dgi.systemId = sys;
  dgi.vulkanInstance = vk.inst;
  XRC(getDev(g_inst, &dgi, &vk.phys));

  VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  pp.pNext = &idp;
  vkGetPhysicalDeviceProperties2(vk.phys, &pp);
  memcpy(vk.uuid, idp.deviceUUID, VK_UUID_SIZE);
  logf("Vulkan: %s", pp.properties.deviceName);
  GLubyte gluuid[16] = {};
  glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, gluuid);
  if (memcmp(gluuid, vk.uuid, 16) != 0)
    logf("AVISO: OpenGL y Vulkan parecen usar GPUs distintas; si la imagen no se ve, usa la misma GPU para los dos");

  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, qf.data());
  vk.qfam = UINT32_MAX;
  for (uint32_t i = 0; i < nq; i++)
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      vk.qfam = i;
      break;
    }
  if (vk.qfam == UINT32_MAX) die("Vulkan sin cola grafica");
  float prio = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = vk.qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
#ifdef _WIN32
  const char *dexts[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
#else
  const char *dexts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
#endif
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dexts;
  XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xdci.systemId = sys;
  xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  xdci.vulkanPhysicalDevice = vk.phys;
  xdci.vulkanCreateInfo = &dci;
  XRC(createDev(g_inst, &xdci, &vk.dev, &vr));
  VKC(vr);
  vkGetDeviceQueue(vk.dev, vk.qfam, 0, &vk.queue);
#ifdef _WIN32
  vk.getMemHandle = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(vk.dev, "vkGetMemoryWin32HandleKHR");
  if (!vk.getMemHandle) die("falta vkGetMemoryWin32HandleKHR");
#else
  vk.getMemFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk.dev, "vkGetMemoryFdKHR");
  if (!vk.getMemFd) die("falta vkGetMemoryFdKHR");
#endif
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = vk.qfam;
  VKC(vkCreateCommandPool(vk.dev, &pci, nullptr, &vk.pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = vk.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VKC(vkAllocateCommandBuffers(vk.dev, &cai, &vk.cmd));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VKC(vkCreateFence(vk.dev, &fci, nullptr, &vk.fence));
}

// ---- session, spaces, swapchains -----------------------------------------------------------------
void App::createSwapchain(Swapchain &s, int w, int h, int64_t fmt) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = fmt;
  ci.sampleCount = 1;
  ci.width = (uint32_t)w;
  ci.height = (uint32_t)h;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  XRC(xrCreateSwapchain(session, &ci, &s.sc));
  uint32_t n = 0;
  XRC(xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr));
  s.imgs.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  XRC(xrEnumerateSwapchainImages(s.sc, n, &n, (XrSwapchainImageBaseHeader *)s.imgs.data()));
  s.w = w;
  s.h = h;
  s.released = false;
}
void App::destroySwapchain(Swapchain &s) {
  if (s.sc) xrDestroySwapchain(s.sc);
  s = Swapchain();
}

void App::initSession() {
  XrGraphicsBindingVulkan2KHR bind{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  bind.instance = vk.inst;
  bind.physicalDevice = vk.phys;
  bind.device = vk.dev;
  bind.queueFamilyIndex = vk.qfam;
  bind.queueIndex = 0;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &bind;
  sci.systemId = sys;
  XRC(xrCreateSession(g_inst, &sci, &session));

  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XRC(xrCreateReferenceSpace(session, &rs, &local));
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  XRC(xrCreateReferenceSpace(session, &rs, &viewSpace));

  uint32_t n = 0;
  XRC(xrEnumerateViewConfigurationViews(g_inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
  viewCfg.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XRC(xrEnumerateViewConfigurationViews(g_inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                        viewCfg.data()));
  if (n != 2) die("se esperaban 2 vistas (estereo), hay %u", n);
  logf("resolucion por ojo del visor: %ux%u", viewCfg[0].recommendedImageRectWidth,
       viewCfg[0].recommendedImageRectHeight);

  XRC(xrEnumerateSwapchainFormats(session, 0, &n, nullptr));
  std::vector<int64_t> fmts(n);
  XRC(xrEnumerateSwapchainFormats(session, n, &n, fmts.data()));
  // raw copies need the same byte order as our RGBA8 images: R8G8B8A8, sRGB preferred (mpv's output
  // is sRGB-encoded, an sRGB swapchain makes the compositor decode it correctly)
  for (int64_t want : {(int64_t)VK_FORMAT_R8G8B8A8_SRGB, (int64_t)VK_FORMAT_R8G8B8A8_UNORM})
    if (!colorFormat && std::find(fmts.begin(), fmts.end(), want) != fmts.end()) colorFormat = want;
  if (!colorFormat) die("el runtime no ofrece swapchains R8G8B8A8");
  if (colorFormat != VK_FORMAT_R8G8B8A8_SRGB) logf("AVISO: sin formato sRGB, los colores pueden verse lavados");

  for (int i = 0; i < 2; i++) {
    int w = (int)viewCfg[i].recommendedImageRectWidth, h = (int)viewCfg[i].recommendedImageRectHeight;
    if (!cfg.renderMode) {  // quad mode: the projection layer is only a plain background
      w = std::min(w, 512);
      h = std::min(h, 512);
    }
    createSwapchain(proj[i], w, h, colorFormat);
    eyeImg[i].create(w, h);
  }
  createSwapchain(osdSc, Osd::W, Osd::H, colorFormat);
  osdImg.create(Osd::W, Osd::H);
  initActions();
}

// ---- controllers ---------------------------------------------------------------------------------
void App::initActions() {
  XrActionSetCreateInfo ai{XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(ai.actionSetName, "player");
  strcpy(ai.localizedActionSetName, "Player");
  XRC(xrCreateActionSet(g_inst, &ai, &actionSet));
  auto mk = [&](XrAction &a, const char *name, const char *loc, XrActionType t) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, loc);
    ci.actionType = t;
    XRC(xrCreateAction(actionSet, &ci, &a));
  };
  mk(aPlay, "play_pause", "Play / pause", XR_ACTION_TYPE_BOOLEAN_INPUT);
  mk(aStick, "seek", "Seek / screen size", XR_ACTION_TYPE_VECTOR2F_INPUT);
  mk(aGrip, "adjust", "Adjust screen (hold)", XR_ACTION_TYPE_BOOLEAN_INPUT);
  mk(aRecenter, "recenter", "Recenter screen", XR_ACTION_TYPE_BOOLEAN_INPUT);
  mk(aAudio, "audio", "Next audio track", XR_ACTION_TYPE_BOOLEAN_INPUT);
  mk(aOsd, "osd", "Show / hide info", XR_ACTION_TYPE_BOOLEAN_INPUT);
  mk(aMenu, "menu", "Info (hold: quit)", XR_ACTION_TYPE_BOOLEAN_INPUT);

  auto path = [&](const char *s) {
    XrPath p;
    XRC(xrStringToPath(g_inst, s, &p));
    return p;
  };
  auto suggest = [&](const char *profile, std::vector<std::pair<XrAction, const char *>> b) {
    std::vector<XrActionSuggestedBinding> sb;
    for (auto &x : b) sb.push_back({x.first, path(x.second)});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = path(profile);
    s.suggestedBindings = sb.data();
    s.countSuggestedBindings = (uint32_t)sb.size();
    XrResult r = xrSuggestInteractionProfileBindings(g_inst, &s);
    if (XR_FAILED(r)) logf("perfil de mando %s no aceptado (%d)", profile, (int)r);
  };
  const char *L = "/user/hand/left/input/", *R = "/user/hand/right/input/";
  std::vector<std::string> keep;  // storage for the concatenated paths
  keep.reserve(64);
  auto p2 = [&](const char *hand, const char *in) {
    keep.push_back(std::string(hand) + in);
    return keep.back().c_str();
  };
  // Oculus Touch layout (also Pico 4 and most controllers emulate it)
  std::vector<std::pair<XrAction, const char *>> touch = {
      {aPlay, p2(R, "trigger/value")},       {aPlay, p2(L, "trigger/value")},
      {aStick, p2(R, "thumbstick")},          {aStick, p2(L, "thumbstick")},
      {aGrip, p2(R, "squeeze/value")},        {aGrip, p2(L, "squeeze/value")},
      {aRecenter, p2(R, "thumbstick/click")}, {aRecenter, p2(L, "thumbstick/click")},
      {aAudio, p2(R, "b/click")},             {aAudio, p2(L, "y/click")},
      {aOsd, p2(R, "a/click")},               {aOsd, p2(L, "x/click")},
      {aMenu, p2(L, "menu/click")}};
  suggest("/interaction_profiles/oculus/touch_controller", touch);
  if (hasPico) suggest("/interaction_profiles/bytedance/pico4_controller", touch);
  suggest("/interaction_profiles/valve/index_controller",
          {{aPlay, p2(R, "trigger/value")},
           {aPlay, p2(L, "trigger/value")},
           {aStick, p2(R, "thumbstick")},
           {aStick, p2(L, "thumbstick")},
           {aGrip, p2(R, "squeeze/value")},
           {aGrip, p2(L, "squeeze/value")},
           {aRecenter, p2(R, "thumbstick/click")},
           {aRecenter, p2(L, "thumbstick/click")},
           {aAudio, p2(R, "b/click")},
           {aAudio, p2(L, "b/click")},
           {aOsd, p2(R, "a/click")},
           {aOsd, p2(L, "a/click")}});
  suggest("/interaction_profiles/htc/vive_controller",
          {{aPlay, p2(R, "trigger/click")},
           {aPlay, p2(L, "trigger/click")},
           {aStick, p2(R, "trackpad")},
           {aStick, p2(L, "trackpad")},
           {aGrip, p2(R, "squeeze/click")},
           {aGrip, p2(L, "squeeze/click")},
           {aMenu, p2(R, "menu/click")},
           {aMenu, p2(L, "menu/click")}});
  suggest("/interaction_profiles/khr/simple_controller",
          {{aPlay, p2(R, "select/click")},
           {aPlay, p2(L, "select/click")},
           {aMenu, p2(R, "menu/click")},
           {aMenu, p2(L, "menu/click")}});
  XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  at.countActionSets = 1;
  at.actionSets = &actionSet;
  XRC(xrAttachSessionActionSets(session, &at));
}

static bool boolState(XrSession s, XrAction a, bool *changed = nullptr) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  gi.action = a;
  XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
  if (XR_FAILED(xrGetActionStateBoolean(s, &gi, &st)) || !st.isActive) {
    if (changed) *changed = false;
    return false;
  }
  if (changed) *changed = st.changedSinceLastSync;
  return st.currentState;
}

void App::handleInput(XrTime t) {
  XrActiveActionSet aas{actionSet, XR_NULL_PATH};
  XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
  si.countActiveActionSets = 1;
  si.activeActionSets = &aas;
  if (xrSyncActions(session, &si) != XR_SUCCESS) return;  // not focused
  double now = nowSec();
  double dt = lastFrameT > 0 ? std::min(0.1, now - lastFrameT) : 0;
  lastFrameT = now;
  bool ch;

  if (boolState(session, aPlay, &ch) && ch) {
    mpvCmd({"cycle", "pause"});
    osdUntil = now + 3;
  }
  if (boolState(session, aAudio, &ch) && ch) {
    mpvKey("SHARP");
    osdUntil = now + 3;
  }
  if (boolState(session, aOsd, &ch) && ch) {
    osdPinned = !osdPinned;
    osdUntil = 0;
    osdDirty = true;
  }
  bool rc = boolState(session, aRecenter, &ch);
  if (ch && rc) recenterDown = now;
  if (ch && !rc && recenterDown > 0) {
    recenter(t, now - recenterDown > 0.8);  // long press: also follow the head's pitch (lying down)
    recenterDown = -1;
  }
  bool mn = boolState(session, aMenu, &ch);
  if (ch && mn) menuDown = now;
  if (mn && menuDown > 0 && now - menuDown > 1.5) {
    menuDown = -1;
    logf("%s", tr("salir (boton menu)", "quit (menu button)"));
    mpvCmd({"quit"});
  }
  if (ch && !mn && menuDown > 0) {
    menuDown = -1;
    osdPinned = !osdPinned;
    osdUntil = 0;
    osdDirty = true;
  }

  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  gi.action = aStick;
  XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_FAILED(xrGetActionStateVector2f(session, &gi, &st)) || !st.isActive) return;
  float x = st.currentState.x, y = st.currentState.y;
  if (cfg.proj == P_FLAT && boolState(session, aGrip)) {
    // hold grip: stick up/down = screen size, left/right = distance
    float dz = 0.15f;
    bool changed = false;
    if (fabsf(y) > dz) {
      cfg.screenW = std::clamp(cfg.screenW * expf(y * (float)dt * 0.8f), 0.3f, 40.0f);
      changed = true;
    }
    if (fabsf(x) > dz) {
      float nd = std::clamp(cfg.dist + x * (float)dt * 1.5f, 0.4f, 30.0f);
      // keep the screen where it is, only moving it along the view direction
      V3 fwd = qrot(screen.orientation, {0, 0, -1});
      V3 delta = fwd * (nd - cfg.dist);
      screen.position.x += delta.x;
      screen.position.y += delta.y;
      screen.position.z += delta.z;
      cfg.dist = nd;
      changed = true;
    }
    if (changed) {
      char b[96];
      snprintf(b, sizeof b, "%s %.1f M  %s %.1f M", tr("PANTALLA", "SCREEN"), cfg.screenW,
               tr("DISTANCIA", "DISTANCE"), cfg.dist);
      showMsg(b, 1.5);
      screenChanged = true;
      sizeChangedAt = now;
    }
    stickDir = 0;
    return;
  }
  // flick the stick: right/left = +/-10 s, up/down = +/-1 min (held: repeats)
  int dir = 0;
  if (fabsf(x) > 0.7f || fabsf(y) > 0.7f) dir = fabsf(x) > fabsf(y) ? (x > 0 ? 1 : 2) : (y > 0 ? 3 : 4);
  if (fabsf(x) < 0.4f && fabsf(y) < 0.4f) stickDir = 0;
  static const char *keys[] = {"", "RIGHT", "LEFT", "UP", "DOWN"};
  if (dir && dir != stickDir) {
    mpvKey(keys[dir]);
    stickDir = dir;
    stickNext = now + 0.6;
    osdUntil = now + 3;
  } else if (dir && dir == stickDir && now >= stickNext) {
    mpvKey(keys[dir]);
    stickNext = now + 0.35;
    osdUntil = now + 3;
  }
}

void App::recenter(XrTime t, bool full) {
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(viewSpace, local, t, &loc))) return;
  if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
      !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    return;
  V3 f = qrot(loc.pose.orientation, {0, 0, -1});
  float yaw = atan2f(-f.x, -f.z);
  XrQuaternionf q = qaxis({0, 1, 0}, yaw);
  V3 dir{-sinf(yaw), 0, -cosf(yaw)};
  if (full) {
    float pitch = asinf(std::clamp(f.y, -1.0f, 1.0f));
    q = qmul(q, qaxis({1, 0, 0}, pitch));
    dir = qrot(q, {0, 0, -1});
  }
  V3 p{loc.pose.position.x, loc.pose.position.y + cfg.heightOff, loc.pose.position.z};
  V3 c = p + dir * cfg.dist;
  screen.orientation = q;
  screen.position = {c.x, c.y, c.z};
  centered = true;
  osdUntil = nowSec() + 2;
}

// ---- mpv -------------------------------------------------------------------------------------------
static void *mpvGetProc(void *, const char *name) { return (void *)glfwGetProcAddress(name); }

void App::initMpv() {
  mpv = mpv_create();
  if (!mpv) die("mpv_create");
  auto set = [&](const char *k, const char *v) {
    int e = mpv_set_option_string(mpv, k, v);
    if (e < 0) logf("opcion mpv --%s=%s: %s", k, v, mpv_error_string(e));
  };
  set("vo", "libmpv");
  set("terminal", "yes");
  set("msg-level", "all=status");
  set("input-default-bindings", "yes");
  set("input-terminal", "yes");
  set("hwdec", "auto-safe");
  set("keepaspect", "no");  // we give mpv the exact eye size
  set("osd-level", "0");
  set("osd-bar", "no");
  set("osc", "no");
  set("sub-visibility", "no");  // would be split between the eyes (vr-play burns them into each eye)
  set("audio-display", "no");
  set("correct-downscaling", "yes");
  set("ytdl", "yes");
  set("idle", "once");  // quit at the end of the file
  std::string scripts;
  std::vector<std::pair<std::string, std::string>> appends;  // --xxx-append: list options, after init
  for (auto &o : mpvOpts) {
    if (o.first.size() > 7 && o.first.compare(o.first.size() - 7, 7, "-append") == 0) {
      appends.push_back({o.first.substr(0, o.first.size() - 7), o.second});
      continue;
    }
    if (o.first == "script" || o.first == "scripts") {
      scripts += (scripts.empty() ? "" : ":") + o.second;
      continue;
    }
    set(o.first.c_str(), o.second.c_str());
  }
  if (!scripts.empty()) set("scripts", scripts.c_str());
  if (mpv_initialize(mpv) < 0) die("mpv_initialize");
  for (auto &ap : appends) {
    const char *c[] = {"change-list", ap.first.c_str(), "append", ap.second.c_str(), nullptr};
    int e = mpv_command(mpv, c);
    if (e < 0) logf("opcion mpv --%s-append=%s: %s", ap.first.c_str(), ap.second.c_str(), mpv_error_string(e));
  }

  mpv_opengl_init_params gp{mpvGetProc, nullptr};
  int adv = 0;
  mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
                               {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gp},
                               {MPV_RENDER_PARAM_ADVANCED_CONTROL, &adv},
                               {MPV_RENDER_PARAM_INVALID, nullptr}};
  if (mpv_render_context_create(&rctx, mpv, params) < 0) die("mpv_render_context_create");
  mpv_render_context_set_update_callback(rctx, [](void *) {}, nullptr);

  // seek keys for VR: 10 s / 1 min (a Blu-ray script may override them with its own forced keys)
  const char *cmd[] = {"define-section", "vrvideo",
                       "RIGHT seek 10\nLEFT seek -10\nUP seek 60\nDOWN seek -60\n"
                       "Shift+RIGHT seek 5\nShift+LEFT seek -5\nPGUP seek 600\nPGDWN seek -600\n",
                       "default", nullptr};
  mpv_command(mpv, cmd);
  const char *en[] = {"enable-section", "vrvideo", nullptr};
  mpv_command(mpv, en);
  mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
  mpv_observe_property(mpv, 0, "aid", MPV_FORMAT_INT64);
}

void App::loadFiles() {
  for (size_t i = 0; i < files.size(); i++) {
    const char *c[] = {"loadfile", files[i].c_str(), i ? "append" : "replace", nullptr};
    mpv_command(mpv, c);
  }
}

void App::mpvKey(const char *k) {
  if (!mpv || mpvDead) return;
  const char *c[] = {"keypress", k, nullptr};
  mpv_command_async(mpv, 0, c);
}
void App::mpvCmd(std::initializer_list<const char *> args) {
  if (!mpv || mpvDead) return;
  std::vector<const char *> c(args);
  c.push_back(nullptr);
  mpv_command_async(mpv, 0, c.data());
}
void App::showMsg(const std::string &m, double secs) {
  msg = m;
  msgUntil = nowSec() + secs;
  osdUntil = std::max(osdUntil, msgUntil);
  osdDirty = true;
}

void App::pollMpvEvents() {
  while (mpv && !mpvDead) {
    mpv_event *e = mpv_wait_event(mpv, 0);
    if (e->event_id == MPV_EVENT_NONE) break;
    switch (e->event_id) {
    case MPV_EVENT_SHUTDOWN:
      mpvDead = true;
      quitting = true;
      quitAt = nowSec();
      break;
    case MPV_EVENT_VIDEO_RECONFIG: {
      int64_t w = 0, h = 0;
      mpv_get_property(mpv, "dwidth", MPV_FORMAT_INT64, &w);
      mpv_get_property(mpv, "dheight", MPV_FORMAT_INT64, &h);
      if (w > 0 && h > 0 && (w != dw || h != dh)) {
        dw = (int)w;
        dh = (int)h;
        int ew, eh;
        eyeSize(ew, eh, true);
        logf("video %dx%d, ojo %dx%d", dw, dh, ew, eh);
        tw = th = 0;  // force a new size
        updateTargetSize();
      }
      break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
      auto *p = (mpv_event_property *)e->data;
      if (!strcmp(p->name, "pause") && p->format == MPV_FORMAT_FLAG) {
        paused = *(int *)p->data;
        osdDirty = true;
      } else if (!strcmp(p->name, "aid") && p->format == MPV_FORMAT_INT64) {
        char *lang = mpv_get_property_string(mpv, "current-tracks/audio/lang");
        char *title = mpv_get_property_string(mpv, "current-tracks/audio/codec");
        std::string m = std::string("AUDIO ") + std::to_string(*(int64_t *)p->data);
        if (lang) m += std::string(" ") + lang;
        if (title) m += std::string(" ") + title;
        showMsg(m);
        mpv_free(lang);
        mpv_free(title);
      }
      break;
    }
    case MPV_EVENT_CLIENT_MESSAGE: {
      // vr-bd: "script-message vr-osd-base <seconds already played> <total length>"
      auto *m = (mpv_event_client_message *)e->data;
      if (m->num_args >= 3 && !strcmp(m->args[0], "vr-osd-base")) {
        timeBase = atof(m->args[1]);
        durationOverride = atof(m->args[2]);
        osdDirty = true;
      } else if (m->num_args >= 2 && !strcmp(m->args[0], "vr-osd-msg")) {
        showMsg(m->args[1]);
      }
      break;
    }
    default:
      break;
    }
  }
}

// ---- video size: mpv renders each eye at the pixel density the screen has in the headset ----------
void App::eyeSize(int &ew, int &eh, bool native) {
  float aspect;
  int nh;
  switch (cfg.layout) {
  case L_SBS: aspect = (dw / 2.0f) / dh; nh = dh; break;
  case L_HSBS: aspect = (float)dw / dh; nh = dh; break;
  case L_TAB: aspect = dw / (dh / 2.0f); nh = dh / 2; break;
  case L_HTAB: aspect = (float)dw / dh; nh = dh / 2; break;
  default: aspect = (float)dw / dh; nh = dh; break;
  }
  if (cfg.eyeAspect > 0) aspect = cfg.eyeAspect;
  int nw = (int)lroundf(nh * aspect);
  if (cfg.proj != P_FLAT) {
    // VR video: the whole frame surrounds you; the headset's density (~20 px/degree) is about
    // that of an 8K 360 video, so keep the source resolution (up to 8192 px per eye)
    ew = std::min(nw, 8192);
  } else if (native || cfg.scale == "native" || !viewsValid) {
    float f = 1;
    if (!native && cfg.scale != "auto" && cfg.scale != "native") f = (float)atof(cfg.scale.c_str());
    if (!(f > 0.05f)) f = 1;
    ew = (int)(nw * f);
  } else {
    // pixels the screen covers in the headset image (perpendicular plane at distance d)
    const XrFovf &fov = views[0].fov;
    float ppt = viewCfg[0].recommendedImageRectWidth / (tanf(fov.angleRight) - tanf(fov.angleLeft));
    float px = ppt * cfg.screenW / cfg.dist * cfg.supersample;
    ew = (int)std::min<float>(px, (float)cfg.maxEye);
    ew = std::min(ew, nw * 2);  // never more than twice the source
    ew = std::max(ew, 64);
  }
  ew = (ew + 1) & ~1;
  eh = ((int)lroundf(ew / aspect) + 1) & ~1;
  if (eh < 2) eh = 2;
}

void App::updateTargetSize() {
  if (dw <= 0 || dh <= 0) return;
  int ew, eh;
  eyeSize(ew, eh, false);
  // ignore small changes (avoid reallocating while the user is still resizing)
  if (tw > 0 && fabsf((float)ew / tw - 1) < 0.08f) return;
  if (tw > 0 && nowSec() - sizeChangedAt < 0.5) return;
  resizeVideo(ew, eh);
}

void App::resizeVideo(int ew, int eh) {
  tw = ew;
  th = eh;
  switch (cfg.layout) {
  case L_SBS:
  case L_HSBS: fw = 2 * tw; fh = th; break;
  case L_TAB:
  case L_HTAB: fw = tw; fh = 2 * th; break;
  default: fw = tw; fh = th; break;
  }
  logf("render: %dx%d por ojo (%s)", tw, th,
       cfg.proj == P_180 ? "VR 180" : cfg.proj == P_360 ? "VR 360" : cfg.proj == P_FISHEYE ? "VR fisheye"
       : cfg.renderMode ? "render" : "quad");
  if (!cfg.renderMode) {
    videoImg.destroy();
    videoImg.create(2 * tw, th);
    destroySwapchain(video);
    createSwapchain(video, 2 * tw, th, colorFormat);
  }
  if (mpvFbo) glDeleteFramebuffers(1, &mpvFbo);
  if (mpvTex) glDeleteTextures(1, &mpvTex);
  glGenTextures(1, &mpvTex);
  glBindTexture(GL_TEXTURE_2D, mpvTex);
  // mutable storage: some drivers (Mesa) give mpv a black image with glTexStorage2D textures
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &mpvFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, mpvFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mpvTex, 0);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  forceRender = true;
  osdDirty = true;
}

// top-left corner of an eye inside mpvTex (rows are stored top row first)
void App::eyeRect(int eye, int &x, int &y) const {
  if (cfg.swap) eye ^= 1;
  x = y = 0;
  if (cfg.layout == L_SBS || cfg.layout == L_HSBS) x = eye ? tw : 0;
  if (cfg.layout == L_TAB || cfg.layout == L_HTAB) y = eye ? th : 0;
}

bool App::renderMpv() {
  if (!rctx) return false;
  if (!mpvTex) {
    // a frame can be ready before we see MPV_EVENT_VIDEO_RECONFIG: size the textures now, otherwise
    // mpv gives up waiting and drops it (a paused video would stay black)
    int64_t w = 0, h = 0;
    mpv_get_property(mpv, "dwidth", MPV_FORMAT_INT64, &w);
    mpv_get_property(mpv, "dheight", MPV_FORMAT_INT64, &h);
    if (w <= 0 || h <= 0) return false;
    dw = (int)w;
    dh = (int)h;
    updateTargetSize();
    if (!mpvTex) return false;
  }
  uint64_t fl = mpv_render_context_update(rctx);
  if (!(fl & MPV_RENDER_UPDATE_FRAME) && !forceRender) return false;
  forceRender = false;
  mpv_opengl_fbo fbo{(int)mpvFbo, fw, fh, GL_RGBA8};
  int flip = 0, block = 0;
  mpv_render_param p[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                          {MPV_RENDER_PARAM_FLIP_Y, &flip},
                          {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                          {MPV_RENDER_PARAM_INVALID, nullptr}};
  mpv_render_context_render(rctx, p);
  // mpv may leave its own state bound (pixel buffers, framebuffers...): back to defaults
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glActiveTexture(GL_TEXTURE0);
  videoReady = true;
  return true;
}

// quad mode: left eye | right eye, side by side, in the image copied to the video swapchain
void App::arrangeVideo() {
  for (int e = 0; e < 2; e++) {
    int x, y;
    eyeRect(e, x, y);
    glCopyImageSubData(mpvTex, GL_TEXTURE_2D, 0, x, y, 0, videoImg.glTex, GL_TEXTURE_2D, 0, e * tw, 0, 0, tw, th, 1);
  }
}

// render mode: draw the screen into each eye's image
void App::renderEyes() {
  glUseProgram(prog);
  glBindVertexArray(vao);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mpvTex);
  glUniform1i(uTex, 0);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  float aspect = th > 0 ? (float)tw / th : 16.0f / 9;
  M4 model = fromPose(screen, cfg.screenW, cfg.screenW / aspect, 1);
  for (int e = 0; e < 2; e++) {
    glBindFramebuffer(GL_FRAMEBUFFER, eyeImg[e].glFbo);
    glViewport(0, 0, eyeImg[e].w, eyeImg[e].h);
    glClearColor(cfg.bg[0], cfg.bg[1], cfg.bg[2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (videoReady && mpvTex) {
      M4 mvp = mmul(projFromFov(views[e].fov, 0.05f, 100.0f, true), mmul(viewFromPose(views[e].pose), model));
      int x, y;
      eyeRect(e, x, y);
      glUniformMatrix4fv(uMvp, 1, GL_FALSE, mvp.m);
      glUniform4f(uUv, (float)x / fw, (float)y / fh, (float)(x + tw) / fw, (float)(y + th) / fh);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

static M4 rotOnly(const XrQuaternionf &q) { return fromPose({q, {0, 0, 0}}); }
static M4 transpose(const M4 &a) {
  M4 t{};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) t.m[j * 4 + i] = a.m[i * 4 + j];
  return t;
}

void App::renderSphere() {
  glUseProgram(sphProg);
  glBindVertexArray(vao);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mpvTex);
  glUniform1i(sTex, 0);
  glUniform1i(sProj, cfg.proj == P_180 ? 1 : cfg.proj == P_FISHEYE ? 3 : 2);
  glUniform4f(sBg, cfg.bg[0], cfg.bg[1], cfg.bg[2], 1);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FRAMEBUFFER_SRGB);
  // the video's front is the direction the screen was centered on (recenter: stick click)
  M4 front = transpose(rotOnly(screen.orientation));
  for (int e = 0; e < 2; e++) {
    glBindFramebuffer(GL_FRAMEBUFFER, eyeImg[e].glFbo);
    glViewport(0, 0, eyeImg[e].w, eyeImg[e].h);
    glClearColor(cfg.bg[0], cfg.bg[1], cfg.bg[2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!videoReady || !mpvTex) continue;
    M4 rot = mmul(front, rotOnly(views[e].pose.orientation));
    const XrFovf &f = views[e].fov;
    int x, y;
    eyeRect(e, x, y);
    glUniformMatrix4fv(sRot, 1, GL_FALSE, rot.m);
    glUniform4f(sTans, tanf(f.angleLeft), tanf(f.angleRight), tanf(f.angleUp), tanf(f.angleDown));
    glUniform4f(sUv, (float)x / fw, (float)y / fh, (float)(x + tw) / fw, (float)(y + th) / fh);
    glUniform4f(sHalf, 0.5f / fw, 0.5f / fh, cfg.fisheyeFov * 0.5f * 3.14159265f / 180.0f, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

bool App::updateOsd() {
  double now = nowSec();
  bool show = osdPinned || paused || now < osdUntil || !videoReady;
  osdShown = show;
  if (!show || !mpv || mpvDead) return false;
  if (!osdDirty && now - osdLastDraw < 0.25) return false;
  double pos = 0, dur = 0;
  mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
  mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
  pos += timeBase;
  if (durationOverride > 0) dur = durationOverride;
  std::string l1 = now < msgUntil ? msg : (paused ? tr("PAUSA", "PAUSED") : "");
  if (!videoReady) l1 = tr("CARGANDO...", "LOADING...");
  std::string l2 = dur > 0 ? fmtTime(pos) + " / " + fmtTime(dur) : fmtTime(pos);
  std::string key = l1 + "|" + l2 + "|" + std::to_string(paused) + "|" + std::to_string((int)(pos * 4));
  osdLastDraw = now;
  osdDirty = false;
  if (key == osdKey) return false;
  osdKey = key;
  osd.draw(l1, l2, dur > 0 ? pos / dur : -1, paused);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, osdImg.glTex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Osd::W, Osd::H, GL_RGBA, GL_UNSIGNED_BYTE, osd.px.data());
  return true;
}

void App::drawMirror() {
  if (!cfg.mirror || glfwGetWindowAttrib(win, GLFW_ICONIFIED)) return;
  int ww, wh;
  glfwGetFramebufferSize(win, &ww, &wh);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, ww, wh);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (videoReady && mpvTex && tw > 0) {
    int x, y;
    eyeRect(0, x, y);
    float a = (float)tw / th;
    int dwid = ww, dhei = (int)(ww / a);
    if (dhei > wh) {
      dhei = wh;
      dwid = (int)(wh * a);
    }
    int ox = (ww - dwid) / 2, oy = (wh - dhei) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mpvFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // texture rows are top-first: flip into the window
    glBlitFramebuffer(x, y, x + tw, y + th, ox, oy + dhei, ox + dwid, oy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }
  glfwSwapBuffers(win);
}

// ---- XR events ------------------------------------------------------------------------------------
void App::pollXrEvents() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
    switch (ev.type) {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      auto *s = (XrEventDataSessionStateChanged *)&ev;
      state = s->state;
      if (state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XRC(xrBeginSession(session, &bi));
        running = true;
        logf("%s", tr("sesion VR iniciada", "VR session started"));
      } else if (state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(session);
        running = false;
        mpvCmd({"set", "pause", "yes"});  // headset taken off / app hidden: don't keep playing unseen
      } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
        running = false;
        exitRequested = true;
      }
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      exitRequested = true;
      break;
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      recenterPending = true;  // the user recentered in the headset's own menu
      break;
    default:
      break;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

// ---- one frame ----------------------------------------------------------------------------------
void App::frame() {
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  XRC(xrWaitFrame(session, &wi, &fs));
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  XRC(xrBeginFrame(session, &bi));
  XrTime t = fs.predictedDisplayTime;

  handleInput(t);
  if ((!centered || recenterPending) && fs.shouldRender) {
    recenter(t, false);
    recenterPending = false;
  }
  if (fs.shouldRender) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    views[0] = views[1] = {XR_TYPE_VIEW};
    XRC(xrLocateViews(session, &li, &vs, 2, &n, views));
    bool valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (valid && !viewsValid) {
      viewsValid = true;
      if (cfg.proj == P_FLAT) tw = th = 0;  // now the headset's density is known
    }
    if (valid && (screenChanged || tw == 0)) updateTargetSize();
  }

  bool newFrame = renderMpv();
  if (newFrame && !cfg.renderMode) arrangeVideo();
  if (fs.shouldRender && cfg.renderMode) {
    if (cfg.proj != P_FLAT) renderSphere();
    else renderEyes();
  }
  bool osdNew = updateOsd();
  glFinish();  // OpenGL done with the shared images before Vulkan reads them

  // Vulkan: copy the shared images into the OpenXR swapchains
  struct Job {
    Swapchain *sc;
    SharedImage *src;  // null: clear
    uint32_t idx;
  };
  std::vector<Job> jobs;
  auto acquire = [&](Swapchain &s, SharedImage *src) {
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx;
    XRC(xrAcquireSwapchainImage(s.sc, &ai, &idx));
    XrSwapchainImageWaitInfo wi2{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi2.timeout = XR_INFINITE_DURATION;
    XRC(xrWaitSwapchainImage(s.sc, &wi2));
    jobs.push_back({&s, src, idx});
  };
  if (newFrame && !cfg.renderMode && video.sc) acquire(video, &videoImg);
  if (osdNew) acquire(osdSc, &osdImg);
  if (fs.shouldRender)
    for (int e = 0; e < 2; e++) acquire(proj[e], cfg.renderMode ? &eyeImg[e] : nullptr);
  if (!jobs.empty()) {
    vk.begin();
    VkClearColorValue clear{};
    for (int i = 0; i < 3; i++) clear.float32[i] = colorFormat == VK_FORMAT_R8G8B8A8_SRGB ? srgbToLinear(cfg.bg[i]) : cfg.bg[i];
    clear.float32[3] = 1;
    for (auto &j : jobs) {
      VkImage dst = j.sc->imgs[j.idx].image;
      vk.barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      if (j.src) {
        vk.barrier(j.src->img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {(uint32_t)std::min(j.src->w, j.sc->w), (uint32_t)std::min(j.src->h, j.sc->h), 1};
        vkCmdCopyImage(vk.cmd, j.src->img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
      } else {
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(vk.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &r);
      }
      vk.barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    }
    vk.submitWait();
    for (auto &j : jobs) {
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      XRC(xrReleaseSwapchainImage(j.sc->sc, &ri));
      j.sc->released = true;
    }
  }

  // layers
  std::vector<XrCompositionLayerBaseHeader *> layers;
  XrCompositionLayerProjectionView pv[2];
  XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad quads[3];
  if (fs.shouldRender) {
    for (int e = 0; e < 2; e++) {
      pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      pv[e].pose = views[e].pose;
      pv[e].fov = views[e].fov;
      pv[e].subImage.swapchain = proj[e].sc;
      pv[e].subImage.imageRect = {{0, 0}, {proj[e].w, proj[e].h}};
    }
    projLayer.space = local;
    projLayer.viewCount = 2;
    projLayer.views = pv;
    layers.push_back((XrCompositionLayerBaseHeader *)&projLayer);
    float aspect = th > 0 ? (float)tw / th : 16.0f / 9;
    float sh = cfg.screenW / aspect;
    int nq = 0;
    if (!cfg.renderMode && video.sc && video.released && videoReady) {
      for (int e = 0; e < 2; e++) {
        XrCompositionLayerQuad &q = quads[nq++];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.space = local;
        q.eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
        q.subImage.swapchain = video.sc;
        q.subImage.imageRect = {{e * tw, 0}, {tw, th}};
        q.pose = screen;
        q.size = {cfg.screenW, sh};
        layers.push_back((XrCompositionLayerBaseHeader *)&q);
      }
    }
    if (osdShown && osdSc.released) {
      XrCompositionLayerQuad &q = quads[nq++];
      q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      q.space = local;
      q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain = osdSc.sc;
      q.subImage.imageRect = {{0, 0}, {Osd::W, Osd::H}};
      float ow = std::min(cfg.screenW * 0.45f, 2.2f), oh = ow * Osd::H / Osd::W;
      V3 off = qrot(screen.orientation, {0, -sh / 2 + oh / 2 + sh * 0.05f, 0.01f});
      if (cfg.proj != P_FLAT) {  // VR video: a bar below the front, at the screen distance
        ow = 0.42f * cfg.dist;
        oh = ow * Osd::H / Osd::W;
        off = qrot(screen.orientation, {0, -0.28f * cfg.dist, 0});
      }
      q.pose = screen;
      q.pose.position = {screen.position.x + off.x, screen.position.y + off.y, screen.position.z + off.z};
      q.size = {ow, oh};
      layers.push_back((XrCompositionLayerBaseHeader *)&q);
    }
  }
  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = t;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = (uint32_t)layers.size();
  ei.layers = layers.data();
  XRC(xrEndFrame(session, &ei));
  if (rctx && newFrame) mpv_render_context_report_swap(rctx);
  drawMirror();
}

// ---- keyboard (mirror window): mpv keys + a few of our own ---------------------------------------
static std::string mods(int m) {
  std::string s;
  if (m & GLFW_MOD_SHIFT) s += "Shift+";
  if (m & GLFW_MOD_CONTROL) s += "Ctrl+";
  if (m & GLFW_MOD_ALT) s += "Alt+";
  return s;
}

void App::keyCb(GLFWwindow *w, int key, int, int action, int m) {
  App *a = (App *)glfwGetWindowUserPointer(w);
  if (action == GLFW_RELEASE) return;
  // our keys: Ctrl+Up/Down screen size, Alt+Up/Down distance, F7 swap eyes, F5 recenter
  if ((m & GLFW_MOD_CONTROL) && (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN)) {
    a->cfg.screenW = std::clamp(a->cfg.screenW * (key == GLFW_KEY_UP ? 1.08f : 1 / 1.08f), 0.3f, 40.0f);
    a->screenChanged = true;
    a->sizeChangedAt = nowSec();
    char b[64];
    snprintf(b, sizeof b, "%s %.1f M", tr("PANTALLA", "SCREEN"), a->cfg.screenW);
    a->showMsg(b, 1.5);
    return;
  }
  if ((m & GLFW_MOD_ALT) && (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN)) {
    float nd = std::clamp(a->cfg.dist + (key == GLFW_KEY_UP ? 0.25f : -0.25f), 0.4f, 30.0f);
    V3 d = qrot(a->screen.orientation, {0, 0, -1}) * (nd - a->cfg.dist);
    a->screen.position.x += d.x;
    a->screen.position.y += d.y;
    a->screen.position.z += d.z;
    a->cfg.dist = nd;
    a->screenChanged = true;
    a->sizeChangedAt = nowSec();
    char b[64];
    snprintf(b, sizeof b, "%s %.1f M", tr("DISTANCIA", "DISTANCE"), a->cfg.dist);
    a->showMsg(b, 1.5);
    return;
  }
  if (key == GLFW_KEY_F7) {
    a->cfg.swap = !a->cfg.swap;
    a->forceRender = true;
    a->showMsg(a->cfg.swap ? tr("OJOS INVERTIDOS", "EYES SWAPPED") : tr("OJOS NORMALES", "EYES NORMAL"));
    return;
  }
  if (key == GLFW_KEY_F5) {
    a->recenterPending = true;
    return;
  }
  static const std::map<int, const char *> names = {
      {GLFW_KEY_RIGHT, "RIGHT"}, {GLFW_KEY_LEFT, "LEFT"},       {GLFW_KEY_UP, "UP"},          {GLFW_KEY_DOWN, "DOWN"},
      {GLFW_KEY_PAGE_UP, "PGUP"}, {GLFW_KEY_PAGE_DOWN, "PGDWN"}, {GLFW_KEY_HOME, "HOME"},     {GLFW_KEY_END, "END"},
      {GLFW_KEY_ENTER, "ENTER"}, {GLFW_KEY_ESCAPE, "ESC"},       {GLFW_KEY_BACKSPACE, "BS"},  {GLFW_KEY_TAB, "TAB"},
      {GLFW_KEY_DELETE, "DEL"},  {GLFW_KEY_INSERT, "INS"},       {GLFW_KEY_SPACE, "SPACE"},   {GLFW_KEY_F1, "F1"},
      {GLFW_KEY_F2, "F2"},       {GLFW_KEY_F3, "F3"},            {GLFW_KEY_F4, "F4"},         {GLFW_KEY_F6, "F6"},
      {GLFW_KEY_F8, "F8"},       {GLFW_KEY_F9, "F9"},            {GLFW_KEY_F10, "F10"},       {GLFW_KEY_F11, "F11"},
      {GLFW_KEY_F12, "F12"}};
  auto it = names.find(key);
  if (it != names.end()) {
    if (key == GLFW_KEY_SPACE && !(m & (GLFW_MOD_CONTROL | GLFW_MOD_ALT))) {
      a->mpvKey("SPACE");
    } else {
      std::string k = mods(m) + it->second;
      a->mpvKey(k.c_str());
    }
    a->osdUntil = nowSec() + 3;
    a->osdDirty = true;
    return;
  }
  // Ctrl/Alt + letter (no char event for those)
  if ((m & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
    std::string k = mods(m & ~GLFW_MOD_SHIFT) + (char)('a' + (key - GLFW_KEY_A));
    a->mpvKey(k.c_str());
  }
}

void App::charCb(GLFWwindow *w, unsigned int cp) {
  App *a = (App *)glfwGetWindowUserPointer(w);
  if (cp == ' ') return;  // handled as SPACE
  if (cp == 'o' || cp == 'O') {
    a->osdPinned = !a->osdPinned;
    a->osdUntil = 0;
    a->osdDirty = true;
    return;
  }
  char buf[8] = {};
  // UTF-8 encode
  if (cp < 0x80) buf[0] = (char)cp;
  else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); }
  else return;
  std::string k = buf;
  if (k == "#") k = "SHARP";
  a->mpvKey(k.c_str());
  a->osdUntil = nowSec() + 3;
  a->osdDirty = true;
}

// ---- main loop ------------------------------------------------------------------------------------
void App::cleanup() {
  if (rctx) mpv_render_context_free(rctx);
  rctx = nullptr;
  if (mpv) mpv_terminate_destroy(mpv);
  mpv = nullptr;
  if (screenChanged) saveScreenConf(cfg);
  if (vk.dev) vkDeviceWaitIdle(vk.dev);
  destroySwapchain(video);
  destroySwapchain(osdSc);
  for (auto &p : proj) destroySwapchain(p);
  videoImg.destroy();
  osdImg.destroy();
  for (auto &e : eyeImg) e.destroy();
  if (actionSet) xrDestroyActionSet(actionSet);
  if (viewSpace) xrDestroySpace(viewSpace);
  if (local) xrDestroySpace(local);
  if (session) xrDestroySession(session);
  if (vk.fence) vkDestroyFence(vk.dev, vk.fence, nullptr);
  if (vk.pool) vkDestroyCommandPool(vk.dev, vk.pool, nullptr);
  if (vk.dev) vkDestroyDevice(vk.dev, nullptr);
  if (vk.inst) vkDestroyInstance(vk.inst, nullptr);
  if (g_inst) xrDestroyInstance(g_inst);
  if (win) glfwDestroyWindow(win);
  glfwTerminate();
}

int App::run() {
  g_app = this;
  signal(SIGINT, onSigint);
  signal(SIGTERM, onSigint);
  loadScreenConf(cfg);
  initWindow();
  initXr();
  initVulkan();
  initSession();
  logf("%s", tr("listo: ponte el visor", "ready: put the headset on"));
  while (!exitRequested) {
    glfwPollEvents();
    if (glfwWindowShouldClose(win) || g_sigint) {
      glfwSetWindowShouldClose(win, GLFW_FALSE);
      g_sigint = false;
      if (mpv && !mpvDead) mpvCmd({"quit"});
      else quitting = true;
      if (!quitAt) quitAt = nowSec();
    }
    pollXrEvents();
    if (running && !mpv) {  // start mpv when the headset shows us (files don't start unseen)
      initMpv();
      loadFiles();
    }
    pollMpvEvents();
    if (quitting) {
      if (running && !exitRequested && session) {
        static bool asked = false;
        if (!asked) xrRequestExitSession(session);
        asked = true;
      }
      if (!running || nowSec() - quitAt > 3) break;
    }
    if (running) {
      frame();
    } else {
      if (mpv && renderMpv()) glFinish();  // keep mpv's video moving
      drawMirror();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  cleanup();
  return 0;
}

// ------------------------------------------------------------------------------------------------
static void usage() {
  fprintf(stderr,
          "uso: vrvideo [opciones] ARCHIVO|URL...\n"
          "  --vr-layout=sbs|hsbs|tab|htab|mono   formato del video (sbs = SBS completo)\n"
          "  --vr-swap                            invertir ojos\n"
          "  --vr-screen=METROS                   ancho de la pantalla virtual (por defecto 3.6)\n"
          "  --vr-distance=METROS                 distancia de la pantalla (por defecto 3.0)\n"
          "  --vr-height=METROS                   subir/bajar la pantalla\n"
          "  --vr-mode=quad|render                quad: capas por ojo (mejor); render: dibujado propio\n"
          "  --vr-projection=flat|180|360         video VR equirectangular de 180 o 360 grados\n"
          "  --vr-projection=fisheye[:GRADOS]     video VR ojo de pez (180 por defecto; MKX200 = 200)\n"
          "  --vr-scale=auto|native|FACTOR        resolucion a la que mpv dibuja cada ojo\n"
          "  --vr-supersample=F                   auto: pixeles por pixel del visor (1.25)\n"
          "  --vr-max-eye=PIXELES                 auto: ancho maximo por ojo (4096)\n"
          "  --vr-aspect=W:H                      proporcion de un ojo (por defecto la del video)\n"
          "  --vr-bg=R,G,B                        color del fondo (0..1)\n"
          "  --vr-no-mirror                       sin ventana espejo en el escritorio\n"
          "  cualquier otra --opcion va a mpv (--start=, --aid=, --vf=, --hwdec=...)\n");
}

int main(int argc, char **argv) {
  App app;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    }
    if (a.rfind("--", 0) != 0) {
      app.files.push_back(a);
      continue;
    }
    std::string k = a.substr(2), v;
    auto eq = k.find('=');
    bool hasV = eq != std::string::npos;
    if (hasV) {
      v = k.substr(eq + 1);
      k = k.substr(0, eq);
    }
    Config &c = app.cfg;
    if (k == "vr-layout") {
      if (v == "sbs" || v == "full") c.layout = L_SBS;
      else if (v == "hsbs" || v == "half") c.layout = L_HSBS;
      else if (v == "tab" || v == "ou") c.layout = L_TAB;
      else if (v == "htab" || v == "hou") c.layout = L_HTAB;
      else if (v == "mono" || v == "2d") c.layout = L_MONO;
      else die("--vr-layout desconocido: %s", v.c_str());
    } else if (k == "vr-swap") c.swap = !hasV || v == "yes" || v == "1";
    else if (k == "vr-screen") { c.screenW = (float)atof(v.c_str()); c.screenFromArgs = true; }
    else if (k == "vr-distance") { c.dist = (float)atof(v.c_str()); c.distFromArgs = true; }
    else if (k == "vr-height") c.heightOff = (float)atof(v.c_str());
    else if (k == "vr-mode") c.renderMode = v == "render";
    else if (k == "vr-projection") {
      if (v == "180") c.proj = P_180;
      else if (v == "360") c.proj = P_360;
      else if (v == "flat" || v == "screen") c.proj = P_FLAT;
      else if (v.rfind("fisheye", 0) == 0) {  // fisheye or fisheye:FOV
        c.proj = P_FISHEYE;
        if (v.size() > 8) c.fisheyeFov = std::clamp((float)atof(v.c_str() + 8), 90.0f, 360.0f);
      } else die("--vr-projection desconocida: %s (flat, 180, 360, fisheye[:GRADOS])", v.c_str());
    }
    else if (k == "vr-scale") c.scale = v;
    else if (k == "vr-supersample") c.supersample = (float)atof(v.c_str());
    else if (k == "vr-max-eye") c.maxEye = atoi(v.c_str());
    else if (k == "vr-aspect") {
      float w = 0, h = 0;
      if (sscanf(v.c_str(), "%f:%f", &w, &h) == 2 && h > 0) c.eyeAspect = w / h;
      else c.eyeAspect = (float)atof(v.c_str());
    } else if (k == "vr-bg") sscanf(v.c_str(), "%f,%f,%f", &c.bg[0], &c.bg[1], &c.bg[2]);
    else if (k == "vr-no-mirror") c.mirror = false;
    else if (k.rfind("vr-", 0) == 0) die("opcion desconocida: %s", a.c_str());
    else if (!hasV && k.rfind("no-", 0) == 0) app.mpvOpts.push_back({k.substr(3), "no"});
    else app.mpvOpts.push_back({k, hasV ? v : "yes"});
  }
  if (app.cfg.proj != P_FLAT) app.cfg.renderMode = true;  // drawn into full-size eye images
  if (app.cfg.screenW < 0.2f) app.cfg.screenW = 3.6f;
  if (app.cfg.dist < 0.2f) app.cfg.dist = 3.0f;
  bool idleGiven = false;
  for (auto &o : app.mpvOpts)
    if (o.first == "idle") idleGiven = true;
  if (app.files.empty() && !idleGiven) {
    usage();
    return 1;
  }
  return app.run();
}
