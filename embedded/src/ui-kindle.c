#include "ui-kindle.h"
#include "connection.h"
#include "kindle-modes.h"
#include "kindle-style.h"
#include "kindle-orient.h"

#include <client.h>
#include <errors.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The UI is a wizard tuned for a 300 ppi e-ink panel:
 *
 *   Step 1  Connect  - type the PC address on a large keypad (numbers or
 *                      letters), or pick one of the recent addresses, then
 *                      tap Connect.
 *   Step 2  Pair     - shown only when the PC is not yet paired; the PIN is
 *                      displayed huge so it can be typed on the PC.
 *   Step 3  Stream   - the PC's apps as large tappable rows; tap one and
 *                      then Stream. Refresh and Disconnect sit beside it.
 *   Settings         - everything the stream uses, editable on the device:
 *                      resolution, fps, bitrate, rendering mode and backend,
 *                      waveforms, panel timing, menu size, port, toggles.
 *                      Saved to a settings file and applied from the next
 *                      stream on. Includes a PC-help page that explains what
 *                      to fix on the PC when the picture stays black.
 *
 * A header shows the current step, a status line under it explains what is
 * happening, and a bottom bar keeps Settings, rotation and Quit reachable
 * everywhere. Everything is static (no animations or spinners) so the panel
 * only repaints when the user acts or an operation finishes.
 *
 * The look is flat black-on-white with hard outlines (see kindle-style.h):
 * heavy outlines mark primary actions and choices, light outlines mark
 * secondary controls, a gray fill marks the selected choice and a pressed
 * button inverts. Widgets are only restyled or re-texted when their value
 * actually changes, because on e-ink an unnecessary repaint is a visible
 * panel refresh. */

enum ui_operation { UI_IDLE, UI_CONNECT, UI_PAIR, UI_APPS, UI_QUIT };
enum ui_page { PAGE_CONNECT, PAGE_PAIR, PAGE_APPS, PAGE_SETTINGS };

#define MAX_RECENT 3
#define APP_ROWS 6
#define ADDRESS_MAX 63

/* Settings page: one group at a time, reached with the pager. */
enum settings_group {
  SG_VIDEO = 0,   /* resolution, fps, bitrate */
  SG_PANEL,       /* rendering mode, backend, video rotation */
  SG_TIMING,      /* refresh, fullrefresh, waveform, fbink rotation */
  SG_INTERFACE,   /* menu size, CPU governor, logging */
  SG_HOST,        /* port, packet size, stream toggles */
  SG_PCHELP,      /* what to fix on the PC (Wayland capture) */
  SG_COUNT
};

/* Defined in video/kindle.cpp: the video rotation (0/90/180/270) the last
 * stream actually used, so the next stream reuses what fit the panel. */
int kindle_video_get_rotate(void);

/* Base font sizes and widget heights, in pixels for a ~1000 px wide panel.
 * Everything is multiplied by the shared UI scale (see kindle-style.h:
 * screen size x uizoom from moonlight.conf) and the whole layout is shrunk
 * to fit the screen by ui_refit() when a page would overflow. The bases are
 * deliberately smaller than in the previous build - the menu is meant to be
 * zoomed out, so more of a page fits and far less text is cramped. Fonts
 * are applied as absolute pixel sizes, so the outcome no longer depends on
 * the DPI the Kindle X server reports. */
enum ink_font_base {
  F_TITLE = 26,   /* "Moonlight" in the header */
  F_STEP = 24,    /* current step line */
  F_STATUS = 24,  /* the status line everyone reads */
  F_CAPTION = 22, /* small captions */
  F_ENTRY = 30,   /* address box */
  F_KEYPAD = 30,  /* keypad buttons */
  F_MAIN = 30,    /* primary action buttons */
  F_PIN = 110,    /* pairing PIN */
  F_ROW = 26,     /* app list rows, mode and backend rows */
  F_SMALL = 21,   /* summary line */
  F_BOTTOM = 24   /* bottom bar and secondary buttons */
};
enum ink_height_base {
  H_ENTRY = 66, H_RECENT = 64, H_KEY = 88, H_CONNECT = 92, H_PIN = 150,
  H_PAIRBTN = 80, H_ROW = 78, H_PAGER = 64, H_MODE = 64, H_STREAM = 100,
  H_APPSBAR = 72, H_ORIENT = 68, H_QUIT = 68, H_STATUS = 52
};

struct kindle_ui {
  PCONFIGURATION config;
  SERVER_DATA server;
  GtkWidget *window;
  GtkWidget *layout; /* the top-to-bottom vbox, needed by ui_refit */

  /* Persistent chrome. */
  GtkWidget *step_label;
  GtkWidget *status;

  /* Connect page. */
  GtkWidget *page_connect;
  GtkWidget *host;
  GtkWidget *connect;
  GtkWidget *recents[MAX_RECENT];

  /* Pair page. */
  GtkWidget *page_pair;
  GtkWidget *pin_display;
  GtkWidget *pair_again;
  GtkWidget *pair_back;

  /* Apps page. */
  GtkWidget *page_apps;
  GtkWidget *app_rows[APP_ROWS];
  GtkWidget *app_prev;
  GtkWidget *app_next;
  GtkWidget *app_pager;
  GtkWidget *mode_buttons[2][KINDLE_MODE_COUNT];   /* [stream, settings] */
  GtkWidget *backend_buttons[2][KINDLE_BACKEND_COUNT];
  GtkWidget *summary;
  GtkWidget *stream_btn;
  GtkWidget *refresh;
  GtkWidget *disconnect;

  /* Settings page. */
  GtkWidget *page_settings;
  GtkWidget *settings_button;
  enum ui_page settings_return_page;
  GtkWidget *settings_pages[SG_COUNT];
  GtkWidget *settings_title;
  GtkWidget *settings_prev;
  GtkWidget *settings_next;
  GtkWidget *settings_pager;
  int settings_group;
  /* Value labels of the stepper and cycle rows, updated by
   * refresh_setting_labels() after every change. */
  GtkWidget *value_resolution;
  GtkWidget *value_fps;
  GtkWidget *value_bitrate;
  GtkWidget *value_refresh;
  GtkWidget *value_fullrefresh;
  GtkWidget *value_port;
  GtkWidget *value_packetsize;
  GtkWidget *wfm_buttons[5];       /* auto/a2/du/gc16/off */
  GtkWidget *vrotate_buttons[5];   /* auto/0/90/180/270 */
  GtkWidget *fbrotate_buttons[5];  /* auto/0/90/180/270 */
  GtkWidget *uizoom_buttons[5];    /* tiny/small/normal/large/huge */
  GtkWidget *toggle_localaudio;
  GtkWidget *toggle_sops;
  GtkWidget *toggle_quitappafter;
  GtkWidget *toggle_viewonly;
  GtkWidget *toggle_debug;
  GtkWidget *toggle_performance;
  bool performance; /* launch.sh reads this from the settings file */

  /* Connect page keypad: numbers (or letters) mode. The alpha table is
   * no_show_all so ui_refit() measures the page without it. */
  GtkWidget *keys_numeric;
  GtkWidget *keys_alpha;
  bool keypad_alpha;

  /* Bottom bar (rotation + quit). */
  GtkWidget *bottom_bar;
  GtkWidget *orient_row;
  GtkWidget *quit;

  pthread_t worker;
  pthread_mutex_t worker_mutex;
  enum ui_operation operation;
  int operation_result;
  char operation_error[192];
  bool operation_done;
  PAPP_LIST pending_apps;

  char **apps;
  int app_count;
  int app_page;
  int selected_index;
  char selected_app[128];

  char address[ADDRESS_MAX + 1];
  char pin[5];
  char hosts[MAX_RECENT][ADDRESS_MAX + 1];
  int host_count;
  bool busy;
  char end_reason[200]; /* why the last stream ended, shown with the result */
  char saved_app[128];
  bool updating_rows;
  bool updating_modes;
  int render_mode;
  int render_backend;
  int stream_rotate; /* video rotation reused from the last stream, -1 auto */
  enum ui_page page;
};

/* ------------------------------------------------------------- widgets */

/* Every font and widget height goes through the registries below, so
 * ui_refit() can re-apply all of them at a different scale after it has
 * measured the pages against the real screen. Sizes are base values
 * (see the enums above); ink_font_px()/ink_px() apply the scale. */
struct ink_font_ref { GtkWidget *w; int bold; int px; };
struct ink_height_ref { GtkWidget *w; int px; };
static GList *ink_fonts = NULL;
static GList *ink_heights = NULL;

static void ui_font(GtkWidget *w, int bold, int px) {
  for (GList *it = ink_fonts; it != NULL; it = it->next) {
    struct ink_font_ref *r = it->data;
    if (r->w == w) {
      if (r->bold != bold || r->px != px) {
        r->bold = bold;
        r->px = px;
        ink_font_px(w, bold, px);
      }
      return;
    }
  }
  struct ink_font_ref *r = g_new(struct ink_font_ref, 1);
  r->w = w;
  r->bold = bold;
  r->px = px;
  ink_fonts = g_list_prepend(ink_fonts, r);
  ink_font_px(w, bold, px);
}

static void ui_height(GtkWidget *w, int px) {
  for (GList *it = ink_heights; it != NULL; it = it->next) {
    struct ink_height_ref *r = it->data;
    if (r->w == w) {
      if (r->px != px) {
        r->px = px;
        gtk_widget_set_size_request(w, -1, ink_px(px));
      }
      return;
    }
  }
  struct ink_height_ref *r = g_new(struct ink_height_ref, 1);
  r->w = w;
  r->px = px;
  ink_heights = g_list_prepend(ink_heights, r);
  gtk_widget_set_size_request(w, -1, ink_px(px));
}

/* Re-apply every registered font and height at the current scale. */
static void ui_apply_scale(void) {
  for (GList *it = ink_fonts; it != NULL; it = it->next) {
    struct ink_font_ref *r = it->data;
    ink_font_px(r->w, r->bold, r->px);
  }
  for (GList *it = ink_heights; it != NULL; it = it->next) {
    struct ink_height_ref *r = it->data;
    gtk_widget_set_size_request(r->w, -1, ink_px(r->px));
  }
}

static GtkWidget *make_label(const char *text, int bold, int px,
                             float xalign) {
  GtkWidget *label = gtk_label_new(text);
  ui_font(label, bold, px);
  gtk_misc_set_alignment(GTK_MISC(label), xalign, 0.0f);
  return label;
}

/* Plain action button; `border` is INK_BORDER_THICK for primary actions and
 * INK_BORDER_THIN for secondary ones. */
static GtkWidget *make_button(const char *text, int bold, int px, int border) {
  GtkWidget *button = gtk_button_new_with_label(text);
  ink_style_button(button, INK_NAME_BUTTON, border);
  ui_font(gtk_bin_get_child(GTK_BIN(button)), bold, px);
  return button;
}

/* Choice button: gray fill while selected. */
static GtkWidget *make_toggle(const char *text, int bold, int px) {
  GtkWidget *button = gtk_toggle_button_new_with_label(text);
  ink_style_button(button, INK_NAME_TOGGLE, INK_BORDER_THICK);
  ui_font(gtk_bin_get_child(GTK_BIN(button)), bold, px);
  return button;
}

/* gtk_label_set_text repaints the label even when the text is identical;
 * on e-ink that is a pointless panel refresh. */
static void label_set(GtkWidget *label, const char *text) {
  const char *current = gtk_label_get_text(GTK_LABEL(label));
  if (current != NULL && strcmp(current, text) == 0)
    return;
  gtk_label_set_text(GTK_LABEL(label), text);
}

static void set_status(struct kindle_ui *ui, const char *message) {
  label_set(ui->status, message);
}

/* ---------------------------------------------------------------- state */

static void state_path(struct kindle_ui *ui, char *path, size_t size) {
  snprintf(path, size, "%s/ui-state", ui->config->key_dir);
}

/* Recent hosts are kept next to the pairing keys so the keypad is only
 * needed when the PC's address actually changes. The newest address is
 * first in the list. */
static void update_recents(struct kindle_ui *ui) {
  for (int i = 0; i < MAX_RECENT; i++) {
    if (i < ui->host_count) {
      gtk_button_set_label(GTK_BUTTON(ui->recents[i]), ui->hosts[i]);
      ui_font(gtk_bin_get_child(GTK_BIN(ui->recents[i])), 0, F_BOTTOM);
      gtk_widget_show(ui->recents[i]);
    } else {
      gtk_widget_hide(ui->recents[i]);
    }
  }
}

static void add_recent(struct kindle_ui *ui, const char *address) {
  if (address[0] == 0)
    return;
  int existing = -1;
  for (int i = 0; i < ui->host_count; i++) {
    if (strcmp(ui->hosts[i], address) == 0) {
      existing = i;
      break;
    }
  }
  if (existing < 0 && ui->host_count < MAX_RECENT)
    ui->host_count++;
  int last = (existing >= 0) ? existing : ui->host_count - 1;
  for (int i = last; i > 0; i--)
    memcpy(ui->hosts[i], ui->hosts[i - 1], sizeof(ui->hosts[0]));
  snprintf(ui->hosts[0], sizeof(ui->hosts[0]), "%s", address);
  update_recents(ui);
}

static void load_state(struct kindle_ui *ui) {
  char path[4096 + 32];
  state_path(ui, path, sizeof(path));
  FILE *fd = fopen(path, "r");
  if (fd == NULL)
    return;

  char line[256];
  while (fgets(line, sizeof(line), fd) != NULL) {
    char value[128];
    int mode;
    if (sscanf(line, "address=%63[^\n]", value) == 1)
      add_recent(ui, value);
    else if (sscanf(line, "app=%127[^\n]", value) == 1)
      snprintf(ui->saved_app, sizeof(ui->saved_app), "%s", value);
    else if (sscanf(line, "mode=%d", &mode) == 1 && mode >= 0 &&
             mode < KINDLE_MODE_COUNT)
      ui->render_mode = mode;
    else if (sscanf(line, "backend=%d", &mode) == 1 && mode >= 0 &&
             mode < KINDLE_BACKEND_COUNT)
      ui->render_backend = mode;
    else if (sscanf(line, "rotate=%d", &mode) == 1 && mode >= 0 &&
             mode <= 270 && mode % 90 == 0)
      ui->stream_rotate = mode;
  }
  fclose(fd);

  if (ui->host_count > 0)
    gtk_entry_set_text(GTK_ENTRY(ui->host), ui->hosts[0]);
  update_recents(ui);
}

static void save_state(struct kindle_ui *ui, const char *app) {
  char path[4096 + 32];
  state_path(ui, path, sizeof(path));
  FILE *fd = fopen(path, "w");
  if (fd == NULL)
    return;
  for (int i = 0; i < ui->host_count; i++)
    fprintf(fd, "address=%s\n", ui->hosts[i]);
  /* Keep the last streamed app unless a newer one is given. */
  const char *stored = (app != NULL && app[0]) ? app : ui->saved_app;
  if (stored != NULL && stored[0])
    fprintf(fd, "app=%s\n", stored);
  fprintf(fd, "mode=%d\n", ui->render_mode);
  fprintf(fd, "backend=%d\n", ui->render_backend);
  fprintf(fd, "rotate=%d\n", ui->stream_rotate);
  fclose(fd);
}

/* ------------------------------------------------------ settings file
 *
 * Everything the Settings page edits is persisted to <keydir>/settings in
 * moonlight.conf's `key = value` syntax. It is applied after
 * moonlight.conf, so a choice made in the GUI wins. Deleting a line from
 * the file (or the whole file) falls back to moonlight.conf. The
 * rendering mode/backend/rotate quick choices stay in ui-state, where
 * they have always lived.
 *
 * `performance` is read by launch.sh (CPU governor) and parsed here only
 * so the toggle round-trips; the C side never applies it itself. */

static void settings_path(struct kindle_ui *ui, char *path, size_t size) {
  snprintf(path, size, "%s/settings", ui->config->key_dir);
}

/* The automatic bitrate table from config.c, kept in sync: after the GUI
 * changes resolution or fps with bitrate = -1 (Auto), the same result the
 * CLI path would produce is computed here. */
static void recompute_auto_bitrate(struct kindle_ui *ui) {
  STREAM_CONFIGURATION *s = &ui->config->stream;
  if (s->bitrate != -1)
    return;
  if (s->width * s->height <= 640 * 360)
    s->bitrate = (int)(1000 * (s->fps / 30.0));
  else if (s->width * s->height <= 854 * 480)
    s->bitrate = (int)(1500 * (s->fps / 30.0));
  else if (s->width * s->height <= 1366 * 768)
    s->bitrate = (int)(5000 * (s->fps / 30.0));
  else if (s->width * s->height <= 1920 * 1200)
    s->bitrate = (int)(10000 * (s->fps / 30.0));
  else if (s->width * s->height <= 2560 * 1600)
    s->bitrate = (int)(20000 * (s->fps / 30.0));
  else
    s->bitrate = (int)(40000 * (s->fps / 30.0));
}

/* Load the settings file into the config. Runs before ui-state so the
 * quick choices (mode/backend/rotate) still win where they overlap. */
static void load_settings(struct kindle_ui *ui) {
  ui->performance = true;
  char path[4096 + 32];
  settings_path(ui, path, sizeof(path));
  FILE *fd = fopen(path, "r");
  if (fd == NULL)
    return;

  char line[256];
  while (fgets(line, sizeof(line), fd) != NULL) {
    char key[40], value[128];
    if (sscanf(line, " %39[^=] = %127[^\n]", key, value) != 2)
      continue;
    /* Trim trailing spaces from the key (`key = value`). */
    for (char *end = key + strlen(key);
         end > key && (end[-1] == ' ' || end[-1] == '\t'); end--)
      end[-1] = 0;
    STREAM_CONFIGURATION *s = &ui->config->stream;
    if (strcmp(key, "port") == 0)
      ui->config->port = (unsigned short)atoi(value);
    else if (strcmp(key, "width") == 0 && atoi(value) > 0)
      s->width = atoi(value);
    else if (strcmp(key, "height") == 0 && atoi(value) > 0)
      s->height = atoi(value);
    else if (strcmp(key, "fps") == 0 && atoi(value) > 0)
      s->fps = atoi(value);
    else if (strcmp(key, "bitrate") == 0)
      s->bitrate = atoi(value);
    else if (strcmp(key, "packetsize") == 0 && atoi(value) >= 1024)
      s->packetSize = atoi(value);
    else if (strcmp(key, "refresh") == 0 && atoi(value) > 0)
      ui->config->refresh_ms = atoi(value);
    else if (strcmp(key, "fullrefresh") == 0 && atoi(value) >= 0)
      ui->config->full_refresh_ms = atoi(value);
    else if (strcmp(key, "wfm") == 0)
      ui->config->wfm =
          (strcmp(value, "a2") == 0 || strcmp(value, "du") == 0 ||
           strcmp(value, "gc16") == 0 || strcmp(value, "off") == 0)
              ? (strcmp(value, "a2") == 0
                     ? "a2"
                     : strcmp(value, "du") == 0
                           ? "du"
                           : strcmp(value, "gc16") == 0
                                 ? "gc16"
                                 : strcmp(value, "off") == 0 ? "off" : "auto")
              : "auto";
    else if (strcmp(key, "fbinkrotate") == 0) {
      int v = atoi(value);
      ui->config->fbink_rotate =
          (v == 0 || v == 90 || v == 180 || v == 270) ? v : -1;
    } else if (strcmp(key, "uizoom") == 0)
      ui->config->uizoom = atof(value);
    else if (strcmp(key, "localaudio") == 0)
      ui->config->localaudio = strcmp(value, "true") == 0;
    else if (strcmp(key, "sops") == 0)
      ui->config->sops = strcmp(value, "true") == 0;
    else if (strcmp(key, "quitappafter") == 0)
      ui->config->quitappafter = strcmp(value, "true") == 0;
    else if (strcmp(key, "viewonly") == 0)
      ui->config->viewonly = strcmp(value, "true") == 0;
    else if (strcmp(key, "debug") == 0)
      ui->config->debug_level = strcmp(value, "true") == 0 ? 1 : 0;
    else if (strcmp(key, "performance") == 0)
      ui->performance = strcmp(value, "true") == 0;
  }
  fclose(fd);
  kindle_limit_stream_size(&ui->config->stream.width,
                           &ui->config->stream.height);
  kindle_limit_stream_fps(&ui->config->stream.fps);
  recompute_auto_bitrate(ui);
}

static void save_settings(struct kindle_ui *ui) {
  char path[4096 + 32];
  settings_path(ui, path, sizeof(path));
  FILE *fd = fopen(path, "w");
  if (fd == NULL)
    return;
  STREAM_CONFIGURATION *s = &ui->config->stream;
  fprintf(fd, "port = %u\n", (unsigned)ui->config->port);
  fprintf(fd, "width = %d\n", s->width);
  fprintf(fd, "height = %d\n", s->height);
  fprintf(fd, "fps = %d\n", s->fps);
  fprintf(fd, "bitrate = %d\n", s->bitrate);
  fprintf(fd, "packetsize = %d\n", s->packetSize);
  fprintf(fd, "refresh = %d\n", ui->config->refresh_ms);
  fprintf(fd, "fullrefresh = %d\n", ui->config->full_refresh_ms);
  fprintf(fd, "wfm = %s\n",
          ui->config->wfm != NULL ? ui->config->wfm : "auto");
  fprintf(fd, "fbinkrotate = %d\n", ui->config->fbink_rotate);
  fprintf(fd, "uizoom = %.2f\n", ui->config->uizoom);
  fprintf(fd, "localaudio = %s\n", ui->config->localaudio ? "true" : "false");
  fprintf(fd, "sops = %s\n", ui->config->sops ? "true" : "false");
  fprintf(fd, "quitappafter = %s\n",
          ui->config->quitappafter ? "true" : "false");
  fprintf(fd, "viewonly = %s\n", ui->config->viewonly ? "true" : "false");
  fprintf(fd, "debug = %s\n", ui->config->debug_level > 0 ? "true" : "false");
  fprintf(fd, "performance = %s\n", ui->performance ? "true" : "false");
  fclose(fd);
}

/* ------------------------------------------------------- page and busy */

static void show_page(struct kindle_ui *ui, enum ui_page page);
static void refresh_setting_labels(struct kindle_ui *ui);
static void update_recents(struct kindle_ui *ui);
static void update_app_rows(struct kindle_ui *ui);

/* Settings page: exactly one group visible. The group boxes are created
 * with gtk_widget_set_no_show_all(), so show_all() never un-hides them
 * behind our back (important while ui_refit() measures the layout).
 * The reveal must be show_all(): every child widget is created hidden,
 * and a plain gtk_widget_show() on the group box would map only the box
 * itself - the page would render as the pager above six empty groups. */
static void show_settings_group(struct kindle_ui *ui, int group) {
  if (group < 0)
    group = 0;
  if (group >= SG_COUNT)
    group = SG_COUNT - 1;
  ui->settings_group = group;
  for (int i = 0; i < SG_COUNT; i++) {
    if (ui->settings_pages[i] == NULL)
      continue;
    if (i == group) {
      gtk_widget_set_no_show_all(ui->settings_pages[i], FALSE);
      gtk_widget_show_all(ui->settings_pages[i]);
      gtk_widget_set_no_show_all(ui->settings_pages[i], TRUE);
    } else {
      gtk_widget_hide(ui->settings_pages[i]);
    }
  }
  static const char *names[SG_COUNT] = {
      "Video", "E-ink panel", "Panel timing", "Interface",
      "Host and stream", "PC help"};
  char title[128];
  snprintf(title, sizeof(title), "%d/%d - %s", group + 1, SG_COUNT,
           names[group]);
  label_set(ui->settings_title, title);
  gtk_widget_set_sensitive(ui->settings_prev, group > 0);
  gtk_widget_set_sensitive(ui->settings_next, group < SG_COUNT - 1);
}

static void show_page(struct kindle_ui *ui, enum ui_page page) {
  ui->page = page;
  gtk_widget_hide(ui->page_connect);
  gtk_widget_hide(ui->page_pair);
  gtk_widget_hide(ui->page_apps);
  gtk_widget_hide(ui->page_settings);
  const char *step = "Step 1 of 3 - Connect to your PC";
  switch (page) {
    case PAGE_CONNECT:
      gtk_widget_show(ui->page_connect);
      break;
    case PAGE_PAIR:
      gtk_widget_show(ui->page_pair);
      step = "Step 2 of 3 - Pair with your PC";
      break;
    case PAGE_APPS:
      gtk_widget_show(ui->page_apps);
      step = "Step 3 of 3 - Choose an app to stream";
      break;
    case PAGE_SETTINGS:
      gtk_widget_show(ui->page_settings);
      step = "Settings - used by the next stream";
      show_settings_group(ui, ui->settings_group);
      break;
  }
  label_set(ui->step_label, step);
  gtk_button_set_label(GTK_BUTTON(ui->settings_button),
                       page == PAGE_SETTINGS ? "Back" : "Settings");
}

/* After ui_refit()'s measuring show_all() every visibility decision has to
 * be made again: hidden recents, hidden app rows, the active page and the
 * active settings group. Without this a first run would show empty
 * "ghost" rows for hosts and apps that do not exist. */
static void ui_restore_visibility(struct kindle_ui *ui) {
  show_page(ui, ui->page);
  update_recents(ui);
  update_app_rows(ui);
  refresh_setting_labels(ui);
}

/* While a request runs every handler ignores taps (they all test ui->busy),
 * and only the page's main action buttons dim, which is a small, local
 * repaint. Graying whole pages, as the first version did, repainted the
 * entire panel twice per request (once to dim, once to restore). The status
 * line says what is happening. Quit stays active on purpose: a touchscreen
 * user must always be able to escape, even when a request hangs. */
static void set_busy_state(struct kindle_ui *ui, bool busy) {
  ui->busy = busy;
  gtk_widget_set_sensitive(ui->connect, !busy);
  gtk_widget_set_sensitive(ui->pair_again, !busy);
  gtk_widget_set_sensitive(ui->pair_back, !busy);
  gtk_widget_set_sensitive(ui->stream_btn, !busy);
  gtk_widget_set_sensitive(ui->refresh, !busy);
  gtk_widget_set_sensitive(ui->disconnect, !busy);
}

/* --------------------------------------------------------------- refit */

/* Shrink the shared UI scale until the largest page fits the real screen.
 * Runs before the window is first shown (the pages are measured unrealized,
 * so nothing repaints) and again after a rotation that really changed the
 * screen. Three bounded passes: fonts shrink linearly, but wrapped labels
 * can settle onto fewer lines and need one more round. */
static void ui_refit(struct kindle_ui *ui) {
  GdkScreen *screen = gdk_screen_get_default();
  int screen_w = screen ? gdk_screen_get_width(screen) : 0;
  int screen_h = screen ? gdk_screen_get_height(screen) : 0;
  if (screen_w <= 0 || screen_h <= 0)
    return;
  int selected_group = ui->settings_group;
  for (int pass = 0; pass < 3; pass++) {
    gtk_widget_show_all(ui->layout);
    gtk_widget_hide(ui->page_connect);
    gtk_widget_hide(ui->page_pair);
    gtk_widget_hide(ui->page_apps);
    gtk_widget_hide(ui->page_settings);
    int req_w = 0, req_h = 0;
    GtkWidget *pages[] = {ui->page_connect, ui->page_pair, ui->page_apps};
    for (unsigned i = 0; i < G_N_ELEMENTS(pages); i++) {
      gtk_widget_show_all(pages[i]);
      GtkRequisition req;
      gtk_widget_size_request(ui->window, &req);
      if (req.width > req_w) req_w = req.width;
      if (req.height > req_h) req_h = req.height;
      gtk_widget_hide(pages[i]);
    }
    gtk_widget_show(ui->page_settings);
    for (int group = 0; group < SG_COUNT; group++) {
      show_settings_group(ui, group);
      GtkRequisition req;
      gtk_widget_size_request(ui->window, &req);
      if (req.width > req_w) req_w = req.width;
      if (req.height > req_h) req_h = req.height;
    }
    GtkRequisition req = {req_w, req_h};
    ui->settings_group = selected_group;
    ui_restore_visibility(ui); /* back to the current page and tab */
    if (req.width <= screen_w && req.height <= screen_h)
      break;
    double fit_w = (double)screen_w / (double)req.width;
    double fit_h = (double)screen_h / (double)req.height;
    double shrink = (fit_w < fit_h ? fit_w : fit_h) * 0.97;
    if (shrink < 0.75)
      shrink = 0.75; /* never shrink into unreadably tiny text */
    ink_zoom_set(ink_zoom_get() * shrink);
    ui_apply_scale();
  }
  /* The zoom may have been shrunk to fit; keep the stored value honest so
   * the Settings page shows what is really in effect. */
  ui->config->uizoom = ink_zoom_get();
}

/* --------------------------------------------------------------- keypad
 *
 * Two layouts over the same handlers: digits (IP addresses) and letters
 * (hostnames like desktop.local). The 123/ABC keys flip between the two
 * tables; hiding one instead of destroying it keeps the e-ink repaint to
 * a single region. */

static void keypad_set_alpha(struct kindle_ui *ui, bool alpha) {
  if (ui->keys_numeric == NULL || ui->keys_alpha == NULL)
    return;
  ui->keypad_alpha = alpha;
  /* show_all, not show: the alpha table is no_show_all (it must stay out
   * of the global show_all walks), so its letter buttons are created
   * hidden and need the recursive reveal when the keypad flips. */
  if (alpha) {
    gtk_widget_hide(ui->keys_numeric);
    gtk_widget_show_all(ui->keys_alpha);
  } else {
    gtk_widget_hide(ui->keys_alpha);
    gtk_widget_show_all(ui->keys_numeric);
  }
}

static void keypad_input(struct kindle_ui *ui, char key) {
  if (ui->busy || ui->page != PAGE_CONNECT)
    return;
  const char *old = gtk_entry_get_text(GTK_ENTRY(ui->host));
  size_t length = strlen(old);
  if (length >= ADDRESS_MAX)
    return;
  char address[ADDRESS_MAX + 1];
  memcpy(address, old, length);
  address[length] = key;
  address[length + 1] = 0;
  gtk_entry_set_text(GTK_ENTRY(ui->host), address);
}

static void keypad_backspace(struct kindle_ui *ui) {
  if (ui->busy || ui->page != PAGE_CONNECT)
    return;
  const char *old = gtk_entry_get_text(GTK_ENTRY(ui->host));
  size_t length = strlen(old);
  if (length == 0)
    return;
  char address[ADDRESS_MAX + 1];
  memcpy(address, old, length - 1);
  address[length - 1] = 0;
  gtk_entry_set_text(GTK_ENTRY(ui->host), address);
}

static void keypad_clear(struct kindle_ui *ui) {
  if (ui->busy || ui->page != PAGE_CONNECT)
    return;
  gtk_entry_set_text(GTK_ENTRY(ui->host), "");
}

static void key_pressed(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  const char *key = g_object_get_data(G_OBJECT(button), "key");
  if (key == NULL)
    return;
  if (strcmp(key, "back") == 0)
    keypad_backspace(ui);
  else if (strcmp(key, "clear") == 0)
    keypad_clear(ui);
  else if (strcmp(key, "mode") == 0)
    keypad_set_alpha(ui, !ui->keypad_alpha);
  else
    keypad_input(ui, key[0]);
}

static GtkWidget *key_button(struct kindle_ui *ui, const char *key,
                             const char *label, GtkWidget *grid,
                             int left, int right, int top) {
  GtkWidget *button = gtk_button_new_with_label(label);
  ink_style_button(button, INK_NAME_BUTTON, INK_BORDER_THICK);
  g_object_set_data(G_OBJECT(button), "key", (gpointer)key);
  ui_font(gtk_bin_get_child(GTK_BIN(button)), 1, F_KEYPAD);
  ui_height(button, H_KEY);
  gtk_table_attach_defaults(GTK_TABLE(grid), button, left, right, top, top + 1);
  g_signal_connect(button, "clicked", G_CALLBACK(key_pressed), ui);
  return button;
}

/* ------------------------------------------------------------- app list */

static void free_apps(PAPP_LIST apps) {
  while (apps != NULL) {
    PAPP_LIST next = apps->next;
    free(apps->name);
    free(apps);
    apps = next;
  }
}

static void store_apps(struct kindle_ui *ui, PAPP_LIST list) {
  int count = 0;
  for (PAPP_LIST app = list; app != NULL; app = app->next)
    count++;

  char **names = NULL;
  if (count > 0) {
    names = calloc(count, sizeof(char *));
    if (names != NULL) {
      int index = 0;
      for (PAPP_LIST app = list; app != NULL && index < count;
           app = app->next, index++) {
        size_t length = strlen(app->name);
        if (length > 127)
          length = 127;
        names[index] = malloc(length + 1);
        if (names[index] != NULL) {
          memcpy(names[index], app->name, length);
          names[index][length] = 0;
        }
      }
    }
  }
  free_apps(list);

  if (ui->apps != NULL) {
    for (int i = 0; i < ui->app_count; i++)
      free(ui->apps[i]);
    free(ui->apps);
  }
  ui->apps = names;
  ui->app_count = (names != NULL) ? count : 0;
  ui->app_page = 0;
}

static void update_app_rows(struct kindle_ui *ui) {
  ui->updating_rows = true;
  int first = ui->app_page * APP_ROWS;
  for (int i = 0; i < APP_ROWS; i++) {
    GtkWidget *row = ui->app_rows[i];
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(row));
    int index = first + i;
    if (index < ui->app_count) {
      label_set(label, ui->apps[index]);
      ui_font(label, index == ui->selected_index, F_ROW);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row),
                                   index == ui->selected_index);
      gtk_widget_show(row);
    } else {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row), FALSE);
      gtk_widget_hide(row);
    }
  }

  char pager[96];
  if (ui->app_count == 0) {
    snprintf(pager, sizeof(pager), "No apps found on the PC");
    gtk_widget_set_sensitive(ui->app_prev, FALSE);
    gtk_widget_set_sensitive(ui->app_next, FALSE);
  } else if (ui->app_count <= APP_ROWS) {
    snprintf(pager, sizeof(pager), "%d app%s on the PC", ui->app_count,
             ui->app_count == 1 ? "" : "s");
    gtk_widget_set_sensitive(ui->app_prev, FALSE);
    gtk_widget_set_sensitive(ui->app_next, FALSE);
  } else {
    int shown_end = first + APP_ROWS;
    if (shown_end > ui->app_count)
      shown_end = ui->app_count;
    snprintf(pager, sizeof(pager), "Showing %d-%d of %d apps", first + 1,
             shown_end, ui->app_count);
    gtk_widget_set_sensitive(ui->app_prev, ui->app_page > 0);
    gtk_widget_set_sensitive(ui->app_next,
                             (ui->app_page + 1) * APP_ROWS < ui->app_count);
  }
  label_set(ui->app_pager, pager);
  ui->updating_rows = false;
}

static void select_app(struct kindle_ui *ui, int index) {
  ui->selected_index = index;
  if (index >= 0 && index < ui->app_count && ui->apps[index] != NULL)
    snprintf(ui->selected_app, sizeof(ui->selected_app), "%s",
             ui->apps[index]);
  else
    ui->selected_app[0] = 0;
  update_app_rows(ui);
}

/* Keep the current selection across a refresh, fall back to the last
 * streamed app, then to the first entry. */
static void select_default_app(struct kindle_ui *ui) {
  int index = -1;
  const char *wanted = ui->selected_app[0] ? ui->selected_app : ui->saved_app;
  if (wanted[0]) {
    for (int i = 0; i < ui->app_count; i++) {
      if (ui->apps[i] != NULL && strcmp(ui->apps[i], wanted) == 0) {
        index = i;
        break;
      }
    }
  }
  if (index < 0 && ui->app_count > 0)
    index = 0;
  select_app(ui, index);
}

static void app_row_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_rows)
    return;
  if (ui->busy) {
    /* The button flipped itself before this handler ran; put it back. */
    update_app_rows(ui);
    return;
  }
  int row = (int)(long)g_object_get_data(G_OBJECT(button), "row");
  int index = ui->app_page * APP_ROWS + row;
  if (index >= ui->app_count || ui->apps[index] == NULL)
    return;
  /* Tapping the selected row keeps it selected: exactly one row stays
   * highlighted, so Stream always has a target. */
  select_app(ui, index);
  char status[192];
  snprintf(status, sizeof(status), "Selected: %s. Tap Stream to start.",
           ui->apps[index]);
  set_status(ui, status);
}

static void app_prev_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy || ui->updating_rows)
    return;
  if (ui->app_page > 0) {
    ui->app_page--;
    update_app_rows(ui);
  }
}

static void app_next_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy || ui->updating_rows)
    return;
  if ((ui->app_page + 1) * APP_ROWS < ui->app_count) {
    ui->app_page++;
    update_app_rows(ui);
  }
}

static void update_summary(struct kindle_ui *ui) {
  char line[224];
  line[0] = 0;
  int refresh = (ui->render_mode >= 0 && ui->render_mode < KINDLE_MODE_COUNT)
                    ? kindle_mode_refresh_ms(ui->render_mode)
                    : ui->config->refresh_ms;
  char rotated[32];
  if (ui->stream_rotate > 0)
    snprintf(rotated, sizeof(rotated), " | video rotated %d", ui->stream_rotate);
  else
    rotated[0] = 0;
  if (ui->config->stream.width > 0 && ui->config->stream.height > 0 &&
      ui->config->stream.fps > 0) {
    if (refresh > 0)
      snprintf(line, sizeof(line),
               "%s via %s | %d x %d%s | %d fps  |  panel update every %d ms",
               kindle_mode_name(ui->render_mode),
               kindle_backend_name(ui->render_backend),
               ui->config->stream.width,
               ui->config->stream.height, rotated, ui->config->stream.fps,
               refresh);
    else
      snprintf(line, sizeof(line), "%s via %s | %d x %d%s | %d fps",
               kindle_mode_name(ui->render_mode),
               kindle_backend_name(ui->render_backend),
               ui->config->stream.width,
               ui->config->stream.height, rotated, ui->config->stream.fps);
  } else if (refresh > 0) {
    snprintf(line, sizeof(line), "%s via %s | panel update every %d ms",
             kindle_mode_name(ui->render_mode),
             kindle_backend_name(ui->render_backend), refresh);
  }
  if (line[0]) {
    label_set(ui->summary, line);
    gtk_widget_show(ui->summary);
  } else {
    gtk_widget_hide(ui->summary);
  }
}

/* ------------------------------------------------------- rendering modes
 *
 * Three presets (Fast/Balanced/Quality) shown as toggle buttons on the
 * apps page AND on the Settings page (button sets 0 and 1). The choice is
 * saved with the rest of the UI state and handed to the stream when it
 * starts; while streaming, the options bar can switch modes again without
 * leaving the session. */

static void update_mode_buttons(struct kindle_ui *ui) {
  ui->updating_modes = true;
  for (int set = 0; set < 2; set++) {
    for (int i = 0; i < KINDLE_MODE_COUNT; i++) {
      GtkWidget *button = ui->mode_buttons[set][i];
      if (button == NULL)
        continue;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button),
                                   i == ui->render_mode);
      ui_font(gtk_bin_get_child(GTK_BIN(button)), i == ui->render_mode,
              F_ROW);
    }
  }
  ui->updating_modes = false;
}

static void mode_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  if (ui->busy) {
    update_mode_buttons(ui);
    return;
  }
  int mode = (int)(long)g_object_get_data(G_OBJECT(button), "mode");
  /* Tapping the active mode keeps it: exactly one button stays pressed. */
  if (mode == ui->render_mode) {
    update_mode_buttons(ui);
    return;
  }
  ui->render_mode = mode;
  /* The preset numbers land in the config immediately, so the Settings
   * timing rows and the summary always show what the stream will really
   * use; launch_app() re-applies them, unchanged. */
  if (mode >= 0 && mode < KINDLE_MODE_COUNT) {
    ui->config->refresh_ms = kindle_mode_refresh_ms(mode);
    ui->config->full_refresh_ms = kindle_mode_full_refresh_ms(mode);
    ui->config->wfm = (char *)kindle_mode_wfm(mode);
  }
  update_mode_buttons(ui);
  refresh_setting_labels(ui);
  update_summary(ui);
  save_state(ui, NULL);
  save_settings(ui);
  char status[192];
  snprintf(status, sizeof(status), "Rendering mode: %s (%s).",
           kindle_mode_name(mode), kindle_mode_hint(mode));
  set_status(ui, status);
}

/* ---------------------------------------------------- rendering backends
 *
 * X11 or FBink pixel path, shown as toggle buttons next to the mode row
 * (apps page and Settings page). The choice is saved with the rest of the
 * UI state and handed to the stream when it starts; while streaming, the
 * options bar can flip the backend at any time without leaving the
 * session. */

static void update_backend_buttons(struct kindle_ui *ui) {
  ui->updating_modes = true;
  for (int set = 0; set < 2; set++) {
    for (int i = 0; i < KINDLE_BACKEND_COUNT; i++) {
      GtkWidget *button = ui->backend_buttons[set][i];
      if (button == NULL)
        continue; /* built without FBink support */
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button),
                                   i == ui->render_backend);
      ui_font(gtk_bin_get_child(GTK_BIN(button)), i == ui->render_backend,
              F_ROW);
    }
  }
  ui->updating_modes = false;
}

static void backend_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  if (ui->busy) {
    update_backend_buttons(ui);
    return;
  }
  int backend = (int)(long)g_object_get_data(G_OBJECT(button), "backend");
  /* Tapping the active backend keeps it: exactly one button stays
   * pressed. */
  if (backend == ui->render_backend) {
    update_backend_buttons(ui);
    return;
  }
  ui->render_backend = backend;
  update_backend_buttons(ui);
  refresh_setting_labels(ui);
  update_summary(ui);
  save_state(ui, NULL);
  char status[192];
  snprintf(status, sizeof(status), "Rendering via %s: %s.",
           kindle_backend_name(backend), kindle_backend_hint(backend));
  set_status(ui, status);
}

/* ------------------------------------------------------- settings page
 *
 * Every value the stream uses, editable on the device with big touch
 * targets, grouped into six pager pages:
 *
 *   1 Video        resolution, fps, bitrate
 *   2 E-ink panel  rendering mode, backend, video rotation
 *   3 Panel timing refresh, fullrefresh, waveform, FBink rotation
 *   4 Interface    menu size, CPU governor, verbose logging
 *   5 Host/stream  port, packet size, stream toggles
 *   6 PC help      what to fix on the PC when the picture stays black
 *
 * Numeric rows are [- value +] steppers with press-and-hold repeat;
 * choices are rows of toggle buttons; on/off values are single toggles.
 * Every change is saved to <keydir>/settings immediately and used by the
 * next stream - nothing here needs a rebuild or a PC visit. */

/* Discrete tables behind the resolution / fps / bitrate rows. A value
 * outside the table (set in moonlight.conf) shows as "custom" and the
 * first tap snaps into the table. */
static const struct { int w, h; } RES_PRESETS[] = {
    {640, 360}, {800, 500}, {960, 540}, {1024, 640}, {1280, 720}};
#define RES_PRESET_COUNT (int)(sizeof(RES_PRESETS) / sizeof(RES_PRESETS[0]))
static const int FPS_PRESETS[] = {5, 10, 15};
#define FPS_PRESET_COUNT (int)(sizeof(FPS_PRESETS) / sizeof(FPS_PRESETS[0]))
static const int BITRATE_PRESETS[] = {-1,   500,  1000, 1500, 2000,
                                      3000, 5000, 8000, 10000, 15000,
                                      20000};
#define BITRATE_PRESET_COUNT \
  (int)(sizeof(BITRATE_PRESETS) / sizeof(BITRATE_PRESETS[0]))

/* Press-and-hold auto-repeat for the stepper buttons: the first tap steps
 * once through the normal "clicked" handler, holding repeats at ~8/s
 * after a short pause. */
static gboolean stepper_repeat(gpointer data) {
  gtk_button_clicked(GTK_BUTTON(data));
  return TRUE;
}

static gboolean stepper_repeat_start(gpointer data) {
  GtkWidget *button = GTK_WIDGET(data);
  guint fast = g_timeout_add(120, stepper_repeat, button);
  g_object_set_data(G_OBJECT(button), "repeat-fast", GUINT_TO_POINTER(fast));
  return FALSE;
}

static gboolean stepper_pressed(GtkWidget *button, GdkEventButton *event,
                                gpointer data) {
  (void)event;
  (void)data;
  guint slow = g_timeout_add(420, stepper_repeat_start, button);
  g_object_set_data(G_OBJECT(button), "repeat-slow", GUINT_TO_POINTER(slow));
  return FALSE; /* the normal clicked handler performs the first step */
}

static gboolean stepper_released(GtkWidget *button, GdkEventButton *event,
                                 gpointer data) {
  (void)event;
  (void)data;
  gpointer slow = g_object_steal_data(G_OBJECT(button), "repeat-slow");
  gpointer fast = g_object_steal_data(G_OBJECT(button), "repeat-fast");
  if (slow != NULL)
    g_source_remove(GPOINTER_TO_UINT(slow));
  if (fast != NULL)
    g_source_remove(GPOINTER_TO_UINT(fast));
  return FALSE;
}

static void stepper_button(GtkWidget *button) {
  g_signal_connect(button, "button-press-event", G_CALLBACK(stepper_pressed),
                   NULL);
  g_signal_connect(button, "button-release-event",
                   G_CALLBACK(stepper_released), NULL);
}

/* One handler for every stepper and cycle row: `which` says the row, `dir`
 * is +1 or -1. Each branch steps or cycles its own value with clamping,
 * then refreshes the row labels and saves. Manual timing edits drop the
 * rendering preset to Custom (that is what Custom means). */
static void settings_step_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  const char *which = g_object_get_data(G_OBJECT(button), "which");
  int dir = (int)(long)g_object_get_data(G_OBJECT(button), "dir");
  STREAM_CONFIGURATION *s = &ui->config->stream;

  if (strcmp(which, "res") == 0) {
    int index = -1;
    for (int i = 0; i < RES_PRESET_COUNT; i++)
      if (RES_PRESETS[i].w == s->width && RES_PRESETS[i].h == s->height) {
        index = i;
        break;
      }
    index = (index < 0) ? (dir > 0 ? 0 : RES_PRESET_COUNT - 1)
                        : index + dir;
    if (index < 0)
      index = RES_PRESET_COUNT - 1;
    if (index >= RES_PRESET_COUNT)
      index = 0;
    s->width = RES_PRESETS[index].w;
    s->height = RES_PRESETS[index].h;
    recompute_auto_bitrate(ui);
  } else if (strcmp(which, "fps") == 0) {
    int index = -1;
    for (int i = 0; i < FPS_PRESET_COUNT; i++)
      if (FPS_PRESETS[i] == s->fps) {
        index = i;
        break;
      }
    index = (index < 0) ? (dir > 0 ? 0 : FPS_PRESET_COUNT - 1) : index + dir;
    if (index < 0)
      index = FPS_PRESET_COUNT - 1;
    if (index >= FPS_PRESET_COUNT)
      index = 0;
    s->fps = FPS_PRESETS[index];
    recompute_auto_bitrate(ui);
  } else if (strcmp(which, "bitrate") == 0) {
    int index = -1;
    for (int i = 0; i < BITRATE_PRESET_COUNT; i++)
      if (BITRATE_PRESETS[i] == s->bitrate) {
        index = i;
        break;
      }
    index = (index < 0) ? (dir > 0 ? 0 : BITRATE_PRESET_COUNT - 1)
                        : index + dir;
    if (index < 0)
      index = BITRATE_PRESET_COUNT - 1;
    if (index >= BITRATE_PRESET_COUNT)
      index = 0;
    s->bitrate = BITRATE_PRESETS[index];
  } else if (strcmp(which, "refresh") == 0) {
    int v = ui->config->refresh_ms + dir * 10;
    if (v < 10)
      v = 10;
    if (v > 1000)
      v = 1000;
    ui->config->refresh_ms = v;
    ui->render_mode = KINDLE_MODE_CUSTOM;
    update_mode_buttons(ui);
    update_summary(ui);
  } else if (strcmp(which, "fullrefresh") == 0) {
    int v = ui->config->full_refresh_ms + dir * 500;
    if (v < 0)
      v = 0;
    if (v > 60000)
      v = 60000;
    ui->config->full_refresh_ms = v;
    ui->render_mode = KINDLE_MODE_CUSTOM;
    update_mode_buttons(ui);
    update_summary(ui);
  } else if (strcmp(which, "port") == 0) {
    int v = (int)ui->config->port + dir;
    if (v < 1024)
      v = 1024;
    if (v > 65535)
      v = 65535;
    ui->config->port = (unsigned short)v;
  } else if (strcmp(which, "packetsize") == 0) {
    int v = s->packetSize + dir * 8;
    if (v < 1024)
      v = 1024;
    if (v > 1392)
      v = 1392;
    s->packetSize = v;
  }
  refresh_setting_labels(ui);
  save_settings(ui);
}

/* Waveform row: auto picks by damage size, the others force one waveform.
 * A manual choice switches the preset to Custom. */
static void wfm_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  static const char *values[5] = {"auto", "a2", "du", "gc16", "off"};
  int index = (int)(long)g_object_get_data(G_OBJECT(button), "wfm");
  ui->config->wfm = (char *)values[index];
  ui->render_mode = KINDLE_MODE_CUSTOM;
  update_mode_buttons(ui);
  refresh_setting_labels(ui);
  update_summary(ui);
  save_settings(ui);
}

/* Video rotation (the one that always works): -1 lets the renderer pick
 * what fits the panel, 0/90/180/270 pin it. Saved with the UI state like
 * the Rotate button in the stream options bar. */
static void vrotate_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  static const int values[5] = {-1, 0, 90, 180, 270};
  ui->stream_rotate = values[(int)(long)g_object_get_data(G_OBJECT(button),
                                                          "vrot")];
  refresh_setting_labels(ui);
  save_state(ui, NULL);
}

/* FBink region rotation between the X screen and the framebuffer; leave
 * on Auto unless partial updates land in the wrong place. */
static void fbrotate_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  static const int values[5] = {-1, 0, 90, 180, 270};
  ui->config->fbink_rotate =
      values[(int)(long)g_object_get_data(G_OBJECT(button), "frot")];
  refresh_setting_labels(ui);
  save_settings(ui);
}

/* Menu size, applied at once. The shared zoom drives every registered
 * font and widget height; ui_refit() then makes sure nothing overflows
 * and writes the effective value back. */
static void uizoom_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  static const double values[5] = {0.6, 0.8, 1.0, 1.25, 1.6};
  double zoom = values[(int)(long)g_object_get_data(G_OBJECT(button),
                                                    "zoom")];
  ui->config->uizoom = zoom;
  ink_zoom_set(zoom);
  ui_apply_scale();
  ui_refit(ui); /* may shrink to fit; writes the effective zoom back */
  refresh_setting_labels(ui);
  save_settings(ui);
}

/* Simple on/off rows. */
static void settings_bool_toggled(GtkToggleButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->updating_modes)
    return;
  const char *which = g_object_get_data(G_OBJECT(button), "which");
  bool on = gtk_toggle_button_get_active(button) ? true : false;
  if (strcmp(which, "localaudio") == 0)
    ui->config->localaudio = on;
  else if (strcmp(which, "sops") == 0)
    ui->config->sops = on;
  else if (strcmp(which, "quitappafter") == 0)
    ui->config->quitappafter = on;
  else if (strcmp(which, "viewonly") == 0)
    ui->config->viewonly = on;
  else if (strcmp(which, "debug") == 0)
    ui->config->debug_level = on ? 1 : 0;
  else if (strcmp(which, "performance") == 0)
    ui->performance = on; /* read by launch.sh */
  refresh_setting_labels(ui);
  save_settings(ui);
}

static void settings_prev_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  show_settings_group(ui, ui->settings_group - 1);
}

static void settings_next_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  show_settings_group(ui, ui->settings_group + 1);
}

/* -------------------------------------------------- settings row widgets */

static GtkWidget *settings_row(const char *name, GtkWidget *control) {
  GtkWidget *row = gtk_hbox_new(FALSE, 12);
  GtkWidget *label = make_label(name, 0, F_ROW, 0.0f);
  gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(row), control, TRUE, TRUE, 0);
  return row;
}

/* [-] value [+] with press-and-hold repeat; the value label is centered
 * between the two buttons and updated by refresh_setting_labels(). */
static GtkWidget *settings_stepper(struct kindle_ui *ui, const char *which,
                                   GtkWidget **value_label) {
  GtkWidget *box = gtk_hbox_new(FALSE, 6);
  GtkWidget *minus = make_button("-", 1, F_KEYPAD, INK_BORDER_THIN);
  ui_height(minus, H_PAGER);
  gtk_widget_set_size_request(minus, ink_px(110), -1);
  g_object_set_data(G_OBJECT(minus), "which", (gpointer)which);
  g_object_set_data(G_OBJECT(minus), "dir", (gpointer)(long)-1);
  g_signal_connect(minus, "clicked", G_CALLBACK(settings_step_clicked), ui);
  stepper_button(minus);
  gtk_box_pack_start(GTK_BOX(box), minus, FALSE, FALSE, 0);

  *value_label = make_label("", 1, F_ROW, 0.5f);
  gtk_box_pack_start(GTK_BOX(box), *value_label, TRUE, TRUE, 0);

  GtkWidget *plus = make_button("+", 1, F_KEYPAD, INK_BORDER_THIN);
  ui_height(plus, H_PAGER);
  gtk_widget_set_size_request(plus, ink_px(110), -1);
  g_object_set_data(G_OBJECT(plus), "which", (gpointer)which);
  g_object_set_data(G_OBJECT(plus), "dir", (gpointer)(long)1);
  g_signal_connect(plus, "clicked", G_CALLBACK(settings_step_clicked), ui);
  stepper_button(plus);
  gtk_box_pack_start(GTK_BOX(box), plus, FALSE, FALSE, 0);
  return box;
}

/* A row of choice buttons. `names`/`key`/`handler` follow the mode row
 * pattern; every button expands equally. */
static GtkWidget *settings_choices(GtkWidget **buttons, const char **names,
                                   int count, const char *data_key,
                                   GCallback handler, struct kindle_ui *ui) {
  GtkWidget *box = gtk_hbox_new(FALSE, 6);
  for (int i = 0; i < count; i++) {
    buttons[i] = make_toggle(names[i], 0, F_ROW);
    g_object_set_data(G_OBJECT(buttons[i]), (gchar *)data_key,
                      (gpointer)(long)i);
    ui_height(buttons[i], H_MODE);
    gtk_box_pack_start(GTK_BOX(box), buttons[i], TRUE, TRUE, 0);
    g_signal_connect(buttons[i], "toggled", handler, ui);
  }
  return box;
}

/* One On/Off toggle on the right of the row. */
static GtkWidget *settings_bool_row(struct kindle_ui *ui, const char *name,
                                    const char *which, GtkWidget **toggle) {
  GtkWidget *button = make_toggle("On", 0, F_ROW);
  ui_height(button, H_MODE);
  gtk_widget_set_size_request(button, ink_px(200), -1);
  g_object_set_data(G_OBJECT(button), "which", (gpointer)which);
  g_signal_connect(button, "toggled", G_CALLBACK(settings_bool_toggled), ui);
  *toggle = button;
  return settings_row(name, button);
}

static GtkWidget *settings_caption(const char *text) {
  GtkWidget *label = make_label(text, 0, F_CAPTION, 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  return label;
}

/* Build the whole settings page. */
static GtkWidget *build_settings_page(struct kindle_ui *ui) {
  GtkWidget *page = gtk_vbox_new(FALSE, 8);

  /* Group pager: [<] title [>]. */
  GtkWidget *pager = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(page), pager, FALSE, FALSE, 0);
  ui->settings_prev = make_button("<", 1, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui->settings_prev, H_PAGER);
  gtk_widget_set_size_request(ui->settings_prev, ink_px(120), -1);
  gtk_box_pack_start(GTK_BOX(pager), ui->settings_prev, FALSE, FALSE, 0);
  g_signal_connect(ui->settings_prev, "clicked",
                   G_CALLBACK(settings_prev_clicked), ui);
  ui->settings_title = make_label("", 1, F_STEP, 0.5f);
  gtk_box_pack_start(GTK_BOX(pager), ui->settings_title, TRUE, TRUE, 0);
  ui->settings_next = make_button(">", 1, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui->settings_next, H_PAGER);
  gtk_widget_set_size_request(ui->settings_next, ink_px(120), -1);
  gtk_box_pack_start(GTK_BOX(pager), ui->settings_next, FALSE, FALSE, 0);
  g_signal_connect(ui->settings_next, "clicked",
                   G_CALLBACK(settings_next_clicked), ui);

  /* ---- 1 Video ---- */
  GtkWidget *video = gtk_vbox_new(FALSE, 8);
  gtk_widget_set_no_show_all(video, TRUE);
  gtk_box_pack_start(GTK_BOX(page), video, FALSE, FALSE, 0);
  ui->settings_pages[SG_VIDEO] = video;
  gtk_box_pack_start(
      GTK_BOX(video),
      settings_caption("What the PC is asked to send. Smaller pictures "
                       "decode faster on the e-ink panel."),
      FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(video),
                     settings_row("Resolution",
                                  settings_stepper(ui, "res",
                                                   &ui->value_resolution)),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(video),
                     settings_row("Frame rate",
                                  settings_stepper(ui, "fps",
                                                   &ui->value_fps)),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(video),
                     settings_row("Bitrate",
                                  settings_stepper(ui, "bitrate",
                                                   &ui->value_bitrate)),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(video),
                     settings_caption("Auto bitrate follows the resolution "
                                      "and frame rate. Changes are used by "
                                      "the next stream."),
                     FALSE, FALSE, 0);

  /* ---- 2 E-ink panel ---- */
  GtkWidget *panel = gtk_vbox_new(FALSE, 8);
  gtk_widget_set_no_show_all(panel, TRUE);
  gtk_box_pack_start(GTK_BOX(page), panel, FALSE, FALSE, 0);
  ui->settings_pages[SG_PANEL] = panel;
  gtk_box_pack_start(GTK_BOX(panel),
                     settings_caption("How the picture reaches the panel. "
                                      "The same choices sit on the Stream "
                                      "page."),
                     FALSE, FALSE, 0);
  static const char *mode_names[KINDLE_MODE_COUNT] = {"Fast", "Balanced",
                                                      "Quality"};
  gtk_box_pack_start(
      GTK_BOX(panel),
      settings_row("Rendering mode",
                   settings_choices(ui->mode_buttons[1], mode_names,
                                    KINDLE_MODE_COUNT, "mode",
                                    G_CALLBACK(mode_toggled), ui)),
      FALSE, FALSE, 0);
#ifdef HAVE_FBINK
  static const char *backend_names[KINDLE_BACKEND_COUNT] = {"X11", "FBink"};
  gtk_box_pack_start(
      GTK_BOX(panel),
      settings_row("Render via",
                   settings_choices(ui->backend_buttons[1], backend_names,
                                    KINDLE_BACKEND_COUNT, "backend",
                                    G_CALLBACK(backend_toggled), ui)),
      FALSE, FALSE, 0);
#else
  ui->backend_buttons[1][0] = NULL;
  ui->backend_buttons[1][1] = NULL;
#endif
  static const char *rot_names[5] = {"Auto", "0", "90", "180", "270"};
  gtk_box_pack_start(
      GTK_BOX(panel),
      settings_row("Video rotation",
                   settings_choices(ui->vrotate_buttons, rot_names, 5,
                                    "vrot", G_CALLBACK(vrotate_toggled),
                                    ui)),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(panel),
      settings_caption("Auto rotation picks what fills the panel best "
                       "(a landscape desktop starts turned 90). Hold the "
                       "Kindle sideways when 90 is picked."),
      FALSE, FALSE, 0);

  /* ---- 3 Panel timing ---- */
  GtkWidget *timing = gtk_vbox_new(FALSE, 8);
  gtk_widget_set_no_show_all(timing, TRUE);
  gtk_box_pack_start(GTK_BOX(page), timing, FALSE, FALSE, 0);
  ui->settings_pages[SG_TIMING] = timing;
  gtk_box_pack_start(GTK_BOX(timing),
                     settings_caption("Speed versus quality of every panel "
                                      "update. Editing one of these "
                                      "switches the preset to Custom."),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(timing),
                     settings_row("Panel update every",
                                  settings_stepper(ui, "refresh",
                                                   &ui->value_refresh)),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(timing),
                     settings_row("Ghost-clear flash every",
                                  settings_stepper(ui, "fullrefresh",
                                                   &ui->value_fullrefresh)),
                     FALSE, FALSE, 0);
  static const char *wfm_names[5] = {"Auto", "A2", "DU", "GC16", "Off"};
  gtk_box_pack_start(
      GTK_BOX(timing),
      settings_row("Waveform",
                   settings_choices(ui->wfm_buttons, wfm_names, 5, "wfm",
                                    G_CALLBACK(wfm_toggled), ui)),
      FALSE, FALSE, 0);
  static const char *frot_names[5] = {"Auto", "0", "90", "180", "270"};
  gtk_box_pack_start(
      GTK_BOX(timing),
      settings_row("FBink rotation",
                   settings_choices(ui->fbrotate_buttons, frot_names, 5,
                                    "frot", G_CALLBACK(fbrotate_toggled),
                                    ui)),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(timing),
      settings_caption("Waveform Auto uses A2 for a cursor, DU for lines "
                       "and GC16 for large changes. FBink rotation only "
                       "matters if updates land in the wrong place."),
      FALSE, FALSE, 0);

  /* ---- 4 Interface ---- */
  GtkWidget *iface = gtk_vbox_new(FALSE, 8);
  gtk_widget_set_no_show_all(iface, TRUE);
  gtk_box_pack_start(GTK_BOX(page), iface, FALSE, FALSE, 0);
  ui->settings_pages[SG_INTERFACE] = iface;
  gtk_box_pack_start(GTK_BOX(iface),
                     settings_caption("How this menu looks and runs."),
                     FALSE, FALSE, 0);
  static const char *zoom_names[5] = {"Tiny", "Small", "Normal", "Large",
                                      "Huge"};
  gtk_box_pack_start(
      GTK_BOX(iface),
      settings_row("Menu size",
                   settings_choices(ui->uizoom_buttons, zoom_names, 5,
                                    "zoom", G_CALLBACK(uizoom_toggled), ui)),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(iface),
      settings_bool_row(ui, "Full CPU speed while streaming", "performance",
                        &ui->toggle_performance),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(iface),
      settings_bool_row(ui, "Verbose log (moonlight.log)", "debug",
                        &ui->toggle_debug),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(iface),
      settings_caption("Full CPU speed switches the cores to the "
                       "performance governor while the client runs and "
                       "restores them on exit."),
      FALSE, FALSE, 0);

  /* ---- 5 Host and stream ---- */
  GtkWidget *host = gtk_vbox_new(FALSE, 8);
  gtk_widget_set_no_show_all(host, TRUE);
  gtk_box_pack_start(GTK_BOX(page), host, FALSE, FALSE, 0);
  ui->settings_pages[SG_HOST] = host;
  gtk_box_pack_start(GTK_BOX(host),
                     settings_caption("How to reach the PC and what to ask "
                                      "it for. The defaults are right for "
                                      "Sunshine."),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(host),
                     settings_row("Port",
                                  settings_stepper(ui, "port",
                                                   &ui->value_port)),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(host),
                     settings_row("Packet size",
                                  settings_stepper(ui, "packetsize",
                                                   &ui->value_packetsize)),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(host),
      settings_bool_row(ui, "Keep sound on the PC", "localaudio",
                        &ui->toggle_localaudio),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(host),
      settings_bool_row(ui, "Let the PC pick optimal settings (sops)",
                        "sops", &ui->toggle_sops),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(host),
      settings_bool_row(ui, "End the app on the PC when the stream ends",
                        "quitappafter", &ui->toggle_quitappafter),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(host),
      settings_bool_row(ui, "View-only (no touches sent)", "viewonly",
                        &ui->toggle_viewonly),
      FALSE, FALSE, 0);

  /* ---- 6 PC help ---- */
  GtkWidget *pchelp = gtk_vbox_new(FALSE, 8);
  gtk_widget_set_no_show_all(pchelp, TRUE);
  gtk_box_pack_start(GTK_BOX(page), pchelp, FALSE, FALSE, 0);
  ui->settings_pages[SG_PCHELP] = pchelp;
  gtk_box_pack_start(
      GTK_BOX(pchelp),
      settings_caption("If the picture stays black, flickers, or windows "
                       "vanish: that is the capture on the PC, not this "
                       "device."),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(pchelp),
      settings_caption("1. On the PC, open Sunshine's web page "
                       "(https://PC-IP:47990) and sign in."),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(pchelp),
      settings_caption("2. In Configuration, find Capture and set it: "
                       "Portal on GNOME/KDE Wayland, KMS otherwise. Save "
                       "and restart Sunshine."),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(pchelp),
      settings_caption("3. NVIDIA with KMS showing black: add "
                       "nvidia-drm.modeset=1 to the kernel command line."),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(pchelp),
      settings_caption("4. Stream the whole Desktop app, not a single "
                       "window; per-window capture does not work on "
                       "Wayland."),
      FALSE, FALSE, 0);
  gtk_box_pack_start(
      GTK_BOX(pchelp),
      settings_caption("5. Still stuck: try Render X11/FBink and the Mode "
                       "presets in the stream bar, and read HOST-SETUP.md "
                       "in the package folder."),
      FALSE, FALSE, 0);

  return page;
}

/* Push every current value into the settings row labels and toggle
 * states. Safe to call often: identical label text does not repaint, and
 * the toggled handlers are suppressed while the states are applied. */
static void refresh_setting_labels(struct kindle_ui *ui) {
  STREAM_CONFIGURATION *s = &ui->config->stream;
  char text[96];

  if (ui->value_resolution != NULL) {
    bool known = false;
    for (int i = 0; i < RES_PRESET_COUNT && !known; i++)
      known = RES_PRESETS[i].w == s->width && RES_PRESETS[i].h == s->height;
    snprintf(text, sizeof(text), "%d x %d%s", s->width, s->height,
             known ? "" : " (custom)");
    label_set(ui->value_resolution, text);
  }
  if (ui->value_fps != NULL) {
    snprintf(text, sizeof(text), "%d fps", s->fps);
    label_set(ui->value_fps, text);
  }
  if (ui->value_bitrate != NULL) {
    if (s->bitrate == -1)
      label_set(ui->value_bitrate, "Auto");
    else {
      snprintf(text, sizeof(text), "%d kbps", s->bitrate);
      label_set(ui->value_bitrate, text);
    }
  }
  if (ui->value_refresh != NULL) {
    snprintf(text, sizeof(text), "%d ms", ui->config->refresh_ms);
    label_set(ui->value_refresh, text);
  }
  if (ui->value_fullrefresh != NULL) {
    if (ui->config->full_refresh_ms == 0)
      label_set(ui->value_fullrefresh, "Never");
    else {
      snprintf(text, sizeof(text), "%d ms", ui->config->full_refresh_ms);
      label_set(ui->value_fullrefresh, text);
    }
  }
  if (ui->value_port != NULL) {
    snprintf(text, sizeof(text), "%u", (unsigned)ui->config->port);
    label_set(ui->value_port, text);
  }
  if (ui->value_packetsize != NULL) {
    snprintf(text, sizeof(text), "%d bytes", s->packetSize);
    label_set(ui->value_packetsize, text);
  }

  /* Choice rows: apply states with the handlers suppressed. */
  ui->updating_modes = true;
  static const char *wfm_values[5] = {"auto", "a2", "du", "gc16", "off"};
  const char *wfm =
      (ui->config->wfm != NULL) ? ui->config->wfm : "auto";
  for (int i = 0; i < 5; i++)
    if (ui->wfm_buttons[i] != NULL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->wfm_buttons[i]),
                                   strcmp(wfm, wfm_values[i]) == 0);
  static const int rot_values[5] = {-1, 0, 90, 180, 270};
  for (int i = 0; i < 5; i++) {
    if (ui->vrotate_buttons[i] != NULL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->vrotate_buttons[i]),
                                   ui->stream_rotate == rot_values[i]);
    if (ui->fbrotate_buttons[i] != NULL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->fbrotate_buttons[i]),
                                   ui->config->fbink_rotate == rot_values[i]);
  }
  static const double zoom_values[5] = {0.6, 0.8, 1.0, 1.25, 1.6};
  for (int i = 0; i < 5; i++) {
    if (ui->uizoom_buttons[i] == NULL)
      continue;
    double diff = ui->config->uizoom - zoom_values[i];
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->uizoom_buttons[i]),
                                 diff < 0.015 && diff > -0.015);
  }
  struct { GtkWidget **button; bool on; const char *label; } bools[] = {
      {&ui->toggle_localaudio, ui->config->localaudio, "localaudio"},
      {&ui->toggle_sops, ui->config->sops, "sops"},
      {&ui->toggle_quitappafter, ui->config->quitappafter, "quitappafter"},
      {&ui->toggle_viewonly, ui->config->viewonly, "viewonly"},
      {&ui->toggle_debug, ui->config->debug_level > 0, "debug"},
      {&ui->toggle_performance, ui->performance, "performance"},
  };
  for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
    GtkWidget *button = *bools[i].button;
    if (button == NULL)
      continue;
    gtk_button_set_label(GTK_BUTTON(button), bools[i].on ? "On" : "Off");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), bools[i].on);
    (void)bools[i].label;
  }
  ui->updating_modes = false;

  update_mode_buttons(ui);
  update_backend_buttons(ui);
  /* The Stream page summary shows resolution/fps/mode; keep it in sync
   * with whatever the Settings page just changed. */
  update_summary(ui);
}

/* ------------------------------------------------------ worker requests */

static gboolean finish_operation(gpointer data);
static void start_pairing_flow(struct kindle_ui *ui);

static void *run_operation(void *data) {
  struct kindle_ui *ui = data;
  PAPP_LIST apps = NULL;
  int result;
  switch (ui->operation) {
    case UI_CONNECT:
      result = gs_init(&ui->server, ui->address, ui->config->port,
                       ui->config->key_dir, ui->config->debug_level,
                       ui->config->unsupported);
      break;
    case UI_PAIR:
      result = gs_pair(&ui->server, ui->pin);
      break;
    case UI_APPS:
      result = gs_applist(&ui->server, &apps);
      break;
    case UI_QUIT:
      /* Ends the session on the PC. Only dropping the connection (what
       * LiStopConnection does) leaves it running and the PC shows the
       * stream as paused. */
      result = gs_quit_app(&ui->server);
      break;
    default:
      result = GS_FAILED;
      break;
  }
  pthread_mutex_lock(&ui->worker_mutex);
  ui->operation_result = result;
  snprintf(ui->operation_error, sizeof(ui->operation_error), "%s",
           gs_error ? gs_error : "");
  ui->pending_apps = apps;
  ui->operation_done = true;
  pthread_mutex_unlock(&ui->worker_mutex);
  return NULL;
}

static void start_operation(struct kindle_ui *ui, enum ui_operation operation,
                            const char *status) {
  ui->operation = operation;
  ui->operation_done = false;
  ui->operation_error[0] = 0;
  ui->pending_apps = NULL;
  set_busy_state(ui, true);
  set_status(ui, status);

  int ret = pthread_create(&ui->worker, NULL, run_operation, ui);
  if (ret != 0) {
    char message[96];
    snprintf(message, sizeof(message), "Could not start the request (error %d)",
             ret);
    set_status(ui, message);
    set_busy_state(ui, false);
    return;
  }
  g_timeout_add(100, finish_operation, ui);
}

static gboolean finish_operation(gpointer data) {
  struct kindle_ui *ui = data;
  pthread_mutex_lock(&ui->worker_mutex);
  if (!ui->operation_done) {
    pthread_mutex_unlock(&ui->worker_mutex);
    return TRUE;
  }
  int result = ui->operation_result;
  enum ui_operation operation = ui->operation;
  PAPP_LIST apps = ui->pending_apps;
  ui->pending_apps = NULL;
  char error[sizeof(ui->operation_error)];
  snprintf(error, sizeof(error), "%s", ui->operation_error);
  pthread_mutex_unlock(&ui->worker_mutex);
  pthread_join(ui->worker, NULL);
  ui->operation = UI_IDLE;

  if (operation == UI_CONNECT) {
    if (result != GS_OK) {
      char message[320];
      snprintf(message, sizeof(message),
               "Connection failed: %s Check the address, Wi-Fi, and that "
               "GameStream or Sunshine is running.",
               error[0] ? error : "");
      set_status(ui, message);
      set_busy_state(ui, false);
      fprintf(stderr, "Kindle connect to %s:%u failed (%d): %s\n", ui->address,
              ui->config->port, result, error);
      return FALSE;
    }

    add_recent(ui, ui->address);
    save_state(ui, ui->selected_app[0] ? ui->selected_app : NULL);
    if (ui->server.paired) {
      show_page(ui, PAGE_APPS);
      start_operation(ui, UI_APPS, "Connected! Loading your apps ...");
    } else {
      /* No pairing yet: jump to the PIN page and start waiting. */
      start_pairing_flow(ui);
    }
  } else if (operation == UI_PAIR) {
    if (result != GS_OK) {
      char message[288];
      snprintf(message, sizeof(message),
               "Pairing failed: %s Tap Pair again to retry.",
               error[0] ? error : "check the PC and try again");
      set_status(ui, message);
      set_busy_state(ui, false);
    } else {
      show_page(ui, PAGE_APPS);
      start_operation(ui, UI_APPS, "Paired! Loading your apps ...");
    }
  } else if (operation == UI_QUIT) {
    set_busy_state(ui, false);
    char message[320];
    if (result == GS_OK)
      snprintf(message, sizeof(message),
               "%sStream stopped and the session on your PC was ended. Tap "
               "Stream to start again.",
               ui->end_reason[0] ? ui->end_reason : "");
    else
      snprintf(message, sizeof(message),
               "%sStream stopped, but the PC did not confirm ending the "
               "session (%s). It may still show as paused.",
               ui->end_reason[0] ? ui->end_reason : "",
               error[0] ? error : "no answer");
    set_status(ui, message);
    ui->end_reason[0] = 0;
  } else if (operation == UI_APPS) {
    set_busy_state(ui, false);
    if (result != GS_OK) {
      store_apps(ui, NULL);
      update_app_rows(ui);
      set_status(ui, error[0] ? error : "Could not load the app list. "
                                        "Tap Refresh to try again.");
    } else {
      store_apps(ui, apps);
      select_default_app(ui);
      update_summary(ui);
      if (ui->app_count > 0)
        set_status(ui, "Tap an app, then Stream.");
      else
        set_status(ui, "The PC has no apps to stream. Add Desktop to "
                       "GeForce Experience or Sunshine, then tap Refresh.");
    }
  }
  return FALSE;
}

/* -------------------------------------------------------------- actions */

static void connect_host(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy)
    return;
  const char *address = gtk_entry_get_text(GTK_ENTRY(ui->host));
  if (strlen(address) == 0) {
    set_status(ui, "Type your PC's address first, then tap Connect.");
    return;
  }
  snprintf(ui->address, sizeof(ui->address), "%s", address);
  memset(&ui->server, 0, sizeof(ui->server));
  char status[128];
  snprintf(status, sizeof(status), "Connecting to %s ... (can take a moment)",
           ui->address);
  start_operation(ui, UI_CONNECT, status);
}

static void recent_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->busy || ui->page != PAGE_CONNECT)
    return;
  int index = (int)(long)g_object_get_data(G_OBJECT(button), "recent");
  if (index < 0 || index >= ui->host_count)
    return;
  gtk_entry_set_text(GTK_ENTRY(ui->host), ui->hosts[index]);
  char status[128];
  snprintf(status, sizeof(status), "Address set to %s. Tap Connect.",
           ui->hosts[index]);
  set_status(ui, status);
}

static gboolean begin_pairing(gpointer data) {
  start_operation(data, UI_PAIR,
                  "Waiting for the PC ... enter the PIN there to approve.");
  return FALSE;
}

/* Shown when the PC is connected but not yet paired. The PIN is drawn as
 * spaced digits so it is easy to read and type on the PC. */
static void start_pairing_flow(struct kindle_ui *ui) {
  if (ui->config->pin > 0 && ui->config->pin <= 9999)
    snprintf(ui->pin, sizeof(ui->pin), "%04d", ui->config->pin);
  else
    snprintf(ui->pin, sizeof(ui->pin), "%u%u%u%u", (unsigned)random() % 10,
             (unsigned)random() % 10, (unsigned)random() % 10,
             (unsigned)random() % 10);

  char spaced[16];
  snprintf(spaced, sizeof(spaced), "%c %c %c %c", ui->pin[0], ui->pin[1],
           ui->pin[2], ui->pin[3]);
  gtk_label_set_text(GTK_LABEL(ui->pin_display), spaced);

  show_page(ui, PAGE_PAIR);
  set_status(ui, "Connected. Now type this PIN on your PC to pair.");
  set_busy_state(ui, true);
  g_timeout_add(1000, begin_pairing, ui);
}

static void pair_again_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (!ui->busy)
    start_pairing_flow(ui);
}

static void pair_back_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy)
    return;
  show_page(ui, PAGE_CONNECT);
  set_status(ui, "Pairing cancelled. Tap Connect to try again.");
}

static void refresh_apps(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy)
    return;
  start_operation(ui, UI_APPS, "Refreshing the app list ...");
}

static void disconnect_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy)
    return;
  show_page(ui, PAGE_CONNECT);
  set_status(ui, "Disconnected. Tap Connect to stream again, or edit the "
                 "address.");
}

static void settings_open_clicked(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->page == PAGE_SETTINGS) {
    show_page(ui, ui->settings_return_page);
    set_status(ui, "Back to the connection menu.");
    return;
  }
  ui->settings_return_page = ui->page;
  show_page(ui, PAGE_SETTINGS);
  set_status(ui, "Changes are saved at once and used by the next stream.");
}

static void launch_app(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  if (ui->busy)
    return;
  if (ui->selected_app[0] == 0 || ui->selected_index < 0 ||
      ui->selected_index >= ui->app_count) {
    set_status(ui, "Tap an app to select it, then tap Stream.");
    return;
  }

  /* Stream parameters come from moonlight.conf (fps, bitrate, width,
   * height), so the stream can be tuned without rebuilding. The rendering
   * mode picked on this page presets refresh, fullrefresh and waveform,
   * and the backend row chooses the pixel path. */
  if (ui->render_mode >= 0 && ui->render_mode < KINDLE_MODE_COUNT) {
    ui->config->render_mode = ui->render_mode;
    ui->config->refresh_ms = kindle_mode_refresh_ms(ui->render_mode);
    ui->config->full_refresh_ms = kindle_mode_full_refresh_ms(ui->render_mode);
    ui->config->wfm = (char *)kindle_mode_wfm(ui->render_mode);
  }
  ui->config->render_backend = ui->render_backend;
  ui->config->app = ui->apps[ui->selected_index];
  ui->config->address = (char *)gtk_entry_get_text(GTK_ENTRY(ui->host));
  ui->config->platform = "kindle";
  ui->config->codec = CODEC_H264;
  /* Video rotation: reuse what the last stream settled on, unless the
   * config pinned one explicitly (rotate = 0/90/180/270 or -rotate). A
   * stored -1 means the renderer picks the orientation that uses the
   * panel best and the choice is saved after the stream. */
  if (ui->config->rotate < 0 && ui->stream_rotate >= 0)
    ui->config->rotate = ui->stream_rotate;
  snprintf(ui->address, sizeof(ui->address), "%s", ui->config->address);
  add_recent(ui, ui->address);
  save_state(ui, ui->selected_app);
  gtk_widget_hide(ui->window);
  int result = stream(&ui->server, ui->config, KINDLE);
  ui->config->app = NULL;
  gtk_widget_show(ui->window);

  /* Remember the rotation the stream actually used, including when the
   * renderer picked it automatically. */
  ui->stream_rotate = kindle_video_get_rotate();
  save_state(ui, ui->selected_app);

  /* Why it ended or failed, from the connection callbacks / stream(). */
  ui->end_reason[0] = 0;
  if (connection_message[0] != 0)
    snprintf(ui->end_reason, sizeof(ui->end_reason), "%s ", connection_message);

  if (result != 0) {
    char message[320];
    snprintf(message, sizeof(message), "%s%s", ui->end_reason,
             ui->end_reason[0] ? "Tap Stream to try again."
                               : "Could not start the stream. Check the PC, "
                                 "then tap Refresh.");
    set_status(ui, message);
  }

  /* End the host session so the PC does not keep it as paused. This also
   * covers a start that failed after the PC had already launched the app.
   * Skipped when moonlight.conf's quitappafter already did it inside
   * stream(). Runs on the worker thread: the PC may be slow or gone. */
  if (stream_session_started && !ui->config->quitappafter) {
    start_operation(ui, UI_QUIT, "Stopping the session on your PC ...");
  } else if (result == 0) {
    char message[320];
    snprintf(message, sizeof(message), "%sTap Stream to restart, or Disconnect.",
             ui->end_reason);
    set_status(ui, message);
    ui->end_reason[0] = 0;
  }
}

/* ------------------------------------------------- orientation and keys */

/* Rotation on a Kindle is a minefield: `lipc-set-prop
 * com.lab126.winmgr orientationLock` is what the stock reader uses, but
 * stock firmware applies it to the booklet reader only and ignores every
 * other app - the property reads back as set while nothing moves. So the
 * request below is sent, but the result is *watched*: the X screen size is
 * polled for a few seconds and only a real size change counts. When the
 * firmware ignored the request the status line says so, and the honest
 * path to a landscape picture stays the Rotate button in the stream
 * options bar (software rotation of the video itself, which works on
 * every firmware). The old hide/unfullscreen dance is gone: it flashed
 * the whole panel and left a blank window behind whenever the rotation
 * never came. */
static int orient_start_w, orient_start_h;

static gboolean orientation_settled(gpointer data) {
  struct kindle_ui *ui = data;
  int w, h;
  ink_screen_size(&w, &h);
  gtk_window_fullscreen(GTK_WINDOW(ui->window));
  gtk_widget_queue_resize(ui->window);
  gtk_widget_queue_draw(ui->window);
  if (w != orient_start_w || h != orient_start_h) {
    ui_refit(ui); /* refit the layout for the new screen shape */
    set_status(ui, "Screen rotated and the layout was adjusted.");
  } else {
    set_status(ui, "This firmware ignores the rotation request (it only "
                   "affects the book reader). For landscape video use the "
                   "Rotate button in the stream options bar.");
  }
  return FALSE;
}

static void set_orientation(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  if (ui->busy)
    return;
  const char *orientation = g_object_get_data(G_OBJECT(button), "orientation");
  ink_orient_send(orientation);
  ink_screen_size(&orient_start_w, &orient_start_h);
  set_status(ui, "Changing orientation ...");
  ink_orient_watch(16, orientation_settled, ui); /* 4 s */
}

static GtkWidget *orientation_button(struct kindle_ui *ui, GtkWidget *bar,
                                     const char *label,
                                     const char *orientation) {
  GtkWidget *button = make_button(label, 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(button, H_ORIENT);
  g_object_set_data(G_OBJECT(button), "orientation", (gpointer)orientation);
  gtk_box_pack_start(GTK_BOX(bar), button, TRUE, TRUE, 0);
  g_signal_connect(button, "clicked", G_CALLBACK(set_orientation), ui);
  return button;
}

/* Touch-only devices get a full keypad; a USB/Bluetooth keyboard can also
 * type the address, with BackSpace and Escape working as expected. */
static gboolean keyboard_event(GtkWidget *widget, GdkEventKey *event,
                               gpointer data) {
  struct kindle_ui *ui = data;
  (void)widget;
  if (event->keyval == GDK_Escape) {
    /* Escape quits even while a request runs: the process exits and the
     * worker dies with it, which is the only escape if the PC is gone. */
    gtk_main_quit();
    return TRUE;
  }
  if (ui->busy || ui->page != PAGE_CONNECT)
    return FALSE;
  if (event->keyval == GDK_BackSpace) {
    keypad_backspace(ui);
    return TRUE;
  }
  if (event->length == 1) {
    unsigned char c = (unsigned char)event->string[0];
    if (isalnum(c) || c == '.' || c == '-') {
      keypad_input(ui, (char)c);
      return TRUE;
    }
  }
  return FALSE;
}

static void quit_ui(GtkButton *button, gpointer data) {
  struct kindle_ui *ui = data;
  (void)button;
  (void)ui;
  /* Always allowed: touch users need a way out even when busy. */
  gtk_main_quit();
}

static gboolean close_ui(GtkWidget *widget, GdkEvent *event, gpointer data) {
  struct kindle_ui *ui = data;
  (void)widget;
  (void)event;
  (void)ui;
  gtk_main_quit();
  return FALSE;
}

/* ----------------------------------------------------------------- main */

void kindle_ui_run(PCONFIGURATION config) {
  struct kindle_ui ui;
  memset(&ui, 0, sizeof(ui));
  ui.config = config;
  ui.selected_index = -1;
  ui.stream_rotate = -1; /* renderer picks, then the choice is saved */
  /* Rendering mode and backend defaults come from moonlight.conf (fast
   * and, when FBink is compiled in, the direct framebuffer); the settings
   * file and the saved ui-state override both below. */
  ui.render_mode = config->render_mode;
  ui.render_backend = config->render_backend;
  /* The Settings page's saved choices are applied after moonlight.conf
   * and before any widget exists, so the menu zoom is right from the
   * first draw and the first Connect uses the saved port. */
  load_settings(&ui);
  ink_zoom_set(config->uizoom);
  pthread_mutex_init(&ui.worker_mutex, NULL);
  srandom((unsigned)(time(NULL) ^ getpid()));
  gtk_init(NULL, NULL);
  ink_style_init();

  ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(ui.window, "ink-window");
  gtk_window_set_title(GTK_WINDOW(ui.window), "L:A_N:application_ID:moonlight_PC:T");
  gtk_window_fullscreen(GTK_WINDOW(ui.window));
  g_signal_connect(ui.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(ui.window, "key-press-event", G_CALLBACK(keyboard_event), &ui);
  g_signal_connect(ui.window, "delete-event", G_CALLBACK(close_ui), &ui);

  GtkWidget *layout = gtk_vbox_new(FALSE, 10);
  gtk_container_set_border_width(GTK_CONTAINER(layout), ink_px(12));
  gtk_container_add(GTK_CONTAINER(ui.window), layout);
  ui.layout = layout;

  /* Header: title, current step, and the status line everyone reads. */
  GtkWidget *title = make_label("Moonlight", 1, F_TITLE, 0.0f);
  gtk_box_pack_start(GTK_BOX(layout), title, FALSE, FALSE, 0);
  ui.step_label = make_label("Step 1 of 3 - Connect to your PC", 0, F_STEP, 0.0f);
  gtk_box_pack_start(GTK_BOX(layout), ui.step_label, FALSE, FALSE, 0);
  GtkWidget *separator = ink_rule(INK_RULE_PX);
  gtk_box_pack_start(GTK_BOX(layout), separator, FALSE, FALSE, 0);
  ui.status = make_label("Enter your PC's address, then tap Connect.",
                         0, F_STATUS, 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(ui.status), TRUE);
  ui_height(ui.status, H_STATUS);
  gtk_box_pack_start(GTK_BOX(layout), ui.status, FALSE, FALSE, 0);

  /* Step 1: connect page. */
  ui.page_connect = gtk_vbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(layout), ui.page_connect, FALSE, FALSE, 0);
  GtkWidget *caption = make_label("PC address (for example 192.168.1.20)",
                                  0, F_CAPTION, 0.0f);
  gtk_box_pack_start(GTK_BOX(ui.page_connect), caption, FALSE, FALSE, 0);
  ui.host = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(ui.host), ADDRESS_MAX);
  gtk_entry_set_editable(GTK_ENTRY(ui.host), FALSE);
  ink_style_field(ui.host);
  ui_font(ui.host, 1, F_ENTRY);
  ui_height(ui.host, H_ENTRY);
  gtk_box_pack_start(GTK_BOX(ui.page_connect), ui.host, FALSE, FALSE, 0);
  GtkWidget *recents_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.page_connect), recents_bar, FALSE, FALSE, 0);
  for (int i = 0; i < MAX_RECENT; i++) {
    ui.recents[i] = gtk_button_new_with_label("");
    ink_style_button(ui.recents[i], INK_NAME_BUTTON, INK_BORDER_THIN);
    g_object_set_data(G_OBJECT(ui.recents[i]), "recent", (gpointer)(long)i);
    ui_font(gtk_bin_get_child(GTK_BIN(ui.recents[i])), 0, F_BOTTOM);
    ui_height(ui.recents[i], H_RECENT);
    gtk_box_pack_start(GTK_BOX(recents_bar), ui.recents[i], TRUE, TRUE, 0);
    g_signal_connect(ui.recents[i], "clicked", G_CALLBACK(recent_clicked), &ui);
  }

  GtkWidget *keys = gtk_table_new(4, 4, TRUE);
  gtk_table_set_row_spacings(GTK_TABLE(keys), 8);
  gtk_table_set_col_spacings(GTK_TABLE(keys), 8);
  gtk_box_pack_start(GTK_BOX(ui.page_connect), keys, FALSE, FALSE, 0);
  ui.keys_numeric = keys;
  key_button(&ui, "1", "1", keys, 0, 1, 0);
  key_button(&ui, "2", "2", keys, 1, 2, 0);
  key_button(&ui, "3", "3", keys, 2, 3, 0);
  key_button(&ui, "back", "Back", keys, 3, 4, 0);
  key_button(&ui, "4", "4", keys, 0, 1, 1);
  key_button(&ui, "5", "5", keys, 1, 2, 1);
  key_button(&ui, "6", "6", keys, 2, 3, 1);
  key_button(&ui, "clear", "Clear", keys, 3, 4, 1);
  key_button(&ui, "7", "7", keys, 0, 1, 2);
  key_button(&ui, "8", "8", keys, 1, 2, 2);
  key_button(&ui, "9", "9", keys, 2, 3, 2);
  key_button(&ui, "0", "0", keys, 3, 4, 2);
  key_button(&ui, ".", ".", keys, 0, 3, 3);
  key_button(&ui, "mode", "ABC", keys, 3, 4, 3);

  /* Letter keypad for hostnames (desktop.local), reached with ABC. Built
   * no_show_all: it stays hidden until ABC is tapped and ui_refit()
   * measures the page with the numeric table only. */
  GtkWidget *alpha = gtk_table_new(6, 5, TRUE);
  gtk_table_set_row_spacings(GTK_TABLE(alpha), 8);
  gtk_table_set_col_spacings(GTK_TABLE(alpha), 8);
  gtk_widget_set_no_show_all(alpha, TRUE);
  gtk_box_pack_start(GTK_BOX(ui.page_connect), alpha, FALSE, FALSE, 0);
  ui.keys_alpha = alpha;
  {
    const char *rows[5] = {"qwerty", "yuiop", "asdfg", "hjkl-", "zxcvb"};
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 5; c++) {
        char key[2] = {rows[r][c], 0};
        char ch = rows[r][c];
        char label[2] = {(ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch, 0};
        key_button(&ui, key, label, alpha, c, c + 1, r);
      }
    key_button(&ui, "n", "N", alpha, 0, 1, 5);
    key_button(&ui, "m", "M", alpha, 1, 2, 5);
    key_button(&ui, "back", "Back", alpha, 2, 4, 5);
    key_button(&ui, "mode", "123", alpha, 4, 5, 5);
  }

  ui.connect = make_button("Connect", 1, F_MAIN, INK_BORDER_THICK);
  ui_height(ui.connect, H_CONNECT);
  gtk_box_pack_start(GTK_BOX(ui.page_connect), ui.connect, FALSE, FALSE, 0);
  g_signal_connect(ui.connect, "clicked", G_CALLBACK(connect_host), &ui);

  /* Step 2: pair page. */
  ui.page_pair = gtk_vbox_new(FALSE, 14);
  gtk_box_pack_start(GTK_BOX(layout), ui.page_pair, FALSE, FALSE, 0);
  GtkWidget *pin_caption =
      make_label("Enter this PIN on your PC:", 0, F_CAPTION, 0.0f);
  gtk_box_pack_start(GTK_BOX(ui.page_pair), pin_caption, FALSE, FALSE, 0);
  ui.pin_display = make_label("", 1, F_PIN, 0.5f);
  ui_height(ui.pin_display, H_PIN);
  gtk_box_pack_start(GTK_BOX(ui.page_pair), ui.pin_display, FALSE, FALSE, 0);
  GtkWidget *pin_hint = make_label(
      "Open GeForce Experience or Sunshine on your PC, choose Pair, and type "
      "these four digits. Pairing then finishes by itself.", 0, F_STATUS, 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(pin_hint), TRUE);
  gtk_box_pack_start(GTK_BOX(ui.page_pair), pin_hint, FALSE, FALSE, 0);
  GtkWidget *pair_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.page_pair), pair_bar, FALSE, FALSE, 0);
  ui.pair_again = make_button("Pair again", 1, F_MAIN, INK_BORDER_THICK);
  ui_height(ui.pair_again, H_PAIRBTN);
  gtk_box_pack_start(GTK_BOX(pair_bar), ui.pair_again, TRUE, TRUE, 0);
  g_signal_connect(ui.pair_again, "clicked", G_CALLBACK(pair_again_clicked), &ui);
  ui.pair_back = make_button("Back to Connect", 1, F_MAIN, INK_BORDER_THICK);
  ui_height(ui.pair_back, H_PAIRBTN);
  gtk_box_pack_start(GTK_BOX(pair_bar), ui.pair_back, TRUE, TRUE, 0);
  g_signal_connect(ui.pair_back, "clicked", G_CALLBACK(pair_back_clicked), &ui);

  /* Step 3: apps page. */
  ui.page_apps = gtk_vbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(layout), ui.page_apps, FALSE, FALSE, 0);
  for (int i = 0; i < APP_ROWS; i++) {
    ui.app_rows[i] = make_toggle("", 0, F_ROW);
    g_object_set_data(G_OBJECT(ui.app_rows[i]), "row", (gpointer)(long)i);
    ui_height(ui.app_rows[i], H_ROW);
    gtk_box_pack_start(GTK_BOX(ui.page_apps), ui.app_rows[i], FALSE, FALSE, 0);
    g_signal_connect(ui.app_rows[i], "toggled", G_CALLBACK(app_row_toggled), &ui);
  }
  GtkWidget *pager_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.page_apps), pager_bar, FALSE, FALSE, 0);
  ui.app_prev = make_button("Previous", 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui.app_prev, H_PAGER);
  gtk_box_pack_start(GTK_BOX(pager_bar), ui.app_prev, FALSE, FALSE, 0);
  g_signal_connect(ui.app_prev, "clicked", G_CALLBACK(app_prev_clicked), &ui);
  ui.app_pager = make_label("", 0, F_BOTTOM, 0.5f);
  gtk_box_pack_start(GTK_BOX(pager_bar), ui.app_pager, TRUE, TRUE, 0);
  ui.app_next = make_button("Next", 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui.app_next, H_PAGER);
  gtk_box_pack_start(GTK_BOX(pager_bar), ui.app_next, FALSE, FALSE, 0);
  g_signal_connect(ui.app_next, "clicked", G_CALLBACK(app_next_clicked), &ui);

  /* Rendering mode row: the active preset is highlighted and echoed in the
   * summary line below. Same choices exist on the Settings page (set 1). */
  GtkWidget *mode_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.page_apps), mode_bar, FALSE, FALSE, 0);
  for (int i = 0; i < KINDLE_MODE_COUNT; i++) {
    ui.mode_buttons[0][i] = make_toggle(kindle_mode_name(i), 0, F_ROW);
    g_object_set_data(G_OBJECT(ui.mode_buttons[0][i]), "mode",
                      (gpointer)(long)i);
    ui_height(ui.mode_buttons[0][i], H_MODE);
    gtk_box_pack_start(GTK_BOX(mode_bar), ui.mode_buttons[0][i], TRUE, TRUE, 0);
    g_signal_connect(ui.mode_buttons[0][i], "toggled",
                     G_CALLBACK(mode_toggled), &ui);
  }

  /* Rendering backend row: X11 or FBink pixel path. Only built when the
   * binary was compiled with FBink support. */
#ifdef HAVE_FBINK
  GtkWidget *backend_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.page_apps), backend_bar, FALSE, FALSE, 0);
  for (int i = 0; i < KINDLE_BACKEND_COUNT; i++) {
    ui.backend_buttons[0][i] = make_toggle(kindle_backend_name(i), 0, F_ROW);
    g_object_set_data(G_OBJECT(ui.backend_buttons[0][i]), "backend",
                      (gpointer)(long)i);
    ui_height(ui.backend_buttons[0][i], H_MODE);
    gtk_box_pack_start(GTK_BOX(backend_bar), ui.backend_buttons[0][i], TRUE,
                       TRUE, 0);
    g_signal_connect(ui.backend_buttons[0][i], "toggled",
                     G_CALLBACK(backend_toggled), &ui);
  }
#else
  ui.backend_buttons[0][0] = NULL;
  ui.backend_buttons[0][1] = NULL;
#endif

  ui.summary = make_label("", 0, F_SMALL, 0.0f);
  gtk_box_pack_start(GTK_BOX(ui.page_apps), ui.summary, FALSE, FALSE, 0);

  ui.stream_btn = make_button("Stream", 1, F_MAIN, INK_BORDER_THICK);
  ui_height(ui.stream_btn, H_STREAM);
  gtk_box_pack_start(GTK_BOX(ui.page_apps), ui.stream_btn, FALSE, FALSE, 0);
  g_signal_connect(ui.stream_btn, "clicked", G_CALLBACK(launch_app), &ui);
  GtkWidget *apps_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.page_apps), apps_bar, FALSE, FALSE, 0);
  ui.refresh = make_button("Refresh apps", 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui.refresh, H_APPSBAR);
  gtk_box_pack_start(GTK_BOX(apps_bar), ui.refresh, TRUE, TRUE, 0);
  g_signal_connect(ui.refresh, "clicked", G_CALLBACK(refresh_apps), &ui);
  ui.disconnect = make_button("Disconnect", 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui.disconnect, H_APPSBAR);
  gtk_box_pack_start(GTK_BOX(apps_bar), ui.disconnect, TRUE, TRUE, 0);
  g_signal_connect(ui.disconnect, "clicked", G_CALLBACK(disconnect_clicked), &ui);

  /* Bottom bar: Settings, rotation and Quit stay reachable from every
   * page. */
  ui.bottom_bar = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_end(GTK_BOX(layout), ui.bottom_bar, FALSE, FALSE, 0);
  ui.orient_row = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start(GTK_BOX(ui.bottom_bar), ui.orient_row, TRUE, TRUE, 0);
  orientation_button(&ui, ui.orient_row, "Portrait", "U");
  orientation_button(&ui, ui.orient_row, "Landscape \342\206\220", "L");
  orientation_button(&ui, ui.orient_row, "Landscape \342\206\222", "R");
  ui.settings_button = make_button("Settings", 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui.settings_button, H_QUIT);
  gtk_box_pack_start(GTK_BOX(ui.bottom_bar), ui.settings_button, FALSE, FALSE, 0);
  g_signal_connect(ui.settings_button, "clicked", G_CALLBACK(settings_open_clicked), &ui);
  ui.quit = make_button("Quit", 0, F_BOTTOM, INK_BORDER_THIN);
  ui_height(ui.quit, H_QUIT);
  gtk_box_pack_start(GTK_BOX(ui.bottom_bar), ui.quit, FALSE, FALSE, 0);
  g_signal_connect(ui.quit, "clicked", G_CALLBACK(quit_ui), &ui);

  /* Settings page: everything configurable on the device, six pager
   * groups. Built after the apps page so both share the same mode and
   * backend state. */
  ui.page_settings = build_settings_page(&ui);
  gtk_box_pack_start(GTK_BOX(layout), ui.page_settings, FALSE, FALSE, 0);

  show_page(&ui, PAGE_CONNECT);
  load_state(&ui);
  /* A saved rendering preset defines the panel timing; make the config
   * match what the stream will actually use, so the Settings page always
   * shows the truth (launch_app re-applies the same numbers). */
  if (ui.render_mode >= 0 && ui.render_mode < KINDLE_MODE_COUNT) {
    ui.config->refresh_ms = kindle_mode_refresh_ms(ui.render_mode);
    ui.config->full_refresh_ms = kindle_mode_full_refresh_ms(ui.render_mode);
    ui.config->wfm = (char *)kindle_mode_wfm(ui.render_mode);
  }
  update_recents(&ui);
  update_app_rows(&ui);
  update_mode_buttons(&ui);
  update_backend_buttons(&ui);
  update_summary(&ui);
  refresh_setting_labels(&ui);
  /* Fit the whole layout to the real screen before anything is shown:
   * pages are measured unrealized and the shared zoom is shrunk until
   * even the largest page fits, so nothing is cramped or cut off. */
  ui_refit(&ui);
  gtk_widget_show_all(ui.window);
  ui_restore_visibility(&ui); /* re-hide what show_all() revealed */
  if (gtk_entry_get_text(GTK_ENTRY(ui.host))[0] != 0)
    set_status(&ui, "Address restored. Tap Connect to stream again.");
  gtk_main();

  if (ui.apps != NULL) {
    for (int i = 0; i < ui.app_count; i++)
      free(ui.apps[i]);
    free(ui.apps);
  }
  pthread_mutex_destroy(&ui.worker_mutex);
}
