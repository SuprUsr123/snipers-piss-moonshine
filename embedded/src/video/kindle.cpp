/*
 * Kindle e-ink video renderer.
 *
 * Renders through the core X11 protocol into an XImage at the panel's native
 * depth (16bpp RGB565 on Kindle). Every decoded frame is diffed against the
 * frame currently shown and only the changed region is pushed with
 * XPutImage. Small damage regions let the Kindle X server pick its fastest
 * e-ink waveform, which is what makes interactive refresh rates possible;
 * larger regions automatically receive higher-quality (slower) waveforms.
 *
 * A periodic full-window repaint clears accumulated ghosting while content
 * is changing. A static picture produces no damage at all, so an idle
 * secondary screen stays flicker-free.
 *
 * All X calls happen on the GTK main loop; the decoder thread only copies
 * the luma plane and computes damage under a mutex.
 */
#include "../input/keyboard.h"
#include "../kindle-modes.h"

extern "C" {
#include <Limelight.h>
#include <wels/codec_api.h>
}

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

#include "../kindle-style.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h> /* XDestroyImage */

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <vector>

#ifdef HAVE_FBINK
extern "C" {
#include <linux/fb.h>
#include <fbink.h>
}
#endif

/* Update policy knobs (tuned via kindle_video_set_options / moonlight.conf). */
static const int TICK_MS = 15;            /* damage polling interval */
static const int MIN_REFRESH_MS = 10;     /* floor for optionRefreshMs */
static const int MOTION_INTERVAL_MS = 15; /* touch position send rate */
static const int EXIT_HOLD_MS = 2000;     /* hold to exit the stream */

static int optionRefreshMs = 100;     /* min interval between panel updates */
static int optionFullRefreshMs = 2000;/* min interval between ghost-clearing repaints, 0 = off */
static int optionWfm = 0;             /* -1 off, 0 auto, else a WFM_* constant */
static int optionFbinkRotate = -1;    /* fb coord mapping: -1 auto, else 0/90/180/270 */
static int optionRotate = -1;         /* video rotation: -1 auto, else 0/90/180/270 */
static bool optionDebug = false;

/* Rendering backend: X11 pushes pixels through the X server, FBink writes
 * them straight into the framebuffer. Both variables track the chosen path;
 * a failed FBink setup remains selected and is reported visibly. */
static int optionBackend = KINDLE_BACKEND_DEFAULT;
static int activeBackend = KINDLE_BACKEND_DEFAULT;
static bool activeBackendReady = false;
static char backendProblem[160];

/* The rendering mode selected at stream start, KINDLE_MODE_CUSTOM when the
 * config file values apply unchanged. The options bar can switch modes on
 * the fly; see applyRenderMode below. */
static int currentMode = KINDLE_MODE_CUSTOM;

/* Apply a rendering preset: overwrite the panel update policy with the
 * mode's numbers. Without FBink the waveform part is a no-op and only the
 * update intervals change. */
static void applyRenderMode(int mode) {
  if (mode < 0 || mode >= KINDLE_MODE_COUNT)
    return;
  currentMode = mode;
  optionRefreshMs = std::max(MIN_REFRESH_MS, kindle_mode_refresh_ms(mode));
  optionFullRefreshMs = kindle_mode_full_refresh_ms(mode);
#ifdef HAVE_FBINK
  const char* wfm = kindle_mode_wfm(mode);
  if (strcmp(wfm, "a2") == 0) optionWfm = WFM_A2;
  else if (strcmp(wfm, "gc16") == 0) optionWfm = WFM_GC16;
  else optionWfm = 0; /* auto */
#endif
}

extern "C" void kindle_video_set_options(int refreshMs, int fullRefreshMs, const char* wfm,
                                         int fbinkRotate, int debugLevel, int renderMode,
                                         int renderBackend, int rotate) {
  optionBackend = renderBackend == KINDLE_BACKEND_FBINK ? KINDLE_BACKEND_FBINK
                                                        : KINDLE_BACKEND_X11;
#ifndef HAVE_FBINK
  if (optionBackend == KINDLE_BACKEND_FBINK)
    fprintf(stderr, "kindle: FBink rendering requested but FBink support is not compiled in\n");
#endif
  if (refreshMs > 0)
    optionRefreshMs = std::max(MIN_REFRESH_MS, refreshMs);
  if (fullRefreshMs >= 0)
    optionFullRefreshMs = fullRefreshMs;
#ifdef HAVE_FBINK
  optionWfm = 0;
  if (wfm != NULL) {
    if (strcmp(wfm, "off") == 0) optionWfm = -1;
    else if (strcmp(wfm, "a2") == 0) optionWfm = WFM_A2;
    else if (strcmp(wfm, "du") == 0) optionWfm = WFM_DU;
    else if (strcmp(wfm, "gc16") == 0) optionWfm = WFM_GC16;
  }
#else
  (void)wfm;
  optionWfm = -1;
#endif
  optionFbinkRotate = fbinkRotate;
  optionRotate = (rotate == 90 || rotate == 180 || rotate == 270 || rotate == 0)
                     ? rotate : -1; /* -1 = pick what uses the panel best */
  optionDebug = debugLevel > 0;
  if (renderMode >= 0 && renderMode < KINDLE_MODE_COUNT)
    applyRenderMode(renderMode); /* preset wins over the manual numbers */
  else
    currentMode = KINDLE_MODE_CUSTOM;
}

/* The rotation in effect (auto already resolved). The menu stores it after
 * a stream so the next one reuses the orientation that fit the panel. */
extern "C" int kindle_video_get_rotate(void);

static ISVCDecoder* decoder = NULL;
static GtkWidget* window = NULL;
static GtkWidget* drawingArea = NULL;
static GtkWidget* modeButton = NULL;   /* streaming options bar */
static GtkWidget* touchButton = NULL;
static guint refreshTimerId = 0;
static guint exitTimerId = 0;
static int keyboardModifiers;
static bool viewOnly;
static int streamWidth;
static int streamHeight;

/* X11 render state, owned by the GTK thread. */
static Display* xDisplay = NULL;
static Window xWindow = 0;
static GC xGc = NULL;
static XImage* screenImage = NULL;   /* view-sized image at the panel depth */
static int screenBpp = 0;            /* bytes per pixel of the X image */
/* The image data buffer is owned by screenImage and freed by XDestroyImage. */
static int builtWidth = 0;           /* window size the state was built for */
static int builtHeight = 0;
static bool rebuildPending = false;
static int viewX, viewY, viewWidth, viewHeight; /* output rect in window coords */
static double cropX, cropY, cropWidth, cropHeight; /* sampled source rect, effective coordinates */
static int winOffX, winOffY;         /* drawing area origin on the X screen */
static int xScreenW, xScreenH;       /* X screen size, for fb coordinate mapping */
static int* columnMap = NULL;        /* view column -> effective column */
static int* rowMap = NULL;           /* view row -> effective row */
/* Rotation-aware source addressing, rebuilt with the tables above. For
 * 0/180 a view pixel is workLuma[rowBaseArr[vy] + colIdx[vx]]; for 90/270
 * (a transpose) it is workLuma[colBaseArr[vx] + rowIdx[vy]]. Precomputed
 * so the per-pixel conversion stays two loads and one add. */
static int* colIdx = NULL;
static int* rowBaseArr = NULL;
static int* colBaseArr = NULL;
static int* rowIdx = NULL;
/* Fallback for X visuals that are not 16/24/32 bit (for example an 8-bit
 * gray panel): a view-sized 8-bit gray buffer drawn through GDK, which knows
 * how to convert to any depth. Used instead of screenImage when set. */
static unsigned char* grayView = NULL;
static int builtStreamW = 0;         /* stream size the scaling tables fit */
static int builtStreamH = 0;
static bool streamResized = false;   /* host sends another size than requested */
static GtkWidget* streamStatus = NULL; /* "waiting for video" line under the video */
static char renderProblem[160] = "";  /* why nothing can be drawn, if known */
static bool firstPushDone = false;
static bool firstFrameLogged = false;
static struct timespec windowStarted = {0, 0};

/* All-black picture detection. Sunshine on Wayland sometimes streams
 * nothing but black frames - or windows that vanish and flicker - when
 * its capture method does not fit the desktop (wrong `capture` setting,
 * NVIDIA without modeset, per-window capture). The decoder thread samples
 * each frame; after ~4 s of solid black a hint appears under the video
 * and disappears again as soon as real content arrives. Transitions only,
 * so the panel is not repainted while the hint stays up. */
static struct timespec blackSince = {0, 0};
static bool blackHintShown = false;
static int blackHintEvent = 0; /* 1 = show hint, 2 = hide, read by refreshTick */

static int viewRotate = 0;           /* rotation in effect for the built view */
static bool rotateChanged = false;   /* Rotate button pressed: rebuild the view */
static int builtRotate = 0;          /* rotation the current tables were built for */

/* The rotation in effect (auto already resolved). Defined here because it
 * reads the state above. */
extern "C" int kindle_video_get_rotate(void) {
  return viewRotate;
}

static bool renderReady() {
  return screenImage != NULL || grayView != NULL;
}

/* Effective frame size after video rotation. */
static void effectiveDims(int* ew, int* eh) {
  if (viewRotate % 180) {
    *ew = streamHeight;
    *eh = streamWidth;
  } else {
    *ew = streamWidth;
    *eh = streamHeight;
  }
}

/* Frame state, protected by frameMutex. */
static pthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char* workLuma = NULL;  /* latest decoded luma plane, packed */
static unsigned char* shownLuma = NULL; /* luma plane the panel currently shows */
static int lumaWidth = 0;
static int lumaHeight = 0;
static bool pendingDamage = false;
static int damageX0, damageY0, damageX1, damageY1; /* stream coords, inclusive */
static bool needsFullPush = false;   /* expose/resize: repaint whole window */
static timespec lastPush = {0, 0};
static timespec lastFullPush = {0, 0};
static timespec lastMotionSend = {0, 0};

static unsigned short grayLut16[256];
static unsigned int grayLut32[256];

/* FBink drives the e-ink controller directly, so every pushed region gets
 * an explicit waveform (A2 for tiny damage, DU for medium regions, GC16
 * otherwise, flashing GC16 for the periodic ghost-clearing repaint)
 * instead of whatever the X server picks on its own. Best effort: if
 * FBink cannot be used, the X server's own refresh behavior remains. */
#ifdef HAVE_FBINK
static int fbinkFd = -1;
static bool fbinkInited = false;
static uint32_t fbinkScreenW = 0;
static uint32_t fbinkScreenH = 0;
static bool fbinkInvertedGray = false;
static unsigned long long statFbink = 0;

static bool fbinkEnsureInit() {
  if (fbinkInited)
    return fbinkFd >= 0;
  fbinkInited = true;
  fbinkFd = fbink_open();
  if (fbinkFd < 0) {
    fprintf(stderr, "kindle: fbink_open failed, staying with X server refreshes\n");
    return false;
  }
  FBInkConfig cfg = {};
  cfg.is_quiet = true;
  if (fbink_init(fbinkFd, &cfg) < 0) {
    fprintf(stderr, "kindle: fbink_init failed, staying with X server refreshes\n");
    fbink_close(fbinkFd);
    fbinkFd = -1;
    return false;
  }
  FBInkState state = {};
  fbink_get_state(&cfg, &state);
  fbinkScreenW = state.screen_width;
  fbinkScreenH = state.screen_height;
  fbinkInvertedGray = state.inverted_grayscale;
  if (optionDebug)
    fprintf(stderr, "kindle: FBink ready (%s), framebuffer %ux%u\n",
            state.device_name, fbinkScreenW, fbinkScreenH);
  return true;
}

/* Rotation between the X screen and the framebuffer layout. Auto resolves
 * to identity when the dimensions match and to 90 degrees clockwise when
 * they are swapped; fbink_rotate in moonlight.conf overrides the guess. */
static int fbinkEffectiveRotation() {
  if (optionFbinkRotate >= 0)
    return optionFbinkRotate;
  if (xScreenW == (int)fbinkScreenW && xScreenH == (int)fbinkScreenH)
    return 0;
  if (xScreenW == (int)fbinkScreenH && xScreenH == (int)fbinkScreenW)
    return 90;
  return 0;
}

/* Push the given X-screen-coordinate rect to the panel through FBink.
 * Called from the GTK thread after the pixels are in the framebuffer. */
static void fbinkRefreshRegion(bool isFull, int x, int y, int w, int h) {
  if (optionWfm < 0)
    return;
  if (!fbinkEnsureInit())
    return;

  FBInkConfig cfg = {};
  if (isFull) {
    /* Flashing GC16 clears ghosting, matching the periodic full repaint. */
    cfg.wfm_mode = WFM_GC16;
    cfg.is_flashing = true;
    if (fbink_refresh(fbinkFd, 0, 0, 0, 0, &cfg) >= 0)
      statFbink++;
    return;
  }

  if (optionWfm > 0)
    cfg.wfm_mode = (WFM_MODE_INDEX_T)optionWfm;
  else {
    double fraction = (viewWidth > 0 && viewHeight > 0)
                          ? ((double)w * h) / ((double)viewWidth * viewHeight)
                          : 1.0;
    if (fraction < 0.03)
      cfg.wfm_mode = WFM_A2;      /* cursor blinks, typing carets */
    else if (fraction < 0.30)
      cfg.wfm_mode = WFM_DU;      /* lines of text, small UI changes */
    else
      cfg.wfm_mode = WFM_GC16;    /* larger regions, no flash */
  }

  long fx, fy, fw, fh;
  long W = fbinkScreenW, H = fbinkScreenH;
  switch (fbinkEffectiveRotation()) {
    case 90:  fx = y;             fy = H - (x + w); fw = h; fh = w; break;
    case 270: fx = W - (y + h);   fy = x;           fw = h; fh = w; break;
    case 180: fx = W - (x + w);   fy = H - (y + h); fw = w; fh = h; break;
    default:  fx = x;             fy = y;           fw = w; fh = h; break;
  }
  if (fx < 0) { fw += fx; fx = 0; }
  if (fy < 0) { fh += fy; fy = 0; }
  if (fx + fw > W) fw = W - fx;
  if (fy + fh > H) fh = H - fy;
  if (fw <= 0 || fh <= 0)
    return;

  /* fbink_refresh takes mxcfb order: top before left. */
  if (fbink_refresh(fbinkFd, (uint32_t)fy, (uint32_t)fx, (uint32_t)fw, (uint32_t)fh, &cfg) >= 0) {
    statFbink++;
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      fprintf(stderr, "kindle: fbink_refresh failed (errno %d), X server refreshes remain\n", errno);
    }
  }
}

/* ------------------------------------------------ FBink rendering backend
 *
 * Instead of asking the X server to move pixels, the decoded frame is
 * written straight into the mmap'ed framebuffer and the region is
 * refreshed through FBink with an explicit waveform. The X window stays
 * mapped for touch input and the options bar; X only touches its own
 * small chrome areas, the video rect belongs to this code. */

static int fbFd = -1;                  /* /dev/fb0, for mmap'ed pixel writes */
static unsigned char* fbPixels = NULL; /* mmap'ed framebuffer memory */
static unsigned long fbMemSize = 0;
static unsigned fbStride = 0;          /* bytes per framebuffer row */
static unsigned fbW = 0, fbH = 0;      /* framebuffer size in pixels */
static int fbBpp = 0;                  /* bytes per framebuffer pixel */
static unsigned char fbLut8[256];
static unsigned short fbLut16[256];
static unsigned int fbLut32[256];

/* Open and map the panel framebuffer, building gray LUTs for its pixel
 * layout. Cheap to call repeatedly; does the work once. */
static bool fbinkBackendReady() {
  if (!fbinkEnsureInit())
    return false;
  if (fbPixels != NULL)
    return true;
  if (fbFd < 0) {
    fbFd = open("/dev/fb0", O_RDWR);
    if (fbFd < 0) {
      fprintf(stderr, "kindle: cannot open /dev/fb0 (%s), FBink rendering unavailable\n",
              strerror(errno));
      return false;
    }
  }
  fb_var_screeninfo var = {};
  fb_fix_screeninfo fix = {};
  if (ioctl(fbFd, FBIOGET_VSCREENINFO, &var) < 0 ||
      ioctl(fbFd, FBIOGET_FSCREENINFO, &fix) < 0 ||
      fix.smem_len == 0 || var.xres == 0 || var.yres == 0) {
    fprintf(stderr, "kindle: framebuffer queries failed, FBink rendering unavailable\n");
    return false;
  }
  int bpp = (int)var.bits_per_pixel / 8;
  if ((var.bits_per_pixel % 8) != 0 || (bpp != 1 && bpp != 2 && bpp != 4)) {
    fprintf(stderr, "kindle: unsupported framebuffer depth %d bpp, FBink rendering unavailable\n",
            var.bits_per_pixel);
    return false;
  }
  if (fbPixels == NULL) {
    void* map = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbFd, 0);
    if (map == MAP_FAILED) {
      fprintf(stderr, "kindle: cannot mmap the framebuffer (%s), FBink rendering unavailable\n",
              strerror(errno));
      return false;
    }
    fbPixels = (unsigned char*)map;
    fbMemSize = fix.smem_len;
  }
  fbStride = fix.line_length;
  fbW = var.xres;
  fbH = var.yres;
  fbBpp = bpp;
  /* The stream is grayscale, so one 256-entry LUT per pixel size covers
   * every panel layout (RGB565 on Kindles, 32bpp elsewhere). */
  for (int value = 0; value < 256; value++) {
    fbLut8[value] = (unsigned char)(fbinkInvertedGray ? 255 - value : value);
    unsigned int pixel = 0;
    const fb_bitfield* channels[3] = {&var.red, &var.green, &var.blue};
    for (int c = 0; c < 3; c++) {
      const fb_bitfield* f = channels[c];
      if (f->length > 0 && f->length <= 8)
        pixel |= ((unsigned)value >> (8 - f->length)) << f->offset;
    }
    fbLut16[value] = (unsigned short)pixel;
    fbLut32[value] = pixel;
  }
  if (optionDebug)
    fprintf(stderr, "kindle: FBink rendering ready, framebuffer %ux%u %d bpp\n",
            fbW, fbH, var.bits_per_pixel);
  return true;
}

static void fbinkBackendShutdown() {
  if (fbPixels != NULL) {
    munmap(fbPixels, fbMemSize);
    fbPixels = NULL;
  }
  fbMemSize = 0;
  if (fbFd >= 0) {
    close(fbFd);
    fbFd = -1;
  }
}

/* Copy the view-coordinate rect [vx0, vx1) x [vy0, vy1) from the converted
 * image into the framebuffer, applying the X-screen-to-fb rotation. Same
 * pixel sizes copy the encoded value directly; anything else goes through
 * the gray value and the panel LUT (the stream is grayscale, so channel
 * order never matters). */
static void fbinkBlitViewRect(int vx0, int vy0, int vx1, int vy1) {
  if (fbPixels == NULL || (screenImage == NULL && grayView == NULL) || fbBpp == 0)
    return;
  const int x0 = viewX + winOffX + vx0; /* view rect origin on the X screen */
  const int y0 = viewY + winOffY + vy0;
  const int w = vx1 - vx0;
  const int h = vy1 - vy0;
  const long W = fbW, H = fbH;
  const int bpl = screenImage != NULL ? screenImage->bytes_per_line : 0;
  const unsigned char* base = screenImage != NULL ? (unsigned char*)screenImage->data : NULL;
  const int rotation = fbinkEffectiveRotation();
  const bool directCopy = screenImage != NULL && screenBpp == fbBpp;

  auto blitPixel = [&](int sx, int sy, int fx, int fy, int vx, int vy) {
    unsigned char* dst = fbPixels + (size_t)fy * fbStride + (size_t)fx * fbBpp;
    if (grayView != NULL) {
      unsigned gray = grayView[(size_t)vy * viewWidth + vx];
      if (fbBpp == 1) {
        *dst = fbLut8[gray];
      } else if (fbBpp == 2) {
        unsigned short v = fbLut16[gray];
        memcpy(dst, &v, 2);
      } else {
        unsigned int v = fbLut32[gray];
        memcpy(dst, &v, 4);
      }
      return;
    }
    const unsigned char* src = base + (size_t)sy * bpl + (size_t)sx * screenBpp;
    if (directCopy) {
      memcpy(dst, src, fbBpp);
      return;
    }
    unsigned gray;
    if (screenBpp == 2) {
      unsigned short v;
      memcpy(&v, src, 2);
      gray = (((v >> 11) & 31) * 255) / 31; /* red channel holds the gray */
    } else if (screenBpp == 4) {
      unsigned int v;
      memcpy(&v, src, 4);
      gray = (v >> 16) & 255;
    } else {
      gray = src[1]; /* 24bpp: the green byte is the full gray value */
    }
    if (fbBpp == 1) {
      *dst = fbLut8[gray & 255];
    } else if (fbBpp == 2) {
      unsigned short v = fbLut16[gray & 255];
      memcpy(dst, &v, 2);
    } else {
      unsigned int v = fbLut32[gray & 255];
      memcpy(dst, &v, 4);
    }
  };

  switch (rotation) {
    case 90:
      /* Screen (x, y) lands at fb (y, H - 1 - x). */
      for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
          int sx = x0 + col, sy = y0 + row;
          blitPixel(sx, sy, sy, H - 1 - sx, vx0 + col, vy0 + row);
        }
      break;
    case 180:
      for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
          int sx = x0 + col, sy = y0 + row;
          blitPixel(sx, sy, W - 1 - sx, H - 1 - sy, vx0 + col, vy0 + row);
        }
      break;
    case 270:
      /* Screen (x, y) lands at fb (W - 1 - y, x). */
      for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
          int sx = x0 + col, sy = y0 + row;
          blitPixel(sx, sy, W - 1 - sy, sx, vx0 + col, vy0 + row);
        }
      break;
    default:
      for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
          int sx = x0 + col, sy = y0 + row;
          blitPixel(sx, sy, sx, sy, vx0 + col, vy0 + row);
        }
      break;
  }
}
#endif

/* Debug counters. */
static unsigned long long statFrames = 0;
static unsigned long long statChanged = 0;
static unsigned long long statPartial = 0;
static unsigned long long statFull = 0;
static timespec statPrinted = {0, 0};

static long msBetween(const timespec& start, const timespec& end) {
  return (end.tv_sec - start.tv_sec) * 1000L + (end.tv_nsec - start.tv_nsec) / 1000000L;
}

static void initGrayLuts() {
  for (int value = 0; value < 256; value++) {
    /* 16bpp RGB565 and 32bpp XRGB gray encodings */
    grayLut16[value] = (unsigned short)(((value >> 3) << 11) | ((value >> 2) << 5) | (value >> 3));
    grayLut32[value] = ((unsigned int)value << 16) | ((unsigned int)value << 8) | (unsigned int)value;
  }
}

static void releaseRenderState() {
  if (screenImage != NULL) {
    XDestroyImage(screenImage); /* also frees the data buffer */
    screenImage = NULL;
  }
  if (xGc != NULL) {
    XFreeGC(xDisplay, xGc);
    xGc = NULL;
  }
  free(columnMap);
  free(rowMap);
  free(colIdx);
  free(rowBaseArr);
  free(colBaseArr);
  free(rowIdx);
  free(grayView);
  columnMap = NULL;
  rowMap = NULL;
  colIdx = NULL;
  rowBaseArr = NULL;
  colBaseArr = NULL;
  rowIdx = NULL;
  grayView = NULL;
  builtWidth = 0;
  builtHeight = 0;
  builtStreamW = 0;
  builtStreamH = 0;
  builtRotate = 0;
  viewWidth = 0;
  viewHeight = 0;
}

/* Rebuild the XImage, scaling tables and view geometry for the current
 * window allocation. Safe to call repeatedly; skips when up to date. */
static void rebuildRenderState(GtkWidget* widget) {
  rebuildPending = false;
  GdkWindow* gdkWindow = widget->window;
  if (gdkWindow == NULL)
    return;
  int windowWidth = widget->allocation.width;
  int windowHeight = widget->allocation.height;
  if (windowWidth <= 0 || windowHeight <= 0)
    return;
  if (streamWidth <= 0 || streamHeight <= 0) {
    rebuildPending = true; /* video setup has not run yet, retry on tick */
    return;
  }
  if (renderReady() && windowWidth == builtWidth && windowHeight == builtHeight &&
      viewRotate == builtRotate)
    return;

  releaseRenderState();
  xDisplay = GDK_WINDOW_XDISPLAY(gdkWindow);
  xWindow = GDK_WINDOW_XWINDOW(gdkWindow);

  /* Screen geometry for the FBink coordinate mapping below. */
  Screen* xScreen = DefaultScreenOfDisplay(xDisplay);
  xScreenW = WidthOfScreen(xScreen);
  xScreenH = HeightOfScreen(xScreen);
  Window childReturn;
  int originX = 0, originY = 0;
  if (XTranslateCoordinates(xDisplay, xWindow, DefaultRootWindow(xDisplay), 0, 0,
                            &originX, &originY, &childReturn)) {
    winOffX = originX;
    winOffY = originY;
  } else {
    winOffX = 0;
    winOffY = 0;
  }

  /* Video rotation. -1 = automatic: pick 0 or 90, whichever shows more of
   * the monitor at the largest fit scale. Explicit rotation still wins. */
  if (optionRotate == 0 || optionRotate == 90 || optionRotate == 180 ||
      optionRotate == 270) {
    viewRotate = optionRotate;
  } else {
    double fit0 = std::min((double)windowWidth / streamWidth,
                           (double)windowHeight / streamHeight);
    double fit90 = std::min((double)windowWidth / streamHeight,
                            (double)windowHeight / streamWidth);
    viewRotate = (fit90 > fit0 * 1.02) ? 90 : 0; /* use more panel; prefer unrotated on ties */
  }
  int effW, effH;
  effectiveDims(&effW, &effH);

  int depth = gdk_drawable_get_depth(gdkWindow);
  const bool grayFallback = depth != 16 && depth != 24 && depth != 32;
  if (grayFallback) {
    fprintf(stderr, "kindle: X depth %d is not 16/24/32, drawing through GDK gray images\n", depth);
    screenBpp = 1;
  } else {
    screenBpp = depth >= 24 ? depth / 8 : 2; /* 16->2, 24->3, 32->4 bytes */
  }

  xGc = XCreateGC(xDisplay, xWindow, 0, NULL);

  /* Fit the entire monitor in the available area. Preserve aspect ratio
   * and center it; the side bars are preferable to losing desktop edges. */
  double scale = std::min((double)windowWidth / effW, (double)windowHeight / effH);
  viewWidth = std::max(1, std::min(windowWidth, (int)(effW * scale + 0.5)));
  viewHeight = std::max(1, std::min(windowHeight, (int)(effH * scale + 0.5)));
  viewX = (windowWidth - viewWidth) / 2;
  viewY = (windowHeight - viewHeight) / 2;
  cropWidth = effW;
  cropHeight = effH;
  cropX = 0;
  cropY = 0;

  columnMap = (int*)malloc(sizeof(int) * viewWidth);
  rowMap = (int*)malloc(sizeof(int) * viewHeight);
  colIdx = (int*)malloc(sizeof(int) * viewWidth);
  colBaseArr = (int*)malloc(sizeof(int) * viewWidth);
  rowBaseArr = (int*)malloc(sizeof(int) * viewHeight);
  rowIdx = (int*)malloc(sizeof(int) * viewHeight);
  if (columnMap == NULL || rowMap == NULL || colIdx == NULL ||
      colBaseArr == NULL || rowBaseArr == NULL || rowIdx == NULL) {
    fprintf(stderr, "kindle: not enough memory for scaling tables\n");
    snprintf(renderProblem, sizeof(renderProblem), "Out of memory while preparing the picture.");
    releaseRenderState();
    return;
  }
  for (int vx = 0; vx < viewWidth; vx++)
    columnMap[vx] = std::min(effW - 1, (int)(cropX + (vx + 0.5) * cropWidth / viewWidth));
  for (int vy = 0; vy < viewHeight; vy++)
    rowMap[vy] = std::min(effH - 1, (int)(cropY + (vy + 0.5) * cropHeight / viewHeight));
  /* Source addressing per rotation (see the arrays' comment above). The
   * offsets use streamWidth: pushToPanel only runs when lumaWidth equals
   * streamWidth, and streamWidth is already final at build time while
   * lumaWidth may still be 0 before the first frame arrives. */
  for (int vx = 0; vx < viewWidth; vx++) {
    int ex = columnMap[vx];
    switch (viewRotate) {
      case 90:  colBaseArr[vx] = (streamHeight - 1 - ex) * streamWidth; colIdx[vx] = 0; break;
      case 180: colBaseArr[vx] = 0; colIdx[vx] = streamWidth - 1 - ex; break;
      case 270: colBaseArr[vx] = ex * streamWidth; colIdx[vx] = 0; break;
      default:  colBaseArr[vx] = 0; colIdx[vx] = ex; break;
    }
  }
  for (int vy = 0; vy < viewHeight; vy++) {
    int ey = rowMap[vy];
    switch (viewRotate) {
      case 90:  rowBaseArr[vy] = 0; rowIdx[vy] = ey; break;
      case 180: rowBaseArr[vy] = (streamHeight - 1 - ey) * streamWidth; rowIdx[vy] = 0; break;
      case 270: rowBaseArr[vy] = 0; rowIdx[vy] = streamWidth - 1 - ey; break;
      default:  rowBaseArr[vy] = ey * streamWidth; rowIdx[vy] = 0; break;
    }
  }

  if (grayFallback) {
    grayView = (unsigned char*)calloc((size_t)viewWidth * viewHeight, 1);
    if (grayView == NULL) {
      fprintf(stderr, "kindle: not enough memory for the gray frame\n");
      snprintf(renderProblem, sizeof(renderProblem), "Out of memory while preparing the picture.");
      releaseRenderState();
      return;
    }
  } else {
    XImage* image = XCreateImage(xDisplay, DefaultVisual(xDisplay, DefaultScreen(xDisplay)),
                                 depth, ZPixmap, 0, NULL, viewWidth, viewHeight, 32, 0);
    if (image == NULL || image->bytes_per_line <= 0) {
      fprintf(stderr, "kindle: XCreateImage failed, rendering disabled\n");
      snprintf(renderProblem, sizeof(renderProblem), "The display refused the picture format (depth %d).", depth);
      if (image != NULL) XFree(image);
      releaseRenderState();
      return;
    }
    image->byte_order = LSBFirst; /* pixels below are written little-endian */
    image->data = (char*)malloc((size_t)image->bytes_per_line * viewHeight);
    if (image->data == NULL) {
      fprintf(stderr, "kindle: not enough memory for the frame image\n");
      snprintf(renderProblem, sizeof(renderProblem), "Out of memory while preparing the picture.");
      XFree(image);
      releaseRenderState();
      return;
    }
    screenImage = image;
  }
  renderProblem[0] = 0;
  builtWidth = windowWidth;
  builtHeight = windowHeight;
  builtStreamW = streamWidth;
  builtStreamH = streamHeight;
  builtRotate = viewRotate;
  fprintf(stderr, "kindle: render ready: window %dx%d, stream %dx%d rotated %d, view %dx%d at %d,%d, X depth %d%s\n",
          windowWidth, windowHeight, streamWidth, streamHeight, viewRotate,
          viewWidth, viewHeight, viewX, viewY, depth, grayFallback ? " (GDK gray)" : "");

  pthread_mutex_lock(&frameMutex);
  needsFullPush = true;
  pthread_mutex_unlock(&frameMutex);
}

/* Convert the given effective-coordinate damage rect into the X image.
 * The rect [ex0,ex1) x [ey0,ey1) is in rotated (view) space; the source
 * addressing inside handles the mapping back to stream pixels. Outputs
 * the view-space rect that was converted. */
static void convertViewRegion(int ex0, int ey0, int ex1, int ey1,
                              int* pvx0, int* pvy0, int* pvx1, int* pvy1) {
  if (!renderReady() || columnMap == NULL || rowMap == NULL)
    return;
  int effW, effH;
  effectiveDims(&effW, &effH);
  /* View rect covering the damage (superset is safe: unchanged pixels
   * convert to the same value they already show). */
  int vx0 = std::max(0, (int)std::floor((ex0 - cropX) * viewWidth / cropWidth));
  int vx1 = std::min(viewWidth, std::max(0, (int)std::ceil((ex1 - cropX) * viewWidth / cropWidth) + 1));
  int vy0 = std::max(0, (int)std::floor((ey0 - cropY) * viewHeight / cropHeight));
  int vy1 = std::min(viewHeight, std::max(0, (int)std::ceil((ey1 - cropY) * viewHeight / cropHeight) + 1));
  if (vx0 >= vx1 || vy0 >= vy1)
    return;
  const bool direct = (viewRotate % 180) == 0; /* row-major source rows */
  for (int vy = vy0; vy < vy1; vy++) {
    if (direct) {
      const unsigned char* source = workLuma + (size_t)rowBaseArr[vy];
      if (grayView != NULL) {
        unsigned char* target = grayView + (size_t)vy * viewWidth + vx0;
        for (int vx = vx0; vx < vx1; vx++)
          target[vx - vx0] = source[colIdx[vx]];
      } else {
        unsigned char* dest = (unsigned char*)screenImage->data +
                              (size_t)vy * screenImage->bytes_per_line + (size_t)vx0 * screenBpp;
        if (screenBpp == 2) {
          unsigned short* target = (unsigned short*)dest;
          for (int vx = vx0; vx < vx1; vx++)
            target[vx - vx0] = grayLut16[source[colIdx[vx]]];
        } else if (screenBpp == 4) {
          unsigned int* target = (unsigned int*)dest;
          for (int vx = vx0; vx < vx1; vx++)
            target[vx - vx0] = grayLut32[source[colIdx[vx]]];
        } else {
          for (int vx = vx0; vx < vx1; vx++) {
            unsigned int pixel = grayLut32[source[colIdx[vx]]];
            dest[(vx - vx0) * 3] = pixel & 0xFF;
            dest[(vx - vx0) * 3 + 1] = (pixel >> 8) & 0xFF;
            dest[(vx - vx0) * 3 + 2] = (pixel >> 16) & 0xFF;
          }
        }
      }
    } else {
      /* 90/270: a transpose; the row offset lives per column. */
      if (grayView != NULL) {
        unsigned char* target = grayView + (size_t)vy * viewWidth + vx0;
        for (int vx = vx0; vx < vx1; vx++)
          target[vx - vx0] = workLuma[colBaseArr[vx] + rowIdx[vy]];
      } else {
        unsigned char* dest = (unsigned char*)screenImage->data +
                              (size_t)vy * screenImage->bytes_per_line + (size_t)vx0 * screenBpp;
        if (screenBpp == 2) {
          unsigned short* target = (unsigned short*)dest;
          for (int vx = vx0; vx < vx1; vx++)
            target[vx - vx0] = grayLut16[workLuma[colBaseArr[vx] + rowIdx[vy]]];
        } else if (screenBpp == 4) {
          unsigned int* target = (unsigned int*)dest;
          for (int vx = vx0; vx < vx1; vx++)
            target[vx - vx0] = grayLut32[workLuma[colBaseArr[vx] + rowIdx[vy]]];
        } else {
          for (int vx = vx0; vx < vx1; vx++) {
            unsigned int pixel = grayLut32[workLuma[colBaseArr[vx] + rowIdx[vy]]];
            dest[(vx - vx0) * 3] = pixel & 0xFF;
            dest[(vx - vx0) * 3 + 1] = (pixel >> 8) & 0xFF;
            dest[(vx - vx0) * 3 + 2] = (pixel >> 16) & 0xFF;
          }
        }
      }
    }
  }
  if (pvx0 != NULL) *pvx0 = vx0;
  if (pvy0 != NULL) *pvy0 = vy0;
  if (pvx1 != NULL) *pvx1 = vx1;
  if (pvy1 != NULL) *pvy1 = vy1;
}

/* Push damage to the panel. Call with frameMutex held on the GTK thread.
 * Returns true when something was actually pushed. */
static bool pushToPanel(bool full, const timespec& now) {
  /* The scaling tables must have been built for the very frame size in
   * workLuma, otherwise they would index outside it. */
  if (!renderReady() || xDisplay == NULL || workLuma == NULL ||
      lumaWidth != streamWidth || lumaHeight != streamHeight ||
      builtStreamW != lumaWidth || builtStreamH != lumaHeight)
    return false;
  int effW, effH;
  effectiveDims(&effW, &effH);
  /* View rect covering the pixels to push (filled in by convertViewRegion). */
  int vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
  int pushX, pushY, pushW, pushH; /* screen coords actually pushed */
  if (full) {
    convertViewRegion(0, 0, effW, effH, &vx0, &vy0, &vx1, &vy1);
    memcpy(shownLuma, workLuma, (size_t)lumaWidth * lumaHeight);
    pendingDamage = false; /* the panel now matches the latest frame */
    statFull++;
    lastFullPush = now;
    pushX = winOffX + viewX + vx0;
    pushY = winOffY + viewY + vy0;
    pushW = vx1 - vx0;
    pushH = vy1 - vy0;
  } else {
    int sx0 = damageX0;
    int sy0 = damageY0;
    int sx1 = damageX1 + 1;
    int sy1 = damageY1 + 1;
    /* Stream-space damage rect -> rotated (effective) rect; the view
     * rect comes back out of convertViewRegion. */
    int esx0, esy0, esx1, esy1;
    switch (viewRotate) {
      case 90:  esx0 = streamHeight - sy1; esx1 = streamHeight - sy0;
                esy0 = sx0;              esy1 = sx1; break;
      case 180: esx0 = streamWidth - sx1;  esx1 = streamWidth - sx0;
                esy0 = streamHeight - sy1; esy1 = streamHeight - sy0; break;
      case 270: esx0 = sy0;                esx1 = sy1;
                esy0 = streamWidth - sx1;  esy1 = streamWidth - sx0; break;
      default:  esx0 = sx0;                esx1 = sx1;
                esy0 = sy0;              esy1 = sy1; break;
    }
    convertViewRegion(esx0, esy0, esx1, esy1, &vx0, &vy0, &vx1, &vy1);
    for (int y = sy0; y < sy1; y++)
      memcpy(shownLuma + (size_t)y * lumaWidth + sx0,
             workLuma + (size_t)y * lumaWidth + sx0, sx1 - sx0);
    statPartial++;
    pushX = winOffX + viewX + vx0;
    pushY = winOffY + viewY + vy0;
    pushW = vx1 - vx0;
    pushH = vy1 - vy0;
  }
  if (pushW <= 0 || pushH <= 0)
    return true; /* no output pixels need updating for this source region */
#ifdef HAVE_FBINK
  if (activeBackend == KINDLE_BACKEND_FBINK) {
    if (!activeBackendReady)
      return false;
    /* Pixels straight into the framebuffer, one explicit FBink refresh
     * with the waveform the current mode asks for. No X server involved,
     * so no double refresh of the same region. */
    fbinkBlitViewRect(vx0, vy0, vx1, vy1);
    fbinkRefreshRegion(full, pushX, pushY, pushW, pushH);
    lastPush = now;
    return true;
  }
#endif
  if (activeBackend != KINDLE_BACKEND_X11)
    return false;
  /* X11 backend: pixels through the X server, which also picks the
   * waveforms itself - the reference behavior. */
  if (grayView != NULL) {
    gdk_draw_gray_image(drawingArea->window, drawingArea->style->black_gc,
                        pushX - winOffX, pushY - winOffY, pushW, pushH,
                        GDK_RGB_DITHER_NONE,
                        grayView + (size_t)vy0 * viewWidth + vx0, viewWidth);
  } else {
    XPutImage(xDisplay, xWindow, xGc, screenImage, vx0, vy0,
              pushX - winOffX, pushY - winOffY, pushW, pushH);
  }
  XFlush(xDisplay);
  lastPush = now;
  return true;
}

/* The line under the video that explains an empty picture. Shown from the
 * start, hidden for good once the first picture reaches the panel. */
static void setStreamStatus(const char* text) {
  if (streamStatus == NULL)
    return;
  const char* current = gtk_label_get_text(GTK_LABEL(streamStatus));
  if (current != NULL && strcmp(current, text) == 0)
    return; /* identical text still repaints on e-ink */
  gtk_label_set_text(GTK_LABEL(streamStatus), text);
}

static void updateStreamStatus() {
  if (streamStatus == NULL || firstPushDone)
    return;
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (renderProblem[0] != 0)
    setStreamStatus(renderProblem);
  else if (backendProblem[0] != 0)
    setStreamStatus(backendProblem);
  else if (msBetween(windowStarted, now) > 8000)
    setStreamStatus("Still no picture from the PC. Check that it is "
                    "streaming; if the PC runs Wayland, set Sunshine's "
                    "Capture to Portal or KMS and restart it (see "
                    "HOST-SETUP.md). Tap Exit to give up.");
}

static gboolean refreshTick(gpointer) {
  /* All-black hint transitions come from the decoder thread. */
  if (blackHintEvent != 0) {
    int event = blackHintEvent;
    blackHintEvent = 0;
    if (streamStatus != NULL) {
      if (event == 1) {
        setStreamStatus("The PC is sending a black picture. On the PC's "
                        "Sunshine, set Capture to Portal or KMS and restart "
                        "it, then tap Exit here and stream again. See "
                        "HOST-SETUP.md.");
        gtk_widget_show(streamStatus);
      } else if (event == 2) {
        gtk_widget_hide(streamStatus);
      }
    }
  }
  pthread_mutex_lock(&frameMutex);
  bool resized = streamResized;
  streamResized = false;
  pthread_mutex_unlock(&frameMutex);
  if (resized && drawingArea != NULL) {
    /* The PC encodes another size than was requested: rebuild the view for
     * the size that actually arrives instead of never drawing anything. */
    releaseRenderState();
    rebuildRenderState(drawingArea);
  }
  if (rotateChanged && drawingArea != NULL) {
    /* The Rotate button picked another orientation: rebuild the view
     * tables for it and repaint everything through the new mapping. */
    rotateChanged = false;
    releaseRenderState();
    rebuildRenderState(drawingArea);
  }
  if (drawingArea != NULL && !renderReady() && rebuildPending)
    rebuildRenderState(drawingArea);
  updateStreamStatus();
  if (xDisplay == NULL)
    return TRUE;

  bool hideStatus = false;
  pthread_mutex_lock(&frameMutex);
  if (needsFullPush || pendingDamage) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sincePush = msBetween(lastPush, now);
    bool full = needsFullPush ||
                (optionFullRefreshMs > 0 && msBetween(lastFullPush, now) >= optionFullRefreshMs);
    if (needsFullPush || sincePush >= optionRefreshMs) {
      if (pushToPanel(full, now)) {
        pendingDamage = false;
        needsFullPush = false;
        if (!firstPushDone) {
          firstPushDone = true;
          hideStatus = true;
          fprintf(stderr, "kindle: first picture pushed to the panel\n");
        }
      }
    }
    if (optionDebug && msBetween(statPrinted, now) >= 5000) {
      statPrinted = now;
#ifdef HAVE_FBINK
      fprintf(stderr,
              "kindle: decoded %llu frames, %llu changed, %llu partial + %llu full pushes, %llu FBink refreshes\n",
              statFrames, statChanged, statPartial, statFull, statFbink);
#else
      fprintf(stderr,
              "kindle: decoded %llu frames, %llu changed, %llu partial + %llu full pushes\n",
              statFrames, statChanged, statPartial, statFull);
#endif
    }
  }
  pthread_mutex_unlock(&frameMutex);
  /* Hiding resizes the video area, whose handler takes frameMutex: do it
   * only after the mutex is released. */
  if (hideStatus && streamStatus != NULL)
    gtk_widget_hide(streamStatus);
  return TRUE;
}

static gboolean onExpose(GtkWidget*, GdkEventExpose*, gpointer) {
  pthread_mutex_lock(&frameMutex);
  needsFullPush = true;
  pthread_mutex_unlock(&frameMutex);
  return FALSE;
}

static gboolean onMap(GtkWidget* widget, gpointer) {
  rebuildRenderState(widget);
  return FALSE;
}

static void onSizeAllocate(GtkWidget* widget, GtkAllocation*, gpointer) {
  if (widget->window != NULL)
    rebuildRenderState(widget);
}

static int keyEvent(GdkEventKey* event, char action) {
  if (event->keyval == GDK_Escape) {
    if (action == KEY_ACTION_DOWN) gtk_main_quit();
    return TRUE;
  }
  if (event->hardware_keycode < 8 || event->hardware_keycode >= sizeof(keyCodes) / sizeof(keyCodes[0]) + 8)
    return FALSE;
  if (viewOnly) return TRUE;
  int key = event->hardware_keycode - 8;
  switch (event->hardware_keycode) {
  case 0x32: case 0x3e: keyboardModifiers = action == KEY_ACTION_DOWN ? keyboardModifiers | MODIFIER_SHIFT : keyboardModifiers & ~MODIFIER_SHIFT; break;
  case 0x40: case 0x6c: keyboardModifiers = action == KEY_ACTION_DOWN ? keyboardModifiers | MODIFIER_ALT : keyboardModifiers & ~MODIFIER_ALT; break;
  case 0x25: case 0x69: keyboardModifiers = action == KEY_ACTION_DOWN ? keyboardModifiers | MODIFIER_CTRL : keyboardModifiers & ~MODIFIER_CTRL; break;
  }
  LiSendKeyboardEvent(0x8000 | keyCodes[key], action, keyboardModifiers);
  return TRUE;
}

static gboolean keyPress(GtkWidget*, GdkEventKey* event, gpointer) {
  return keyEvent(event, KEY_ACTION_DOWN);
}

static gboolean keyRelease(GtkWidget*, GdkEventKey* event, gpointer) {
  return keyEvent(event, KEY_ACTION_UP);
}

/* Map touch coordinates through the fitted video and its rotation. */
static void sendMousePosition(double x, double y) {
  if (viewWidth <= 0 || viewHeight <= 0 || streamWidth <= 0 || streamHeight <= 0)
    return;
  int effW, effH;
  effectiveDims(&effW, &effH);
  int ex = (int)(cropX + (x - viewX) * cropWidth / viewWidth);
  int ey = (int)(cropY + (y - viewY) * cropHeight / viewHeight);
  ex = std::min(std::max(ex, 0), effW - 1);
  ey = std::min(std::max(ey, 0), effH - 1);
  int px, py;
  switch (viewRotate) {
    case 90:  px = ey;                   py = streamHeight - 1 - ex; break;
    case 180: px = streamWidth - 1 - ex; py = streamHeight - 1 - ey; break;
    case 270: px = streamWidth - 1 - ey; py = ex; break;
    default:  px = ex;                   py = ey; break;
  }
  px = std::min(std::max(px, 0), streamWidth - 1);
  py = std::min(std::max(py, 0), streamHeight - 1);
  LiSendMousePositionEvent((short)px, (short)py, (short)streamWidth, (short)streamHeight);
}

/* Exit-corner helpers, defined below (shared by motion and button). */
static bool exitHoldActive = false; /* press started in the corner, until release */
static int exitCornerPx = 100;      /* long-press exit zone, top-left, scaled at init */
static bool pointInExitCorner(double x, double y);
static void repaintExitCorner();

static gboolean mouseMotion(GtkWidget*, GdkEventMotion* event, gpointer) {
  if (exitTimerId != 0 && !pointInExitCorner(event->x, event->y)) {
    /* Slid out of the corner while holding: cancel the exit, but keep
     * swallowing motion until release so no stray click reaches the host. */
    g_source_remove(exitTimerId);
    exitTimerId = 0;
    repaintExitCorner();
    return TRUE;
  }
  if (exitHoldActive)
    return TRUE; /* holding in the corner: the pointer stays put */
  if (viewOnly) return TRUE;
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (msBetween(lastMotionSend, now) < MOTION_INTERVAL_MS)
    return TRUE;
  lastMotionSend = now;
  sendMousePosition(event->x, event->y);
  return TRUE;
}

/* Long-press the top-left corner to exit the stream (touch-only Kindles
 * have no Escape key). The hold is visualized with a marker drawn over the
 * corner so the gesture is discoverable, it is cancelled when the finger
 * slides out of the zone, and a quick tap in the corner still clicks
 * normally. */

static bool pointInExitCorner(double x, double y) {
  return x < exitCornerPx && y < exitCornerPx;
}

static void drawExitMarker() {
  if (xDisplay == NULL || xWindow == 0 || xGc == NULL)
    return;
  XSetForeground(xDisplay, xGc, 0UL);
  XSetLineAttributes(xDisplay, xGc, 8, LineSolid, CapButt, JoinMiter);
  XDrawRectangle(xDisplay, xWindow, xGc, 8, 8, exitCornerPx - 16, exitCornerPx - 16);
  XFlush(xDisplay);
}

/* Repaint the corner over the marker: the unused part clears to the
 * background, the view part comes straight from the XImage, which the
 * marker never touched. */
static void repaintExitCorner() {
  if (xDisplay == NULL || xWindow == 0)
    return;
  XClearArea(xDisplay, xWindow, 0, 0, exitCornerPx, exitCornerPx, False);
  if (screenImage != NULL && viewWidth > 0 && viewHeight > 0) {
    int ix0 = std::max(0, viewX), iy0 = std::max(0, viewY);
    int ix1 = std::min(exitCornerPx, viewX + viewWidth);
    int iy1 = std::min(exitCornerPx, viewY + viewHeight);
    if (ix0 < ix1 && iy0 < iy1)
      XPutImage(xDisplay, xWindow, xGc, screenImage, ix0 - viewX, iy0 - viewY,
                ix0, iy0, ix1 - ix0, iy1 - iy0);
  }
  XFlush(xDisplay);
}

static gboolean exitHoldFired(gpointer) {
  exitTimerId = 0;
  exitHoldActive = false;
  gtk_main_quit();
  return FALSE;
}

static int mouseButton(GdkEventButton* event, char action) {
  if (action == BUTTON_ACTION_PRESS) {
    if (pointInExitCorner(event->x, event->y) && event->button == 1) {
      exitHoldActive = true;
      if (exitTimerId == 0)
        exitTimerId = g_timeout_add(EXIT_HOLD_MS, exitHoldFired, NULL);
      drawExitMarker();
      return TRUE;
    }
  } else if (exitHoldActive && event->button == 1) {
    bool timerWasPending = exitTimerId != 0;
    if (timerWasPending) {
      g_source_remove(exitTimerId);
      exitTimerId = 0;
    }
    exitHoldActive = false;
    repaintExitCorner();
    if (viewOnly) return TRUE;
    if (timerWasPending && pointInExitCorner(event->x, event->y)) {
      /* Quick tap in the corner: click normally. */
      sendMousePosition(event->x, event->y);
      LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
      LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }
    /* Slid out of the corner mid-hold: cancelled, no click reaches the
     * host. */
    return TRUE;
  }
  if (viewOnly) return TRUE;
  sendMousePosition(event->x, event->y);
  int button = event->button == 3 ? BUTTON_RIGHT : event->button == 2 ? BUTTON_MIDDLE : BUTTON_LEFT;
  if (event->button <= 3) LiSendMouseButtonEvent(action, button);
  return TRUE;
}

static gboolean mousePress(GtkWidget*, GdkEventButton* event, gpointer) {
  return mouseButton(event, BUTTON_ACTION_PRESS);
}

static gboolean mouseRelease(GtkWidget*, GdkEventButton* event, gpointer) {
  return mouseButton(event, BUTTON_ACTION_RELEASE);
}

/* ------------------------------------------------ streaming options bar
 *
 * A row of large touch buttons docked under the video area, so everything
 * a second-screen user needs stays one tap away without a keyboard:
 *
 *   Mode: X  - switch the rendering mode (Fast/Balanced/Quality) while the
 *              stream runs; the panel update policy changes immediately.
 *   Flash    - one full flashing repaint right now, clearing ghosting.
 *   Touch:X  - forward taps to the host, or freeze input for pure reading.
 *   Exit     - end the stream (the corner long-press still works too).
 *
 * The bar is static chrome: it only repaints when tapped, so it does not
 * add panel refreshes during streaming. */

static GtkWidget* rotateButton = NULL; /* options bar: video 0/90/180/270 */

/* Bar fonts and heights follow the shared UI scale (screen size based,
 * static per device - there is no zoom control by design). */
static void applyFont(GtkWidget* widget, int bold) {
  ink_font_px(widget, bold, 24);
}

/* Keyboard focus returns to the video area after every tap so Escape and
 * the keyboard passthrough keep working. */
static void refocusVideo() {
  if (drawingArea != NULL)
    gtk_widget_grab_focus(drawingArea);
}

static void updateModeButtonLabel() {
  if (modeButton == NULL)
    return;
  char text[48];
  snprintf(text, sizeof(text), "Mode: %s", kindle_mode_name(currentMode));
  gtk_button_set_label(GTK_BUTTON(modeButton), text);
  applyFont(gtk_bin_get_child(GTK_BIN(modeButton)), 1);
}

static void updateTouchButtonLabel() {
  if (touchButton == NULL)
    return;
  gtk_button_set_label(GTK_BUTTON(touchButton), viewOnly ? "Touch: Off" : "Touch: On");
  applyFont(gtk_bin_get_child(GTK_BIN(touchButton)), 1);
}

static void barExitClicked(GtkButton*, gpointer) {
  gtk_main_quit();
}

/* Window manager asked to close the stream window: leave the stream loop and
 * let kindleCleanup() destroy the window itself. */
static gboolean onWindowDelete(GtkWidget*, GdkEvent*, gpointer) {
  gtk_main_quit();
  return TRUE;
}

/* ------------------------------------------------- rendering backend bar */

static GtkWidget* backendButton = NULL; /* options bar: X11 / FBink switch */

static void updateBackendButtonLabel() {
  if (backendButton == NULL)
    return;
  char text[48];
  snprintf(text, sizeof(text), "Render: %s", kindle_backend_name(activeBackend));
  gtk_button_set_label(GTK_BUTTON(backendButton), text);
  applyFont(gtk_bin_get_child(GTK_BIN(backendButton)), 1);
}

/* ---------------------------------------------- video rotation button
 *
 * The one orientation that always works: the video itself is rotated in
 * software before it is written to the panel, so a landscape desktop can
 * fill the portrait panel while the Kindle is held sideways. The menu's
 * lipc rotation buttons cannot do this on stock firmware (see
 * kindle-orient.h), and unlike them this needs no window manager help at
 * all. Touch coordinates are mapped through the same rotation. */
static void updateRotateButtonLabel() {
  if (rotateButton == NULL)
    return;
  char text[48];
  snprintf(text, sizeof(text), "Rotate: %d", viewRotate);
  gtk_button_set_label(GTK_BUTTON(rotateButton), text);
  applyFont(gtk_bin_get_child(GTK_BIN(rotateButton)), 1);
}

static void barRotateClicked(GtkButton*, gpointer) {
  int next = (viewRotate + 90) % 360;
  optionRotate = next; /* explicit from now on, no more auto */
  viewRotate = next;
  pthread_mutex_lock(&frameMutex);
  needsFullPush = true;
  lastPush.tv_sec = 0;
  lastPush.tv_nsec = 0;
  lastFullPush.tv_sec = 0;
  lastFullPush.tv_nsec = 0;
  pthread_mutex_unlock(&frameMutex);
  rotateChanged = true; /* refreshTick rebuilds the view tables */
  updateRotateButtonLabel();
  if (optionDebug)
    fprintf(stderr, "kindle: video rotated %d, panel update every %d ms\n",
            viewRotate, optionRefreshMs);
  refocusVideo();
}

/* Switch renderers only when the selected path is ready; never change the
 * user choice behind their back. */
static bool setBackend(int backend) {
#ifdef HAVE_FBINK
  if (backend == KINDLE_BACKEND_FBINK && !fbinkBackendReady()) {
    activeBackend = backend;
    activeBackendReady = false;
    snprintf(backendProblem, sizeof(backendProblem),
             "FBink could not initialize. It remains selected; tap Render to choose X11.");
    return false;
  }
#else
  if (backend == KINDLE_BACKEND_FBINK) {
    activeBackend = backend;
    activeBackendReady = false;
    snprintf(backendProblem, sizeof(backendProblem),
             "FBink is unavailable in this build. Choose X11 in moonlight.conf.");
    return false;
  }
#endif
  if (backend == activeBackend && activeBackendReady)
    return true;
  activeBackend = backend;
  activeBackendReady = true;
  backendProblem[0] = 0;
  if (optionDebug)
    fprintf(stderr, "kindle: rendering through %s now\n", kindle_backend_name(activeBackend));
  pthread_mutex_lock(&frameMutex);
  needsFullPush = true;
  lastPush.tv_sec = 0;
  lastPush.tv_nsec = 0;
  lastFullPush.tv_sec = 0;
  lastFullPush.tv_nsec = 0;
  pthread_mutex_unlock(&frameMutex);
  return true;
}

static void barBackendClicked(GtkButton*, gpointer) {
  int other = activeBackend == KINDLE_BACKEND_X11 ? KINDLE_BACKEND_FBINK
                                                  : KINDLE_BACKEND_X11;
  if (!setBackend(other)) {
    fprintf(stderr, "kindle: %s rendering is unavailable on this device\n",
            kindle_backend_name(other));
    setStreamStatus(backendProblem);
    gtk_widget_show(streamStatus);
  } else if (firstPushDone && streamStatus != NULL) {
    gtk_widget_hide(streamStatus);
  }
  updateBackendButtonLabel();
  refocusVideo();
}

static void barModeClicked(GtkButton*, gpointer) {
  int next = (currentMode < 0) ? KINDLE_MODE_DEFAULT
                               : (currentMode + 1) % KINDLE_MODE_COUNT;
  applyRenderMode(next);
  updateModeButtonLabel();
  if (optionDebug)
    fprintf(stderr, "kindle: rendering mode %s, panel update every %d ms\n",
            kindle_mode_name(next), optionRefreshMs);
  refocusVideo();
}

static void barFlashClicked(GtkButton*, gpointer) {
  /* Next refreshTick pushes the whole view with a flashing waveform. */
  pthread_mutex_lock(&frameMutex);
  needsFullPush = true;
  lastPush.tv_sec = 0;
  lastPush.tv_nsec = 0;
  lastFullPush.tv_sec = 0;
  lastFullPush.tv_nsec = 0;
  pthread_mutex_unlock(&frameMutex);
  refocusVideo();
}

static void barTouchClicked(GtkButton*, gpointer) {
  viewOnly = !viewOnly;
  updateTouchButtonLabel();
  refocusVideo();
}

/* `styleName` is INK_NAME_BUTTON (white at rest) or INK_NAME_MARKED (light
 * gray at rest). Every bar button gets the heavy outline: they are the
 * primary controls while streaming. */
static GtkWidget* barButton(GtkWidget* bar, const char* label, GCallback handler,
                            const char* styleName = INK_NAME_BUTTON) {
  GtkWidget* button = gtk_button_new_with_label(label);
  ink_style_button(button, styleName, INK_BORDER_THICK);
  applyFont(gtk_bin_get_child(GTK_BIN(button)), 1);
  gtk_widget_set_size_request(button, -1, ink_px(64));
  g_signal_connect(button, "clicked", handler, NULL);
  gtk_box_pack_start(GTK_BOX(bar), button, TRUE, TRUE, 0);
  return button;
}

static int kindleSetup(int videoFormat, int width, int height, int, void*, int) {
  if ((videoFormat & VIDEO_FORMAT_MASK_H264) == 0) return -1;

  streamWidth = width;
  streamHeight = height;
  fprintf(stderr, "kindle: stream setup %dx%d, video format 0x%x\n", width, height, videoFormat);
  if (WelsCreateDecoder(&decoder) != 0) {
    decoder = NULL;
    return -1;
  }
  int decoderThreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
  decoderThreads = std::min(3, std::max(1, decoderThreads));
  if (decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS, &decoderThreads) != 0) {
    WelsDestroyDecoder(decoder);
    decoder = NULL;
    return -1;
  }
  SDecodingParam params = {};
  params.uiTargetDqLayer = 255;
  params.eEcActiveIdc = ERROR_CON_FRAME_COPY;
  params.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  params.sVideoProperty.size = sizeof(params.sVideoProperty);
  if (decoder->Initialize(&params) != 0) {
    WelsDestroyDecoder(decoder);
    decoder = NULL;
    return -1;
  }
  initGrayLuts();
  if (optionDebug)
    fprintf(stderr, "kindle: streaming %dx%d via %s, rendering mode %s, panel update every %d ms, full refresh every %d ms, wfm %s\n",
            streamWidth, streamHeight, kindle_backend_name(activeBackend),
            kindle_mode_name(currentMode),
            optionRefreshMs, optionFullRefreshMs,
#ifdef HAVE_FBINK
            optionWfm == -1 ? "off" : optionWfm == 0 ? "auto" : "fixed"
#else
            "off (no FBink)"
#endif
            );
  return 0;
}

extern "C" void kindle_window_init(bool viewonly) {
  ::viewOnly = viewonly;
  gtk_init(NULL, NULL);
  ink_style_init();
  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(window, "ink-window");
  gtk_window_set_title(GTK_WINDOW(window), "L:A_N:application_ID:moonlight_PC:T");
  gtk_window_fullscreen(GTK_WINDOW(window));
  /* Only the window manager closing the window ends the stream here. Do not
   * connect "destroy" to gtk_main_quit: kindleCleanup() destroys this window
   * after the stream loop has already returned, and by then the innermost
   * main loop is the *menu's*, so that destroy would quit the whole app. */
  g_signal_connect(window, "delete-event", G_CALLBACK(onWindowDelete), NULL);
  drawingArea = gtk_drawing_area_new();
  gtk_widget_set_can_focus(drawingArea, TRUE);
  gtk_widget_add_events(drawingArea, GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
            GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  /* White background matches typical document content and avoids dark
   * flashes when the window is exposed on the panel. */
  GdkColor white = { 0, 0xffff, 0xffff, 0xffff };
  gtk_widget_modify_bg(drawingArea, GTK_STATE_NORMAL, &white);
  g_signal_connect(drawingArea, "expose-event", G_CALLBACK(onExpose), NULL);
  g_signal_connect(drawingArea, "map", G_CALLBACK(onMap), NULL);
  g_signal_connect(drawingArea, "size-allocate", G_CALLBACK(onSizeAllocate), NULL);
  g_signal_connect(drawingArea, "key-press-event", G_CALLBACK(keyPress), NULL);
  g_signal_connect(drawingArea, "key-release-event", G_CALLBACK(keyRelease), NULL);
  g_signal_connect(drawingArea, "motion-notify-event", G_CALLBACK(mouseMotion), NULL);
  g_signal_connect(drawingArea, "button-press-event", G_CALLBACK(mousePress), NULL);
  g_signal_connect(drawingArea, "button-release-event", G_CALLBACK(mouseRelease), NULL);

  /* Video area on top, options bar docked underneath it. */
  GtkWidget* layout = gtk_vbox_new(FALSE, 0);
  gtk_container_add(GTK_CONTAINER(window), layout);
  gtk_box_pack_start(GTK_BOX(layout), drawingArea, TRUE, TRUE, 0);

  /* One explanatory line while no picture has arrived yet; hidden by
   * refreshTick() as soon as the first picture is on the panel. */
  firstPushDone = false;
  firstFrameLogged = false;
  renderProblem[0] = 0;
  clock_gettime(CLOCK_MONOTONIC, &windowStarted);
  exitCornerPx = ink_px(110);
  streamStatus = gtk_label_new("Starting the stream ...");
  ink_font_px(streamStatus, 0, 24);
  gtk_label_set_line_wrap(GTK_LABEL(streamStatus), TRUE);
  gtk_box_pack_start(GTK_BOX(layout), streamStatus, FALSE, FALSE, 6);

  GtkWidget* bar = gtk_hbox_new(FALSE, 8);
  gtk_container_set_border_width(GTK_CONTAINER(bar), 8);
  gtk_box_pack_start(GTK_BOX(layout), bar, FALSE, FALSE, 0);
#ifdef HAVE_FBINK
  backendButton = barButton(bar, "Render: X11", G_CALLBACK(barBackendClicked));
#endif
  modeButton = barButton(bar, "Mode", G_CALLBACK(barModeClicked), INK_NAME_MARKED);
  rotateButton = barButton(bar, "Rotate: 0", G_CALLBACK(barRotateClicked));
  barButton(bar, "Flash", G_CALLBACK(barFlashClicked));
  touchButton = barButton(bar, "Touch: On", G_CALLBACK(barTouchClicked));
  barButton(bar, "Exit", G_CALLBACK(barExitClicked));
  updateBackendButtonLabel();
  updateModeButtonLabel();
  updateRotateButtonLabel();
  updateTouchButtonLabel();
  gtk_widget_show_all(window);
  gtk_widget_grab_focus(drawingArea);
  /* The host video has no cursor in it; draw a local X cursor so the pointer
   * is visible. Cursor moves create tiny damage regions, so the pointer is
   * still responsive on the panel. */
  GdkCursor* cursor = gdk_cursor_new(GDK_ARROW);
  gdk_window_set_cursor(drawingArea->window, cursor);
  gdk_cursor_unref(cursor);
  refreshTimerId = g_timeout_add(TICK_MS, refreshTick, NULL);
  /* Honor the configured renderer. An unavailable FBink stays selected
   * and leaves this message visible until the user switches to X11. */
  if (!setBackend(optionBackend)) {
    setStreamStatus(backendProblem);
    gtk_widget_show(streamStatus);
  }
  updateBackendButtonLabel();
}

static void kindleCleanup() {
  if (refreshTimerId != 0) {
    g_source_remove(refreshTimerId);
    refreshTimerId = 0;
  }
  if (exitTimerId != 0) {
    g_source_remove(exitTimerId);
    exitTimerId = 0;
  }
  exitHoldActive = false;
#ifdef HAVE_FBINK
  /* Re-init on the next session: panel rotation may have changed. */
  if (fbinkFd >= 0) {
    fbink_close(fbinkFd);
    fbinkFd = -1;
  }
  fbinkInited = false;
  fbinkBackendShutdown();
#endif
  if (decoder != NULL) {
    /* WelsDestroyDecoder stops its worker threads before freeing contexts. */
    WelsDestroyDecoder(decoder);
    decoder = NULL;
  }
  pthread_mutex_lock(&frameMutex);
  releaseRenderState();
  xDisplay = NULL;
  free(workLuma);
  free(shownLuma);
  workLuma = NULL;
  shownLuma = NULL;
  lumaWidth = 0;
  lumaHeight = 0;
  pendingDamage = false;
  needsFullPush = false;
  streamResized = false;
  firstPushDone = false;
  firstFrameLogged = false;
  renderProblem[0] = 0;
  blackSince.tv_sec = 0;
  blackSince.tv_nsec = 0;
  blackHintShown = false;
  blackHintEvent = 0;
  pthread_mutex_unlock(&frameMutex);
  streamStatus = NULL;
  if (window != NULL) {
    gtk_widget_destroy(window);
    window = NULL;
    drawingArea = NULL;
  }
  if (optionDebug)
#ifdef HAVE_FBINK
    fprintf(stderr, "kindle: session totals: %llu frames decoded, %llu changed, "
            "%llu partial + %llu full panel updates, %llu FBink refreshes\n",
            statFrames, statChanged, statPartial, statFull, statFbink);
#else
    fprintf(stderr, "kindle: session totals: %llu frames decoded, %llu changed, "
            "%llu partial + %llu full panel updates\n",
            statFrames, statChanged, statPartial, statFull);
#endif
}

static int kindleSubmit(PDECODE_UNIT unit) {
  /* Reused across frames so steady-state decoding does not allocate. */
  static std::vector<unsigned char> bitstream;
  bitstream.clear();
  if (bitstream.capacity() < unit->fullLength)
    bitstream.reserve(unit->fullLength + 1024);
  for (PLENTRY entry = unit->bufferList; entry != NULL; entry = entry->next)
    bitstream.insert(bitstream.end(), entry->data, entry->data + entry->length);

  unsigned char* planes[3] = {};
  SBufferInfo info = {};
  DECODING_STATE state = decoder->DecodeFrameNoDelay(bitstream.data(), (int)bitstream.size(), planes, &info);
  /* Only the dsError* bits are failures; dsFramePending and
   * dsRefListNullPtrs still produce usable output. */
  if ((state & ~0x3) != 0)
    return DR_NEED_IDR;
  if (info.iBufferStatus != 1 || planes[0] == NULL)
    return DR_OK;

  int width = info.UsrData.sSystemBuffer.iWidth;
  int height = info.UsrData.sSystemBuffer.iHeight;
  int stride = info.UsrData.sSystemBuffer.iStride[0];
  if (width <= 0 || height <= 0 || stride < width)
    return DR_OK;

  pthread_mutex_lock(&frameMutex);
  statFrames++;
  if (!firstFrameLogged) {
    firstFrameLogged = true;
    fprintf(stderr, "kindle: first frame decoded: %dx%d (requested %dx%d)\n",
            width, height, streamWidth, streamHeight);
  }
  if (width != streamWidth || height != streamHeight) {
    fprintf(stderr, "kindle: the PC sends %dx%d, not the requested %dx%d; following the PC\n",
            width, height, streamWidth, streamHeight);
    streamWidth = width;
    streamHeight = height;
    streamResized = true;
  }
  if (width != lumaWidth || height != lumaHeight) {
    unsigned char* work = (unsigned char*)malloc((size_t)width * height);
    unsigned char* shown = (unsigned char*)malloc((size_t)width * height);
    if (work == NULL || shown == NULL) {
      free(work);
      free(shown);
      pthread_mutex_unlock(&frameMutex);
      return DR_NEED_IDR;
    }
    free(workLuma);
    free(shownLuma);
    workLuma = work;
    shownLuma = shown;
    lumaWidth = width;
    lumaHeight = height;
    pendingDamage = false;
    needsFullPush = true;
  }
  for (int y = 0; y < height; y++)
    memcpy(workLuma + (size_t)y * width, planes[0] + (size_t)y * stride, width);

  /* Cheap all-black sample of the freshly copied frame (see the comment
   * above blackSince). */
  {
    size_t total = (size_t)width * height;
    bool black = true;
    for (size_t i = 0; i < total; i += 61) {
      if (workLuma[i] > 12) {
        black = false;
        break;
      }
    }
    if (black) {
      if (blackSince.tv_sec == 0 && blackSince.tv_nsec == 0)
        clock_gettime(CLOCK_MONOTONIC, &blackSince);
      timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (!blackHintShown && msBetween(blackSince, now) > 4000) {
        blackHintShown = true;
        blackHintEvent = 1;
      }
    } else {
      blackSince.tv_sec = 0;
      blackSince.tv_nsec = 0;
      if (blackHintShown) {
        blackHintShown = false;
        blackHintEvent = 2;
      }
    }
  }

  /* Damage = bounding box of rows that differ from what the panel shows. */
  int bx0 = -1, by0 = -1, bx1 = -1, by1 = -1;
  for (int y = 0; y < height; y++) {
    const unsigned char* next = workLuma + (size_t)y * width;
    const unsigned char* shown = shownLuma + (size_t)y * width;
    if (memcmp(next, shown, width) == 0)
      continue;
    int x0 = 0;
    while (x0 < width && next[x0] == shown[x0]) x0++;
    int x1 = width - 1;
    while (x1 > x0 && next[x1] == shown[x1]) x1--;
    if (bx0 < 0 || x0 < bx0) bx0 = x0;
    if (x1 > bx1) bx1 = x1;
    if (by0 < 0) by0 = y;
    by1 = y;
  }
  if (bx0 >= 0) {
    if (pendingDamage) {
      if (bx0 < damageX0) damageX0 = bx0;
      if (by0 < damageY0) damageY0 = by0;
      if (bx1 > damageX1) damageX1 = bx1;
      if (by1 > damageY1) damageY1 = by1;
    } else {
      damageX0 = bx0;
      damageY0 = by0;
      damageX1 = bx1;
      damageY1 = by1;
      pendingDamage = true;
    }
    statChanged++;
  }
  pthread_mutex_unlock(&frameMutex);
  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_kindle = {
  .setup = kindleSetup,
  .start = NULL,
  .stop = NULL,
  .cleanup = kindleCleanup,
  .submitDecodeUnit = kindleSubmit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC,
};
