/*
 * Flat, high-contrast e-ink look shared by the touch menu (ui-kindle.c) and
 * the stream options bar (video/kindle.cpp).
 *
 * Why this exists: the stock GTK2 theme draws bevels, gradients, hover
 * highlights and focus rings. On an e-ink panel every one of those is a
 * dithered gray smear, and every state change (hover, focus, "insensitive")
 * costs a visible panel refresh. This header replaces that with the look of
 * a printed page:
 *
 *   - pure white paper, pure black ink, one light gray for "selected";
 *   - hard 2-3 px black outlines instead of bevels;
 *   - a pressed button inverts (black with white text), a selected toggle
 *     fills light gray - so state is readable without color or shading;
 *   - no hover/prelight, no focus ring, no blinking cursor, no animation.
 *
 * The look is built from three layers so it survives whatever theme the
 * Kindle ships:
 *
 *   1. GtkSettings (touchscreen mode, no cursor blink, no animations).
 *   2. A GTK RC style keyed on widget *names* (ink-button, ink-toggle,
 *      ink-marked, ink-field). Names, not classes, so only widgets we
 *      deliberately styled change, the same approach gambatte-k2 uses.
 *   3. An expose-after handler that overdraws the outline with plain GDK
 *      calls, so the border is identical whichever GTK theme engine is
 *      active and never depends on how that engine draws bevels.
 *
 * Everything here is header-only (static inline) so both the C and the C++
 * translation units can share one copy of the numbers.
 *
 * Tuning without rebuilding: if a file called ink.rc exists in the working
 * directory (launch.sh cd's to the install folder) it is parsed after the
 * built-in style. Use the ":highest" priority in it so it wins, e.g.
 *
 *   style "mine" { bg[ACTIVE] = "#c8c8c8" }
 *   widget "*ink-toggle*" style:highest "mine"
 */
#ifndef KINDLE_STYLE_H
#define KINDLE_STYLE_H

#include <gtk/gtk.h>
#include <unistd.h>

/* ---- UI scale and zoom ----------------------------------------------
 *
 * Kindle panels differ (PW4 1072x1448, PW5 1236x1648, plus whatever the
 * window manager does on rotation), so hard-coded pixel sizes cannot fit
 * every screen. Every UI size is a base value for a ~1000 px wide screen
 * multiplied by one factor:
 *
 *     scale = clamp(smaller screen side / 1000, 0.55 .. 2.2) * uizoom
 *
 * The uizoom factor comes from moonlight.conf (default 1). Values below
 * 1 zoom the UI out (smaller text, more content), values above 1 zoom in.
 * The menu additionally shrinks the whole layout automatically when a
 * page would not fit the screen (ui_refit in ui-kindle.c).
 *
 * Fonts use Pango absolute sizes, so the outcome no longer depends on
 * the DPI the Kindle X server reports.
 */
static double g_ink_zoom = 1.0;

static inline void ink_zoom_set(double zoom) {
  if (zoom >= 0.4 && zoom <= 3.0)
    g_ink_zoom = zoom;
}

static inline double ink_zoom_get(void) {
  return g_ink_zoom;
}

static inline double ink_ui_scale(void) {
  GdkScreen *screen = gdk_screen_get_default();
  int w = screen ? gdk_screen_get_width(screen) : 1024;
  int h = screen ? gdk_screen_get_height(screen) : 768;
  int smaller = w < h ? w : h;
  double base = smaller / 1000.0;
  if (base < 0.55)
    base = 0.55;
  if (base > 2.2)
    base = 2.2;
  return base * g_ink_zoom;
}

/* A base size in real pixels for the current scale. */
static inline int ink_px(double base) {
  int v = (int)(base * ink_ui_scale() + 0.5);
  return v > 0 ? v : 1;
}

/* Apply a font in absolute pixels (base size before scaling). */
static inline void ink_font_px(GtkWidget *widget, int bold, int base_px) {
  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, "Sans");
  pango_font_description_set_weight(
      font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_absolute_size(
      font, (double)ink_px(base_px) * PANGO_SCALE);
  gtk_widget_modify_font(widget, font);
  pango_font_description_free(font);
}

/* Outline weights in pixels (the panel is ~300 ppi, so 1 px is a hairline).
 * Primary actions and choices get the heavy outline, secondary controls the
 * light one; that is the whole visual hierarchy. */
#define INK_BORDER_THIN 2
#define INK_BORDER_THICK 3
#define INK_RULE_PX 2

/* One-off ink_rule heights stay as compiled; anything that must follow
 * the zoom goes through ink_px(). */

/* Widget names the RC style below is keyed on. They must not be prefixes of
 * each other: the RC patterns are substring globs. */
#define INK_NAME_BUTTON "ink-button" /* plain action: pressed = inverted */
#define INK_NAME_TOGGLE "ink-toggle" /* choice: selected = gray fill     */
#define INK_NAME_MARKED "ink-marked" /* plain action drawn gray at rest  */
#define INK_NAME_FIELD "ink-field"   /* read-only value box (entry)      */

/* The only two grays in the whole UI. Light enough that black text stays
 * crisp, dark enough to be distinct from white even on fast waveforms. */
#define INK_SELECTED_GRAY "#dcdcdc"
#define INK_DISABLED_GRAY "#8c8c8c"

/* One palette block: every state gets a flat fill and the bevel colors
 * (light/dark/mid) are set equal to that fill, so whatever bevel the theme
 * engine still draws is invisible under our own outline. */
#define INK_PALETTE(bn, ba, fa)                                              \
  "bg[NORMAL] = \"" bn "\"\n"                                                \
  "bg[PRELIGHT] = \"" bn "\"\n"                                              \
  "bg[ACTIVE] = \"" ba "\"\n"                                                \
  "bg[SELECTED] = \"" INK_SELECTED_GRAY "\"\n"                               \
  "bg[INSENSITIVE] = \"" bn "\"\n"                                           \
  "light[NORMAL] = \"" bn "\"\n"                                             \
  "light[PRELIGHT] = \"" bn "\"\n"                                           \
  "light[ACTIVE] = \"" ba "\"\n"                                             \
  "light[SELECTED] = \"" INK_SELECTED_GRAY "\"\n"                            \
  "light[INSENSITIVE] = \"" bn "\"\n"                                        \
  "dark[NORMAL] = \"" bn "\"\n"                                              \
  "dark[PRELIGHT] = \"" bn "\"\n"                                            \
  "dark[ACTIVE] = \"" ba "\"\n"                                              \
  "dark[SELECTED] = \"" INK_SELECTED_GRAY "\"\n"                             \
  "dark[INSENSITIVE] = \"" bn "\"\n"                                         \
  "mid[NORMAL] = \"" bn "\"\n"                                               \
  "mid[PRELIGHT] = \"" bn "\"\n"                                             \
  "mid[ACTIVE] = \"" ba "\"\n"                                               \
  "mid[SELECTED] = \"" INK_SELECTED_GRAY "\"\n"                              \
  "mid[INSENSITIVE] = \"" bn "\"\n"                                          \
  "fg[NORMAL] = \"#000000\"\n"                                               \
  "fg[PRELIGHT] = \"#000000\"\n"                                             \
  "fg[ACTIVE] = \"" fa "\"\n"                                                \
  "fg[SELECTED] = \"#000000\"\n"                                             \
  "fg[INSENSITIVE] = \"" INK_DISABLED_GRAY "\"\n"                            \
  "text[NORMAL] = \"#000000\"\n"                                             \
  "text[PRELIGHT] = \"#000000\"\n"                                           \
  "text[ACTIVE] = \"" fa "\"\n"                                              \
  "text[SELECTED] = \"#000000\"\n"                                           \
  "text[INSENSITIVE] = \"" INK_DISABLED_GRAY "\"\n"                          \
  "base[NORMAL] = \"#ffffff\"\n"                                             \
  "base[PRELIGHT] = \"#ffffff\"\n"                                           \
  "base[ACTIVE] = \"#ffffff\"\n"                                             \
  "base[SELECTED] = \"" INK_SELECTED_GRAY "\"\n"                             \
  "base[INSENSITIVE] = \"#ffffff\"\n"

#define INK_RC                                                               \
  "style \"ink-base\" {\n"                                                   \
  INK_PALETTE("#ffffff", "#000000", "#ffffff")                               \
  "  xthickness = 2\n"                                                       \
  "  ythickness = 2\n"                                                       \
  "  GtkWidget::focus-line-width = 0\n"                                      \
  "  GtkWidget::focus-padding = 0\n"                                         \
  "  GtkWidget::interior-focus = 1\n"                                        \
  "  GtkButton::default-border = { 0, 0, 0, 0 }\n"                           \
  "  GtkButton::default-outside-border = { 0, 0, 0, 0 }\n"                   \
  "  GtkButton::inner-border = { 4, 4, 4, 4 }\n"                             \
  "  GtkButton::child-displacement-x = 0\n"                                  \
  "  GtkButton::child-displacement-y = 0\n"                                  \
  "  GtkEntry::inner-border = { 10, 10, 6, 6 }\n"                            \
  "}\n"                                                                      \
  /* Selected toggle = light gray with black text; pressing one also shows  \
   * gray, so a tap is acknowledged and committed in one repaint. */         \
  "style \"ink-toggle\" = \"ink-base\" {\n"                                  \
  "  bg[ACTIVE] = \"" INK_SELECTED_GRAY "\"\n"                               \
  "  light[ACTIVE] = \"" INK_SELECTED_GRAY "\"\n"                            \
  "  dark[ACTIVE] = \"" INK_SELECTED_GRAY "\"\n"                             \
  "  mid[ACTIVE] = \"" INK_SELECTED_GRAY "\"\n"                              \
  "  fg[ACTIVE] = \"#000000\"\n"                                             \
  "  text[ACTIVE] = \"#000000\"\n"                                           \
  "}\n"                                                                      \
  /* A plain action that is drawn gray at rest (the stream bar's Mode). */   \
  "style \"ink-marked\" = \"ink-base\" {\n"                                  \
  "  bg[NORMAL] = \"" INK_SELECTED_GRAY "\"\n"                               \
  "  bg[PRELIGHT] = \"" INK_SELECTED_GRAY "\"\n"                             \
  "  bg[INSENSITIVE] = \"" INK_SELECTED_GRAY "\"\n"                          \
  "  light[NORMAL] = \"" INK_SELECTED_GRAY "\"\n"                            \
  "  dark[NORMAL] = \"" INK_SELECTED_GRAY "\"\n"                             \
  "  mid[NORMAL] = \"" INK_SELECTED_GRAY "\"\n"                              \
  "}\n"                                                                      \
  "style \"ink-field\" = \"ink-base\" {\n"                                   \
  "  xthickness = 4\n"                                                       \
  "  ythickness = 4\n"                                                       \
  "}\n"                                                                      \
  "class \"GtkWidget\" style \"ink-base\"\n"                                 \
  "widget \"*" INK_NAME_TOGGLE "*\" style \"ink-toggle\"\n"                  \
  "widget \"*" INK_NAME_MARKED "*\" style \"ink-marked\"\n"                  \
  "widget \"*" INK_NAME_FIELD "*\" style \"ink-field\"\n"

static inline void ink_set_bool_setting(GtkSettings *settings, const char *name,
                                        gboolean value) {
  /* Older GTK2 builds lack some of these properties; setting an unknown one
   * would only log a warning, but checking keeps the console clean. */
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings), name) != NULL)
    g_object_set(settings, name, value, NULL);
}

/* Call once after gtk_init() and before any widget is created. Safe to call
 * again (the stream window does); the second call is a no-op. */
static inline void ink_style_init(void) {
  GtkSettings *settings = gtk_settings_get_default();
  if (settings == NULL)
    return;
  if (g_object_get_data(G_OBJECT(settings), "ink-style-applied") != NULL)
    return;
  g_object_set_data(G_OBJECT(settings), "ink-style-applied", GINT_TO_POINTER(1));

  /* Touchscreen mode: no hover/prelight state, which on a touch panel would
   * otherwise stay stuck on the last button touched. */
  ink_set_bool_setting(settings, "gtk-touchscreen", TRUE);
  /* A blinking caret is a panel refresh every second, forever. */
  ink_set_bool_setting(settings, "gtk-cursor-blink", FALSE);
  ink_set_bool_setting(settings, "gtk-enable-animations", FALSE);
  ink_set_bool_setting(settings, "gtk-button-images", FALSE);

  gtk_rc_parse_string(INK_RC);
  if (access("ink.rc", R_OK) == 0)
    gtk_rc_parse("ink.rc");
}

/* Overdraw a crisp black outline on top of whatever the theme painted. It
 * runs after the widget's own expose handler, so the outline is the last
 * thing drawn and the label inside is untouched (it sits inside the
 * padding). Insensitive widgets get the gray outline instead. */
static inline gboolean ink_border_expose(GtkWidget *widget,
                                         GdkEventExpose *event, gpointer data) {
  /* Entries also expose an inner text window; only the frame window counts. */
  if (event->window != widget->window)
    return FALSE;
  int thickness = GPOINTER_TO_INT(data);
  GtkAllocation area = widget->allocation;
  /* Windowless widgets (buttons) draw into the parent window at their
   * allocation; widgets with their own window start at 0,0. */
  int x = gtk_widget_get_has_window(widget) ? 0 : area.x;
  int y = gtk_widget_get_has_window(widget) ? 0 : area.y;
  GdkGC *gc = gtk_widget_is_sensitive(widget)
                  ? widget->style->black_gc
                  : widget->style->fg_gc[GTK_STATE_INSENSITIVE];
  for (int i = 0; i < thickness; i++) {
    int w = area.width - 1 - 2 * i;
    int h = area.height - 1 - 2 * i;
    if (w <= 0 || h <= 0)
      break;
    gdk_draw_rectangle(widget->window, gc, FALSE, x + i, y + i, w, h);
  }
  return FALSE;
}

/* Style a button or toggle: name it for the RC style, take it out of the
 * focus chain (a focus ring is a stray repaint on touch) and attach the
 * outline. `name` is one of the INK_NAME_* constants. */
static inline GtkWidget *ink_style_button(GtkWidget *button, const char *name,
                                          int border) {
  gtk_widget_set_name(button, name);
  gtk_widget_set_can_focus(button, FALSE);
  gtk_button_set_focus_on_click(GTK_BUTTON(button), FALSE);
  g_signal_connect_after(button, "expose-event", G_CALLBACK(ink_border_expose),
                         GINT_TO_POINTER(border));
  return button;
}

/* Style a read-only value box. The address field is driven by the on-screen
 * keypad, so it never needs focus (and without focus it has no caret). */
static inline GtkWidget *ink_style_field(GtkWidget *entry) {
  gtk_widget_set_name(entry, INK_NAME_FIELD);
  gtk_widget_set_can_focus(entry, FALSE);
  g_signal_connect_after(entry, "expose-event", G_CALLBACK(ink_border_expose),
                         GINT_TO_POINTER(INK_BORDER_THICK));
  return entry;
}

/* A solid black horizontal rule. Painted with an explicit background rather
 * than a GtkSeparator, whose etched look is theme dependent. */
static inline GtkWidget *ink_rule(int thickness) {
  GtkWidget *rule = gtk_event_box_new();
  GdkColor black = {0, 0, 0, 0};
  gtk_widget_set_name(rule, "ink-rule");
  gtk_widget_modify_bg(rule, GTK_STATE_NORMAL, &black);
  gtk_widget_set_size_request(rule, -1, thickness);
  return rule;
}

#endif
