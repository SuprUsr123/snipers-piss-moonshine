/*
 * Orientation helpers shared by the touch menu (ui-kindle.c) and the
 * stream options bar (video/kindle.cpp).
 *
 * The obvious way to rotate a Kindle screen is
 *
 *     lipc-set-prop com.lab126.winmgr orientationLock U|R|L|D
 *
 * but stock firmware applies that property to the booklet reader only;
 * every other app (including this one) is ignored - the property reads
 * back as set while nothing on the panel moves. The helpers here still
 * send the request (harmless, and older or modified firmware may honor
 * it), but callers MUST NOT assume it worked: they watch the X screen
 * size instead and only act when it really changes.
 *
 * Because the firmware usually does nothing, the actual landscape
 * picture is produced by rotating the video inside this client (the
 * Rotate button in the stream options bar, `rotate` in moonlight.conf);
 * that path is pure software and works on every firmware.
 *
 * Header-only so both the C and the C++ translation units can share it.
 */
#ifndef KINDLE_ORIENT_H
#define KINDLE_ORIENT_H

#include <gtk/gtk.h>
#include <glib.h>

/* Best effort: ask the window manager to lock an orientation.
 * `dir` is one of "U" (portrait), "R" or "L" (landscape) and "D". */
static inline void ink_orient_send(const char *dir) {
  gchar *argv[] = {(gchar *)"lipc-set-prop", (gchar *)"com.lab126.winmgr",
                   (gchar *)"orientationLock", (gchar *)dir, NULL};
  GError *error = NULL;
  if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                     &error)) {
    /* Not an error worth surfacing: firmware without the tool simply
     * never rotates, and the caller's screen watch reports that. */
    g_error_free(error);
  }
}

/* Screen size right now, so callers can detect a rotation afterwards. */
static inline void ink_screen_size(int *width, int *height) {
  GdkScreen *screen = gdk_screen_get_default();
  *width = screen ? gdk_screen_get_width(screen) : 0;
  *height = screen ? gdk_screen_get_height(screen) : 0;
}

typedef gboolean (*ink_orient_changed_fn)(gpointer data);

/* Poll the X screen size every 250 ms for up to `tries` rounds and call
 * `cb(data)` exactly once - as soon as the size changed, or when the
 * wait ran out without a change. The callback decides what to do either
 * way (refit the menu, rebuild the stream view, report that the firmware
 * ignored the rotation, ...).
 *
 * One watch at a time; a new watch replaces a pending one. */
static struct {
  int left;
  int width, height;
  ink_orient_changed_fn cb;
  gpointer data;
  guint id;
} g_ink_orient = {0, 0, 0, NULL, NULL, 0};

static gboolean ink_orient_poll(gpointer unused) {
  (void)unused;
  GdkScreen *screen = gdk_screen_get_default();
  int w = screen ? gdk_screen_get_width(screen) : 0;
  int h = screen ? gdk_screen_get_height(screen) : 0;
  gboolean sizeChanged = (w != g_ink_orient.width || h != g_ink_orient.height);
  if (sizeChanged) {
    g_ink_orient.width = w;
    g_ink_orient.height = h;
  }
  g_ink_orient.left--;
  if (!sizeChanged && g_ink_orient.left > 0)
    return TRUE; /* keep waiting; the panel may still be flipping */
  g_ink_orient.id = 0;
  ink_orient_changed_fn cb = g_ink_orient.cb;
  gpointer data = g_ink_orient.data;
  if (cb != NULL)
    cb(data);
  return FALSE; /* always ends after the callback */
}

static inline void ink_orient_watch(int tries, ink_orient_changed_fn cb,
                                    gpointer data) {
  if (g_ink_orient.id != 0) {
    g_source_remove(g_ink_orient.id);
    g_ink_orient.id = 0;
  }
  ink_screen_size(&g_ink_orient.width, &g_ink_orient.height);
  g_ink_orient.left = tries > 0 ? tries : 1;
  g_ink_orient.cb = cb;
  g_ink_orient.data = data;
  g_ink_orient.id = g_timeout_add(250, ink_orient_poll, NULL);
}

#endif
