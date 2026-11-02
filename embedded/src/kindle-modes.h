/*
 * Rendering mode presets shared by the touch UI (ui-kindle.c) and the
 * streaming renderer (video/kindle.cpp).
 *
 * A rendering mode bundles the three e-ink tuning knobs - panel update
 * interval, ghost-clearing interval and waveform - into names a user can
 * pick on a touch screen:
 *
 *   Fast     - A2 waveform, a panel update about every 30 ms, ghost-clear
 *              flash every 15 s. Black/white look, lowest latency; best for
 *              mouse tracking and typing.
 *   Balanced - waveform picked per damage size (A2/DU/GC16), update about
 *              every 66 ms. The default; good middle ground.
 *   Quality  - GC16 grayscales everywhere, update about every 120 ms, rare
 *              ghost-clear flash. For documents and smooth shading.
 *
 * The accessors keep one authoritative copy of the numbers for both C and
 * C++ translation units.
 */
#ifndef KINDLE_MODES_H
#define KINDLE_MODES_H

enum kindle_render_mode {
  KINDLE_MODE_FAST = 0,
  KINDLE_MODE_BALANCED = 1,
  KINDLE_MODE_QUALITY = 2,
  KINDLE_MODE_COUNT
};

/* Used when nothing was chosen: refresh/fullrefresh/wfm from
 * moonlight.conf (or the command line) apply unchanged. */
#define KINDLE_MODE_CUSTOM (-1)
#define KINDLE_MODE_DEFAULT KINDLE_MODE_BALANCED

/* Kindle streaming is software-decoded; keep requests within the target budget. */
static inline void kindle_limit_stream_size(int *width, int *height) {
  if (!width || !height || *width <= 0 || *height <= 0 ||
      (*width <= 1280 && *height <= 720))
    return;
  if ((long long)*width * 720 > (long long)*height * 1280) {
    int scaled = (int)((long long)*height * 1280 / *width);
    *height = scaled > 0 ? scaled : 1;
    *width = 1280;
  } else {
    int scaled = (int)((long long)*width * 720 / *height);
    *width = scaled > 0 ? scaled : 1;
    *height = 720;
  }
}

/* Kindle software decode is tuned for a maximum 15 fps stream request. */
static inline void kindle_limit_stream_fps(int *fps) {
  if (fps && *fps > 15)
    *fps = 15;
}

/*
 * Rendering backend - where the decoded pixels go:
 *
 *   X11    - XPutImage into the stream window, the Kindle X server blits
 *            to the panel and picks the e-ink waveforms itself. The
 *            reference behavior; identical to upstream Moonlight.
 *   FBink  - decoded pixels are written straight into the framebuffer
 *            (mmap'ed /dev/fb0, skipping the X server entirely) and every
 *            pushed region is refreshed through FBink with an explicit
 *            waveform (A2/DU/GC16 by damage size, flashing GC16 for the
 *            ghost-clearing repaint). No double refreshes, no X server
 *            refresh policy in the way; the low-latency path.
 *
 * Both paths keep the X11 window alive for touch input and the options
 * bar; the video area just changes owners. The backend can be switched
 * at any time while streaming from the options bar.
 */
enum kindle_render_backend {
  KINDLE_BACKEND_X11 = 0,
  KINDLE_BACKEND_FBINK = 1,
  KINDLE_BACKEND_COUNT
};

/* Default pixel path: when FBink is compiled in, direct framebuffer
 * rendering is the low-latency default; without it only X11 exists. */
#ifdef HAVE_FBINK
#define KINDLE_BACKEND_DEFAULT KINDLE_BACKEND_FBINK
#else
#define KINDLE_BACKEND_DEFAULT KINDLE_BACKEND_X11
#endif

static inline const char *kindle_backend_name(int backend) {
  static const char *names[KINDLE_BACKEND_COUNT] = {"X11", "FBink"};
  if (backend < 0 || backend >= KINDLE_BACKEND_COUNT)
    return "X11";
  return names[backend];
}

static inline const char *kindle_backend_hint(int backend) {
  static const char *hints[KINDLE_BACKEND_COUNT] = {
      "X server draws and refreshes (compatible)",
      "direct framebuffer writes, FBink waveforms (fastest)",
  };
  if (backend < 0 || backend >= KINDLE_BACKEND_COUNT)
    return hints[KINDLE_BACKEND_X11];
  return hints[backend];
}

static inline const char *kindle_mode_name(int mode) {
  static const char *names[KINDLE_MODE_COUNT] = {"Fast", "Balanced", "Quality"};
  if (mode < 0 || mode >= KINDLE_MODE_COUNT)
    return "Custom";
  return names[mode];
}

static inline const char *kindle_mode_hint(int mode) {
  static const char *hints[KINDLE_MODE_COUNT] = {
      "fewest gray tones, lowest latency",
      "mixes speed and quality (default)",
      "smooth grayscales, slower updates",
  };
  if (mode < 0 || mode >= KINDLE_MODE_COUNT)
    return "manual settings from moonlight.conf";
  return hints[mode];
}

/* Minimum interval between panel updates, in milliseconds. */
static inline int kindle_mode_refresh_ms(int mode) {
  static const int values[KINDLE_MODE_COUNT] = {30, 66, 120};
  if (mode < 0 || mode >= KINDLE_MODE_COUNT)
    return 0;
  return values[mode];
}

/* Minimum interval between full ghost-clearing flashes, in milliseconds,
 * 0 disables them. The Flash button in the stream options bar can always
 * trigger one on demand. */
static inline int kindle_mode_full_refresh_ms(int mode) {
  static const int values[KINDLE_MODE_COUNT] = {15000, 10000, 30000};
  if (mode < 0 || mode >= KINDLE_MODE_COUNT)
    return 0;
  return values[mode];
}

/* Waveform name as used by moonlight.conf: a2, auto or gc16. */
static inline const char *kindle_mode_wfm(int mode) {
  static const char *values[KINDLE_MODE_COUNT] = {"a2", "auto", "gc16"};
  if (mode < 0 || mode >= KINDLE_MODE_COUNT)
    return "auto";
  return values[mode];
}

#endif
