/*
 * lbp2900_ui.c — Canon LASER SHOT LBP-2900
 * CAPT Status Monitor & Maintenance UI (Compiled C + GTK4 + Libadwaita + Cairo)
 *
 * Ultra-lightweight native GTK4 + Libadwaita (libadwaita-1) binary.
 * Connects to lbp2900d via /tmp/lbp2900-control.sock.
 */

#define _GNU_SOURCE
#include "capt_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>

/*
 * Minimal GTK4 / Libadwaita-1 / GDK4 / GObject / GLib / Cairo C ABI declarations
 * (Compiles directly against libadwaita-1.so.0 and libgtk-4.so.1)
 */
typedef void GtkWidget;
typedef void GtkWindow;
typedef void GtkBox;
typedef void GtkLabel;
typedef void GtkButton;
typedef void GtkProgressBar;
typedef void GtkDrawingArea;
typedef void GdkDisplay;
typedef void GMainLoop;
typedef void GMainContext;
typedef void GtkIconTheme;
typedef void AdwStyleManager;
typedef void AdwViewStack;
typedef void AdwViewStackPage;
typedef void AdwViewSwitcher;
typedef void AdwPreferencesPage;
typedef void AdwPreferencesGroup;
typedef void AdwPreferencesRow;
typedef void AdwActionRow;
typedef void AdwSwitchRow;
typedef void *gpointer;
typedef int gboolean;
typedef unsigned int guint;
typedef unsigned long gulong;
typedef double gdouble;
typedef char gchar;
typedef void (*GCallback)(void);
typedef void (*GDestroyNotify)(gpointer data);
typedef gboolean (*GSourceFunc)(gpointer user_data);
typedef struct _cairo cairo_t;
typedef void (*GtkDrawingAreaDrawFunc)(GtkDrawingArea *drawing_area,
                                       cairo_t *cr,
                                       int width,
                                       int height,
                                       gpointer user_data);

#define G_CALLBACK(f) ((GCallback)(f))
#define GTK_ORIENTATION_HORIZONTAL 0
#define GTK_ORIENTATION_VERTICAL   1
#define GTK_ALIGN_FILL   0
#define GTK_ALIGN_START  1
#define GTK_ALIGN_END    2
#define GTK_ALIGN_CENTER 3

/* AdwColorScheme enum (libadwaita-1) */
#define ADW_COLOR_SCHEME_DEFAULT      0
#define ADW_COLOR_SCHEME_FORCE_LIGHT  1
#define ADW_COLOR_SCHEME_PREFER_LIGHT 2
#define ADW_COLOR_SCHEME_PREFER_DARK  3
#define ADW_COLOR_SCHEME_FORCE_DARK   4

/* AdwViewSwitcherPolicy enum */
#define ADW_VIEW_SWITCHER_POLICY_NARROW 0
#define ADW_VIEW_SWITCHER_POLICY_WIDE   1

/* GTK4 & GLib Core */
extern void gtk_init(void);
extern GMainLoop *g_main_loop_new(GMainContext *context, gboolean is_running);
extern void g_main_loop_run(GMainLoop *loop);
extern void g_main_loop_quit(GMainLoop *loop);
extern void g_set_prgname(const char *prgname);
extern void g_set_application_name(const char *application_name);

extern void gtk_window_set_title(GtkWindow *window, const char *title);
extern void gtk_window_set_default_size(GtkWindow *window, int width, int height);
extern void gtk_window_set_resizable(GtkWindow *window, gboolean resizable);
extern void gtk_window_present(GtkWindow *window);
extern void gtk_window_set_default_icon_name(const char *name);
extern void gtk_window_set_icon_name(GtkWindow *window, const char *name);

extern GdkDisplay *gdk_display_get_default(void);
extern GtkIconTheme *gtk_icon_theme_get_for_display(GdkDisplay *display);
extern void gtk_icon_theme_add_search_path(GtkIconTheme *self, const char *path);

extern void gtk_widget_queue_draw(GtkWidget *widget);
extern void gtk_widget_set_size_request(GtkWidget *widget, int width, int height);
extern void gtk_widget_set_hexpand(GtkWidget *widget, gboolean expand);
extern void gtk_widget_set_vexpand(GtkWidget *widget, gboolean expand);
extern void gtk_widget_set_halign(GtkWidget *widget, int align);
extern void gtk_widget_set_valign(GtkWidget *widget, int align);
extern void gtk_widget_set_margin_start(GtkWidget *widget, int margin);
extern void gtk_widget_set_margin_end(GtkWidget *widget, int margin);
extern void gtk_widget_set_margin_top(GtkWidget *widget, int margin);
extern void gtk_widget_set_margin_bottom(GtkWidget *widget, int margin);
extern void gtk_widget_set_sensitive(GtkWidget *widget, gboolean sensitive);
extern void gtk_widget_add_css_class(GtkWidget *widget, const char *css_class);
extern void gtk_widget_remove_css_class(GtkWidget *widget, const char *css_class);

extern GtkWidget *gtk_box_new(int orientation, int spacing);
extern void gtk_box_append(GtkBox *box, GtkWidget *child);

extern GtkWidget *gtk_label_new(const char *str);
extern void gtk_label_set_text(GtkLabel *label, const char *str);
extern void gtk_label_set_wrap(GtkLabel *label, gboolean wrap);
extern void gtk_label_set_xalign(GtkLabel *label, float xalign);

extern GtkWidget *gtk_button_new_with_label(const char *label);
extern void gtk_button_set_label(GtkButton *button, const char *label);

extern GtkWidget *gtk_progress_bar_new(void);
extern void gtk_progress_bar_set_fraction(GtkProgressBar *pbar, gdouble fraction);
extern void gtk_progress_bar_set_text(GtkProgressBar *pbar, const char *text);
extern void gtk_progress_bar_set_show_text(GtkProgressBar *pbar, gboolean show_text);

extern GtkWidget *gtk_drawing_area_new(void);
extern void gtk_drawing_area_set_content_width(GtkDrawingArea *self, int width);
extern void gtk_drawing_area_set_content_height(GtkDrawingArea *self, int height);
extern void gtk_drawing_area_set_draw_func(GtkDrawingArea *self,
                                           GtkDrawingAreaDrawFunc draw_func,
                                           gpointer user_data,
                                           GDestroyNotify destroy);

extern gulong g_signal_connect_data(gpointer instance, const gchar *detailed_signal,
                                    GCallback c_handler, gpointer data, void *destroy_data, int connect_flags);
extern guint g_timeout_add(guint interval, GSourceFunc function, gpointer data);

#define g_signal_connect(instance, detailed_signal, c_handler, data) \
    g_signal_connect_data((instance), (detailed_signal), (c_handler), (data), NULL, 0)

/* Libadwaita (libadwaita-1) Core & Widgets */
extern void adw_init(void);
extern AdwStyleManager *adw_style_manager_get_default(void);
extern void adw_style_manager_set_color_scheme(AdwStyleManager *self, int color_scheme);
extern gboolean adw_style_manager_get_dark(AdwStyleManager *self);

extern GtkWidget *adw_application_window_new(void *app);
extern void adw_application_window_set_content(GtkWindow *self, GtkWidget *content);

extern GtkWidget *adw_toolbar_view_new(void);
extern void adw_toolbar_view_add_top_bar(GtkWidget *self, GtkWidget *widget);
extern void adw_toolbar_view_set_content(GtkWidget *self, GtkWidget *content);

extern GtkWidget *adw_header_bar_new(void);
extern void adw_header_bar_set_title_widget(GtkWidget *self, GtkWidget *title_widget);
extern void adw_header_bar_set_show_start_title_buttons(GtkWidget *self, gboolean setting);
extern void adw_header_bar_set_show_end_title_buttons(GtkWidget *self, gboolean setting);

extern GtkWidget *adw_view_stack_new(void);
extern AdwViewStackPage *adw_view_stack_add_titled_with_icon(AdwViewStack *self,
                                                             GtkWidget *child,
                                                             const char *name,
                                                             const char *title,
                                                             const char *icon_name);
extern void adw_view_stack_set_visible_child_name(AdwViewStack *self, const char *name);

extern GtkWidget *adw_view_switcher_new(void);
extern void adw_view_switcher_set_stack(AdwViewSwitcher *self, AdwViewStack *stack);
extern void adw_view_switcher_set_policy(AdwViewSwitcher *self, int policy);

extern GtkWidget *adw_preferences_page_new(void);
extern void adw_preferences_page_add(AdwPreferencesPage *self, AdwPreferencesGroup *group);

extern GtkWidget *adw_preferences_group_new(void);
extern void adw_preferences_group_set_title(AdwPreferencesGroup *self, const char *title);
extern void adw_preferences_group_set_description(AdwPreferencesGroup *self, const char *description);
extern void adw_preferences_group_add(AdwPreferencesGroup *self, GtkWidget *child);

extern void adw_preferences_row_set_title(AdwPreferencesRow *self, const char *title);

extern GtkWidget *adw_action_row_new(void);
extern void adw_action_row_set_subtitle(AdwActionRow *self, const char *subtitle);
extern void adw_action_row_add_suffix(AdwActionRow *self, GtkWidget *widget);
extern void adw_action_row_set_activatable_widget(AdwActionRow *self, GtkWidget *widget);

extern GtkWidget *adw_switch_row_new(void);
extern void adw_switch_row_set_active(AdwSwitchRow *self, gboolean is_active);
extern gboolean adw_switch_row_get_active(AdwSwitchRow *self);

/* Cairo drawing functions */
extern void cairo_set_source_rgb(cairo_t *cr, double red, double green, double blue);
extern void cairo_set_source_rgba(cairo_t *cr, double red, double green, double blue, double alpha);
extern void cairo_rectangle(cairo_t *cr, double x, double y, double width, double height);
extern void cairo_fill(cairo_t *cr);
extern void cairo_fill_preserve(cairo_t *cr);
extern void cairo_stroke(cairo_t *cr);
extern void cairo_set_line_width(cairo_t *cr, double width);
extern void cairo_move_to(cairo_t *cr, double x, double y);
extern void cairo_line_to(cairo_t *cr, double x, double y);
extern void cairo_new_sub_path(cairo_t *cr);
extern void cairo_arc(cairo_t *cr, double xc, double yc, double radius, double angle1, double angle2);
extern void cairo_select_font_face(cairo_t *cr, const char *family, int slant, int weight);
extern void cairo_set_font_size(cairo_t *cr, double size);
extern void cairo_show_text(cairo_t *cr, const char *utf8);

/*
 * UI State & Widgets
 */
static GMainLoop *g_loop = NULL;
static AdwStyleManager *g_style_mgr = NULL;
static GtkWidget *g_win = NULL;
static GtkWidget *g_view_stack = NULL;
static GtkWidget *g_drawing_area = NULL;
static GtkWidget *g_banner_box = NULL;
static GtkWidget *g_lbl_headline = NULL;
static GtkWidget *g_lbl_detail = NULL;
static GtkWidget *g_lbl_job_info = NULL;
static GtkWidget *g_lbl_tech_stats = NULL;
static GtkWidget *g_pbar_buf = NULL;
static GtkWidget *g_pbar_job = NULL;
static GtkWidget *g_btn_pause = NULL;
static GtkWidget *g_btn_resume_paper = NULL;
static GtkWidget *g_btn_cancel = NULL;
static GtkWidget *g_sw_autopopup = NULL;
static gboolean   g_loading_prefs = 0;

static int g_cur_state = PRN_STATE_DISCONNECTED;
static int g_cur_usb = 0;
static int g_cur_buf_slots = 15;
static int g_cur_page = 0;
static int g_total_pages = 0;
static int g_printed_pages = 0;
static int g_paper_out_latched = 0;
static int g_paused = 0;
static int g_pending_jobs = 0;
static int g_streaming_mode = 0;
static int g_anim_tick = 0;
static int g_prev_state = -1;
static int g_prev_pending = 0;

static void set_widget_margins(GtkWidget *w, int top, int bottom, int start, int end) {
    gtk_widget_set_margin_top(w, top);
    gtk_widget_set_margin_bottom(w, bottom);
    gtk_widget_set_margin_start(w, start);
    gtk_widget_set_margin_end(w, end);
}

static int send_ctrl_cmd(const char *cmd, char *resp, size_t resp_sz) {
    if (resp && resp_sz > 0)
        resp[0] = '\0';
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, LBP2900_CTRL_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    send(s, cmd, strlen(cmd), MSG_NOSIGNAL);
    send(s, "\n", 1, MSG_NOSIGNAL);
    if (resp && resp_sz > 1) {
        size_t off = 0;
        while (off < resp_sz - 1) {
            ssize_t n = recv(s, resp + off, resp_sz - 1 - off, 0);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        resp[off] = '\0';
    }
    close(s);
    return 0;
}

/*
 * Full-Size Cairo Illustration of the Canon LASER SHOT LBP-2900 Printer
 * with Live LED Status Lights, Animated Paper Feed, and Native Adwaita Dark/Light Adaptation
 */
static void on_draw_printer(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    (void)area;
    (void)data;

    gboolean is_dark = g_style_mgr ? adw_style_manager_get_dark(g_style_mgr) : 0;

    /* Strict single-preview state machine: only 1 action preview can be active at a time */
    bool is_disconnected = (!g_cur_usb || g_cur_state == PRN_STATE_DISCONNECTED);
    bool is_out_of_paper = (!is_disconnected && g_cur_state == PRN_STATE_OUT_OF_PAPER);
    bool is_error        = (!is_disconnected && !is_out_of_paper && g_cur_state == PRN_STATE_ERROR);
    bool is_printing     = (!is_disconnected && !is_out_of_paper && !is_error &&
                            (g_cur_state == PRN_STATE_PRINTING || g_cur_state == PRN_STATE_RENDERING ||
                             g_cur_state == PRN_STATE_CLEANING));
    bool is_duplex_wait  = (!is_disconnected && !is_out_of_paper && !is_error && !is_printing &&
                            g_cur_state == PRN_STATE_MANUAL_DUPLEX_WAIT);

    double cx = w * 0.5;
    double cy = h * 0.50;

    /* Preview 1: Top output paper sheet (ONLY when actively printing/rendering) */
    if (is_printing) {
        double slide = (g_anim_tick % 10) * 1.4;
        cairo_set_source_rgb(cr, 0.98, 0.99, 1.0);
        cairo_rectangle(cr, cx - 38, cy - 62 - slide, 76, 36);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.45, 0.50, 0.58);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        /* Simulated printed text lines on emerging sheet */
        cairo_set_source_rgb(cr, 0.25, 0.28, 0.32);
        for (int line = 0; line < 4; ++line) {
            cairo_move_to(cr, cx - 30, cy - 54 - slide + line * 6);
            cairo_line_to(cr, cx + 24 - (line % 2) * 12, cy - 54 - slide + line * 6);
            cairo_stroke(cr);
        }
    }

    /* Main Canon LBP-2900 Body (Classic Canon two-tone light grey + dark slate top cover) */
    if (is_dark)
        cairo_set_source_rgb(cr, 0.78, 0.80, 0.84);
    else
        cairo_set_source_rgb(cr, 0.86, 0.87, 0.89);
    cairo_rectangle(cr, cx - 72, cy - 36, 144, 68);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.38, 0.40, 0.44);
    cairo_set_line_width(cr, 1.8);
    cairo_stroke(cr);

    /* Top Recessed Output Tray Slope (Dark grey/blue-grey like real LBP-2900) */
    cairo_set_source_rgb(cr, 0.44, 0.47, 0.52);
    cairo_rectangle(cr, cx - 52, cy - 36, 104, 22);
    cairo_fill(cr);

    /* Front Multi-Purpose Paper Tray Slot */
    cairo_set_source_rgb(cr, 0.64, 0.66, 0.70);
    cairo_rectangle(cr, cx - 48, cy + 12, 96, 20);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.40, 0.42, 0.46);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* Preview 2: Paper in Lower Input Tray (red X ONLY when in Out-of-Paper state) */
    if (!is_out_of_paper) {
        cairo_set_source_rgb(cr, 0.98, 0.99, 1.0);
        cairo_rectangle(cr, cx - 36, cy + 20, 72, 24);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.55, 0.58, 0.62);
        cairo_stroke(cr);
    } else {
        /* Flashing red empty tray indicator */
        if (g_anim_tick % 2 == 0) {
            cairo_set_source_rgba(cr, 0.90, 0.15, 0.15, 0.25);
            cairo_rectangle(cr, cx - 40, cy + 16, 80, 28);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, 0.88, 0.14, 0.14);
        cairo_set_line_width(cr, 2.5);
        cairo_move_to(cr, cx - 12, cy + 20);
        cairo_line_to(cr, cx + 12, cy + 40);
        cairo_move_to(cr, cx + 12, cy + 20);
        cairo_line_to(cr, cx - 12, cy + 40);
        cairo_stroke(cr);
    }

    /* Canon Brand Label on Front Cover */
    cairo_set_source_rgb(cr, 0.14, 0.16, 0.22);
    cairo_select_font_face(cr, "Sans", 0, 1);
    cairo_set_font_size(cr, 11.0);
    cairo_move_to(cr, cx - 22, cy - 2);
    cairo_show_text(cr, "Canon");
    cairo_set_font_size(cr, 8.5);
    cairo_move_to(cr, cx - 23, cy + 9);
    cairo_show_text(cr, "LBP-2900");

    /* Right-hand Control Panel: Power LED + Paper/Resume Button LED */
    if (!is_disconnected)
        cairo_set_source_rgb(cr, 0.12, 0.78, 0.25); /* Bright green */
    else
        cairo_set_source_rgb(cr, 0.50, 0.50, 0.50); /* Off */
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx + 56, cy - 20, 5.0, 0, 2 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    bool blink_on = (g_anim_tick % 2 == 0);
    if (is_out_of_paper || is_error || is_duplex_wait) {
        if (blink_on)
            cairo_set_source_rgb(cr, 1.0, 0.32, 0.05); /* Flashing Orange-Red */
        else
            cairo_set_source_rgb(cr, 0.45, 0.18, 0.10);
    } else if (is_printing) {
        if (blink_on)
            cairo_set_source_rgb(cr, 0.15, 0.85, 0.30);
        else
            cairo_set_source_rgb(cr, 0.10, 0.45, 0.18);
    } else {
        cairo_set_source_rgb(cr, 0.68, 0.70, 0.74);
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx + 56, cy - 4, 6.5, 0, 2 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    /* Preview 3: Unplugged USB Connector Plug Icon Preview (ONLY when disconnected) */
    if (is_disconnected) {
        cairo_set_source_rgba(cr, 0.08, 0.10, 0.14, is_dark ? 0.32 : 0.18);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);

        double ux = 34.0, uy = h - 28.0;
        /* Detached cable tail */
        cairo_set_source_rgb(cr, is_dark ? 0.65 : 0.25, is_dark ? 0.68 : 0.28, is_dark ? 0.72 : 0.32);
        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, 6, uy);
        cairo_line_to(cr, ux - 14, uy);
        cairo_stroke(cr);

        /* USB Plug Body */
        cairo_set_source_rgb(cr, 0.94, 0.95, 0.98);
        cairo_rectangle(cr, ux - 14, uy - 10, 20, 20);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.20, 0.23, 0.28);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);

        /* Metal USB Prongs */
        cairo_set_source_rgb(cr, 0.78, 0.80, 0.84);
        cairo_rectangle(cr, ux + 6, uy - 7, 10, 14);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.20, 0.23, 0.28);
        cairo_set_line_width(cr, 1.3);
        cairo_stroke(cr);

        /* Red Prohibition Circle + Slash over USB Plug */
        cairo_set_source_rgba(cr, 0.92, 0.18, 0.18, (g_anim_tick % 2 == 0) ? 0.95 : 0.65);
        cairo_set_line_width(cr, 2.6);
        cairo_new_sub_path(cr);
        cairo_arc(cr, ux + 2, uy, 16.0, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, ux - 9, uy - 11);
        cairo_line_to(cr, ux + 13, uy + 11);
        cairo_stroke(cr);
    }
}

static void on_theme_changed(AdwStyleManager *sm, gpointer pspec, gpointer data) {
    (void)sm;
    (void)pspec;
    (void)data;
    if (g_drawing_area)
        gtk_widget_queue_draw(g_drawing_area);
}

static void load_autopopup_pref(void) {
    char resp[2048];
    if (send_ctrl_cmd("GET_PREFS", resp, sizeof(resp)) != 0)
        return;

    g_loading_prefs = 1;
    char *saveptr = NULL;
    char *line = strtok_r(resp, "\n", &saveptr);
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *k = line;
            const char *v = eq + 1;
            if (strcmp(k, "auto_popup_ui") == 0 && g_sw_autopopup) {
                adw_switch_row_set_active(g_sw_autopopup, atoi(v) != 0);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    g_loading_prefs = 0;
}

static void on_autopopup_notify(AdwSwitchRow *sw, gpointer pspec, gpointer d) {
    (void)pspec;
    (void)d;
    if (g_loading_prefs)
        return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "SET_PREF auto_popup_ui=%d",
             adw_switch_row_get_active(sw) ? 1 : 0);
    send_ctrl_cmd(cmd, NULL, 0);
}

static void on_btn_test_page_1(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("TEST_PAGE 1", NULL, 0);
    adw_view_stack_set_visible_child_name(g_view_stack, "status");
}

static void on_btn_test_page_3(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("TEST_PAGE 3", NULL, 0);
    adw_view_stack_set_visible_child_name(g_view_stack, "status");
}

static void on_btn_pause_toggle(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "PAUSE %d", g_paused ? 0 : 1);
    send_ctrl_cmd(cmd, NULL, 0);
}

static void on_btn_resume_paper_clicked(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("RESUME_PAPER", NULL, 0);
}

static void on_btn_cancel_clicked(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("CANCEL", NULL, 0);
}

static void on_btn_clean_clicked(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("CLEAN", NULL, 0);
    adw_view_stack_set_visible_child_name(g_view_stack, "status");
}

static void on_btn_poll_usb(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("POLL_USB", NULL, 0);
}

static gboolean on_window_close_request(GtkWindow *win, gpointer data) {
    (void)win;
    (void)data;
    if (g_loop)
        g_main_loop_quit(g_loop);
    return 0;
}

static void apply_headline_semantic_style(const char *semantic_class) {
    gtk_widget_remove_css_class(g_lbl_headline, "success");
    gtk_widget_remove_css_class(g_lbl_headline, "accent");
    gtk_widget_remove_css_class(g_lbl_headline, "warning");
    gtk_widget_remove_css_class(g_lbl_headline, "error");
    if (semantic_class && semantic_class[0])
        gtk_widget_add_css_class(g_lbl_headline, semantic_class);
}

static gboolean poll_daemon_status_timer(gpointer data) {
    (void)data;
    g_anim_tick++;
    char resp[4096];
    if (send_ctrl_cmd("STATUS", resp, sizeof(resp)) != 0) {
        apply_headline_semantic_style("error");
        gtk_label_set_text(g_lbl_headline, "Daemon Not Running (lbp2900d)");
        gtk_label_set_text(g_lbl_detail, "Start lbp2900d to communicate with Canon LASER SHOT LBP-2900.");
        gtk_widget_queue_draw(g_drawing_area);
        return 1;
    }

    char headline[128] = "Ready to Print";
    char detail[256] = "Printer is ready";
    char job_name[128] = "";
    char job_user[64] = "";
    unsigned job_id = 0, jobs_done = 0, pages_life = 0;
    size_t rss_kb = 0;
    unsigned capt_bas = 0, capt_cnt = 0, capt_pap = 0, capt_eng = 0;

    char *saveptr = NULL;
    char *line = strtok_r(resp, "\n", &saveptr);
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *k = line;
            const char *v = eq + 1;
            if (strcmp(k, "state") == 0) g_cur_state = atoi(v);
            else if (strcmp(k, "usb_connected") == 0) g_cur_usb = atoi(v);
            else if (strcmp(k, "headline") == 0) snprintf(headline, sizeof(headline), "%s", v);
            else if (strcmp(k, "detail") == 0) snprintf(detail, sizeof(detail), "%s", v);
            else if (strcmp(k, "active_job_id") == 0) job_id = (unsigned)atoi(v);
            else if (strcmp(k, "active_job_name") == 0) snprintf(job_name, sizeof(job_name), "%s", v);
            else if (strcmp(k, "active_job_user") == 0) snprintf(job_user, sizeof(job_user), "%s", v);
            else if (strcmp(k, "current_page") == 0) g_cur_page = atoi(v);
            else if (strcmp(k, "total_pages") == 0) g_total_pages = atoi(v);
            else if (strcmp(k, "printed_pages") == 0) g_printed_pages = atoi(v);
            else if (strcmp(k, "buf_slots_free") == 0) g_cur_buf_slots = atoi(v);
            else if (strcmp(k, "streaming_mode") == 0) g_streaming_mode = atoi(v);
            else if (strcmp(k, "paper_out_latched") == 0) g_paper_out_latched = atoi(v);
            else if (strcmp(k, "paused") == 0) g_paused = atoi(v);
            else if (strcmp(k, "pending_jobs") == 0) g_pending_jobs = atoi(v);
            else if (strcmp(k, "total_jobs_completed") == 0) jobs_done = (unsigned)atoi(v);
            else if (strcmp(k, "total_pages_lifetime") == 0) pages_life = (unsigned)atoi(v);
            else if (strcmp(k, "daemon_rss_kb") == 0) rss_kb = (size_t)atol(v);
            else if (strcmp(k, "capt_bas") == 0) capt_bas = (unsigned)strtoul(v, NULL, 0);
            else if (strcmp(k, "capt_cnt") == 0) capt_cnt = (unsigned)strtoul(v, NULL, 0);
            else if (strcmp(k, "capt_pap") == 0) capt_pap = (unsigned)strtoul(v, NULL, 0);
            else if (strcmp(k, "capt_eng") == 0) capt_eng = (unsigned)strtoul(v, NULL, 0);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Auto-raise window when printing starts, Out of Paper, Error, or Disconnect WITH active/pending jobs */
    if (g_cur_state != g_prev_state || (g_pending_jobs > g_prev_pending)) {
        int disc_with_jobs = (g_cur_state == PRN_STATE_DISCONNECTED && (g_pending_jobs > 0 || job_id > 0));
        if (g_cur_state == PRN_STATE_PRINTING || g_cur_state == PRN_STATE_RENDERING ||
            g_cur_state == PRN_STATE_OUT_OF_PAPER || g_cur_state == PRN_STATE_MANUAL_DUPLEX_WAIT ||
            g_cur_state == PRN_STATE_ERROR || disc_with_jobs ||
            g_pending_jobs > g_prev_pending) {
            adw_view_stack_set_visible_child_name(g_view_stack, "status");
            gtk_window_present(g_win);
        }
        g_prev_state = g_cur_state;
        g_prev_pending = g_pending_jobs;
    }

    /* Native Libadwaita semantic classes (.error, .accent, .success) for dynamic dark/light theming */
    if (!g_cur_usb || g_cur_state == PRN_STATE_DISCONNECTED ||
        g_cur_state == PRN_STATE_OUT_OF_PAPER || g_cur_state == PRN_STATE_ERROR) {
        apply_headline_semantic_style("error");
    } else if (g_cur_state == PRN_STATE_PRINTING || g_cur_state == PRN_STATE_RENDERING ||
               g_cur_state == PRN_STATE_CLEANING || g_cur_state == PRN_STATE_MANUAL_DUPLEX_WAIT) {
        apply_headline_semantic_style("accent");
    } else {
        apply_headline_semantic_style("success");
    }

    gtk_label_set_text(g_lbl_headline, headline);
    gtk_label_set_text(g_lbl_detail, detail);

    /* Progress Bar & Log line (placed right above "USB 04a9:2676: ...") */
    char jinfo[256];
    if (!g_cur_usb || g_cur_state == PRN_STATE_DISCONNECTED) {
        snprintf(jinfo, sizeof(jinfo),
                 "NO USB CONNECTED — PRINTER OFFLINE  |  Pending FIFO Queue: %d job(s)",
                 g_pending_jobs);
        gtk_progress_bar_set_fraction(g_pbar_job, 0.0);
    } else if (g_cur_state == PRN_STATE_OUT_OF_PAPER) {
        snprintf(jinfo, sizeof(jinfo),
                 "ALERT: NO PAPER IN TRAY — Load paper & press button on printer");
        gtk_progress_bar_set_fraction(g_pbar_job, 0.0);
    } else if (g_cur_state == PRN_STATE_ERROR) {
        snprintf(jinfo, sizeof(jinfo),
                 "HARDWARE FAULT — Check cover / paper path");
        gtk_progress_bar_set_fraction(g_pbar_job, 0.0);
    } else if (job_id > 0) {
        snprintf(jinfo, sizeof(jinfo),
                 "Active Job #%u: %s (User: %s)  —  Page %d (Printed: %d)  |  Pending FIFO: %d",
                 job_id, job_name, job_user, g_cur_page, g_printed_pages, g_pending_jobs);
        double frac = (g_cur_page > 0) ? ((double)g_printed_pages / (double)g_cur_page) : 0.25;
        if (frac < 0.15) frac = 0.25;
        if (frac > 1.0) frac = 1.0;
        gtk_progress_bar_set_fraction(g_pbar_job, frac);
    } else if (g_pending_jobs > 0) {
        snprintf(jinfo, sizeof(jinfo),
                 "Pending FIFO Queue: %d job(s) waiting for printer connection  |  Completed: %u",
                 g_pending_jobs, jobs_done);
        gtk_progress_bar_set_fraction(g_pbar_job, 0.0);
    } else {
        snprintf(jinfo, sizeof(jinfo),
                 "No active print job in queue  |  Completed Jobs: %u  |  Pages Printed: %u",
                 jobs_done, pages_life);
        gtk_progress_bar_set_fraction(g_pbar_job, 0.0);
    }
    gtk_label_set_text(g_lbl_job_info, jinfo);

    /* Printer Onboard 2MB Buffer Usage bar (Host-Assisted Offload monitor) */
    int used_slots = 15 - g_cur_buf_slots;
    if (used_slots < 0) used_slots = 0;
    if (used_slots > 15) used_slots = 15;
    double buf_frac = (double)used_slots / 15.0;
    char buf_txt[128];
    snprintf(buf_txt, sizeof(buf_txt),
             "Printer RAM Buffer: %d / 15 Slots Free (%s)",
             g_cur_buf_slots,
             g_streaming_mode ? "Host Page-Streaming Active" : "Host Band Offload Active");
    gtk_progress_bar_set_fraction(g_pbar_buf, buf_frac);
    gtk_progress_bar_set_text(g_pbar_buf, buf_txt);

    char tech[256];
    snprintf(tech, sizeof(tech),
             "USB 04a9:2676: %s  |  Daemon RAM: %.2f MB  |  CAPT Bas=0x%04X Cnt=0x%02X Pap=0x%02X Eng=0x%04X",
             g_cur_usb ? "ONLINE (Persistent)" : "OFFLINE",
             (double)rss_kb / 1024.0,
             capt_bas, capt_cnt, capt_pap, capt_eng);
    gtk_label_set_text(g_lbl_tech_stats, tech);

    gtk_button_set_label(g_btn_pause, g_paused ? "▶ Resume Queue" : "⏸ Pause Queue");
    gtk_widget_set_sensitive(g_btn_resume_paper,
                             (g_cur_state == PRN_STATE_OUT_OF_PAPER ||
                              g_cur_state == PRN_STATE_MANUAL_DUPLEX_WAIT) ? 1 : 0);
    gtk_widget_set_sensitive(g_btn_cancel, (job_id > 0) ? 1 : 0);

    gtk_widget_queue_draw(g_drawing_area);
    return 1;
}

static GtkWidget *build_status_monitor_page(void) {
    GtkWidget *page = adw_preferences_page_new();

    /* Group 1: Live Printer Visual & Status Card */
    GtkWidget *grp_monitor = adw_preferences_group_new();
    adw_preferences_page_add(page, grp_monitor);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    adw_preferences_group_add(grp_monitor, vbox);

    /* Top Row: Left = Printer Illustration (.card, 210x145), Right = Compact Status Card + 2 Bars */
    GtkWidget *top_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_valign(top_hbox, GTK_ALIGN_START);
    gtk_box_append(vbox, top_hbox);

    GtkWidget *drawing_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(drawing_card, "card");
    gtk_widget_set_hexpand(drawing_card, 0);
    gtk_widget_set_vexpand(drawing_card, 0);
    gtk_widget_set_valign(drawing_card, GTK_ALIGN_START);

    g_drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(g_drawing_area, 210);
    gtk_drawing_area_set_content_height(g_drawing_area, 118);
    gtk_widget_set_size_request(g_drawing_area, 210, 118);
    gtk_widget_set_vexpand(g_drawing_area, 0);
    gtk_widget_set_valign(g_drawing_area, GTK_ALIGN_START);
    gtk_drawing_area_set_draw_func(g_drawing_area, on_draw_printer, NULL, NULL);
    gtk_box_append(drawing_card, g_drawing_area);
    gtk_box_append(top_hbox, drawing_card);

    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(right_vbox, 1);
    gtk_widget_set_vexpand(right_vbox, 0);
    gtk_widget_set_valign(right_vbox, GTK_ALIGN_START);
    gtk_box_append(top_hbox, right_vbox);

    g_banner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_vexpand(g_banner_box, 0);
    gtk_widget_set_valign(g_banner_box, GTK_ALIGN_START);
    gtk_widget_add_css_class(g_banner_box, "card");

    GtkWidget *banner_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    set_widget_margins(banner_inner, 10, 10, 14, 14);
    gtk_box_append(g_banner_box, banner_inner);

    g_lbl_headline = gtk_label_new("Ready to Print");
    gtk_label_set_xalign(g_lbl_headline, 0.0f);
    gtk_widget_add_css_class(g_lbl_headline, "title-2");
    gtk_widget_add_css_class(g_lbl_headline, "success");
    gtk_box_append(banner_inner, g_lbl_headline);

    g_lbl_detail = gtk_label_new("Printer is ready");
    gtk_label_set_xalign(g_lbl_detail, 0.0f);
    gtk_widget_add_css_class(g_lbl_detail, "dim-label");
    gtk_box_append(banner_inner, g_lbl_detail);

    gtk_box_append(right_vbox, g_banner_box);

    g_pbar_job = gtk_progress_bar_new();
    gtk_box_append(right_vbox, g_pbar_job);

    g_pbar_buf = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(g_pbar_buf, 1);
    gtk_box_append(right_vbox, g_pbar_buf);

    /* Action Buttons Bar — Fixed width (not dynamic width), symbols kept for play/pause/cancel */
    GtkWidget *btn_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_hbox, GTK_ALIGN_START);
    gtk_box_append(vbox, btn_hbox);

    g_btn_resume_paper = gtk_button_new_with_label("▶ Paper Loaded (Resume)");
    gtk_widget_set_size_request(g_btn_resume_paper, 210, 36);
    gtk_widget_set_hexpand(g_btn_resume_paper, 0);
    gtk_widget_add_css_class(g_btn_resume_paper, "suggested-action");
    g_signal_connect(g_btn_resume_paper, "clicked", G_CALLBACK(on_btn_resume_paper_clicked), NULL);
    gtk_box_append(btn_hbox, g_btn_resume_paper);

    g_btn_pause = gtk_button_new_with_label("⏸ Pause Queue");
    gtk_widget_set_size_request(g_btn_pause, 155, 36);
    gtk_widget_set_hexpand(g_btn_pause, 0);
    g_signal_connect(g_btn_pause, "clicked", G_CALLBACK(on_btn_pause_toggle), NULL);
    gtk_box_append(btn_hbox, g_btn_pause);

    g_btn_cancel = gtk_button_new_with_label("✕ Cancel Job");
    gtk_widget_set_size_request(g_btn_cancel, 155, 36);
    gtk_widget_set_hexpand(g_btn_cancel, 0);
    gtk_widget_add_css_class(g_btn_cancel, "destructive-action");
    g_signal_connect(g_btn_cancel, "clicked", G_CALLBACK(on_btn_cancel_clicked), NULL);
    gtk_box_append(btn_hbox, g_btn_cancel);

    /* Group 2: CAPT Telemetry Card — Log placed directly above "USB 04a9:2676: ..." */
    GtkWidget *grp_telemetry = adw_preferences_group_new();
    adw_preferences_page_add(page, grp_telemetry);

    GtkWidget *telemetry_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(telemetry_card, "card");
    adw_preferences_group_add(grp_telemetry, telemetry_card);

    GtkWidget *telemetry_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    set_widget_margins(telemetry_inner, 10, 10, 14, 14);
    gtk_box_append(telemetry_card, telemetry_inner);

    g_lbl_job_info = gtk_label_new("No active print job in queue  |  Completed Jobs: 0  |  Pages Printed: 0");
    gtk_label_set_xalign(g_lbl_job_info, 0.0f);
    gtk_label_set_wrap(g_lbl_job_info, 1);
    gtk_box_append(telemetry_inner, g_lbl_job_info);

    g_lbl_tech_stats = gtk_label_new("USB 04a9:2676: Checking...");
    gtk_label_set_xalign(g_lbl_tech_stats, 0.0f);
    gtk_label_set_wrap(g_lbl_tech_stats, 1);
    gtk_widget_add_css_class(g_lbl_tech_stats, "dim-label");
    gtk_widget_add_css_class(g_lbl_tech_stats, "caption");
    gtk_widget_add_css_class(g_lbl_tech_stats, "monospace");
    gtk_box_append(telemetry_inner, g_lbl_tech_stats);

    return page;
}

static GtkWidget *create_maintenance_action_row(const char *title,
                                                const char *subtitle,
                                                const char *btn_label,
                                                GCallback callback) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(row, title);
    adw_action_row_set_subtitle(row, subtitle);

    GtkWidget *btn = gtk_button_new_with_label(btn_label);
    gtk_widget_set_size_request(btn, 140, 34);
    gtk_widget_set_hexpand(btn, 0);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    g_signal_connect(btn, "clicked", callback, NULL);

    adw_action_row_add_suffix(row, btn);
    adw_action_row_set_activatable_widget(row, btn);
    return row;
}

static GtkWidget *build_maintenance_page(void) {
    GtkWidget *page = adw_preferences_page_new();

    /* Preferences Group 1: Behavior */
    GtkWidget *grp_behavior = adw_preferences_group_new();
    adw_preferences_group_set_title(grp_behavior, "Status Monitor Behavior");
    adw_preferences_page_add(page, grp_behavior);

    g_sw_autopopup = adw_switch_row_new();
    adw_preferences_row_set_title(g_sw_autopopup,
        "Automatically open Status Monitor when printing from any application");
    adw_action_row_set_subtitle(g_sw_autopopup,
        "Raise this window immediately when a print job is submitted or needs paper");
    adw_switch_row_set_active(g_sw_autopopup, 1);
    g_signal_connect(g_sw_autopopup, "notify::active", G_CALLBACK(on_autopopup_notify), NULL);
    adw_preferences_group_add(grp_behavior, g_sw_autopopup);

    /* Preferences Group 2: Diagnostic & Maintenance Operations */
    GtkWidget *grp_diag = adw_preferences_group_new();
    adw_preferences_group_set_title(grp_diag, "Canon LASER SHOT LBP-2900 Diagnostic &amp; Maintenance");
    adw_preferences_group_set_description(grp_diag,
        "Verify single-page printing, multi-page continuous pipelining, Out-of-Paper recovery, "
        "and fixing assembly cleaning directly over USB (04a9:2676).");
    adw_preferences_page_add(page, grp_diag);

    adw_preferences_group_add(grp_diag,
        create_maintenance_action_row(
            "Print Windows-Style Canon LBP-2900 Test Page (1 Page)",
            "Verify 600 DPI Hi-SCoA band rendering and USB transfer",
            "Print 1 Page",
            G_CALLBACK(on_btn_test_page_1)));

    adw_preferences_group_add(grp_diag,
        create_maintenance_action_row(
            "Print Multi-Page Continuous Pipeline Test (3 Pages)",
            "Test back-to-back multi-page pipelining and SetJobInfo2 heartbeats",
            "Print 3 Pages",
            G_CALLBACK(on_btn_test_page_3)));

    adw_preferences_group_add(grp_diag,
        create_maintenance_action_row(
            "Clean Fixing Assembly (Fuser Roller Cleaning Cycle)",
            "Run the hardware fuser cleaning routine (CAPT 0xE0AD)",
            "Run Cleaning",
            G_CALLBACK(on_btn_clean_clicked)));

    adw_preferences_group_add(grp_diag,
        create_maintenance_action_row(
            "Query Live USB Extended Status (0xA0A8)",
            "Poll hardware registers and clear any idle status flags",
            "Query USB",
            G_CALLBACK(on_btn_poll_usb)));

    return page;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_set_prgname("canon-lbp2900");
    g_set_application_name("LBP2900 Status Monitor");
    gtk_init();
    adw_init();

    /* Native Dynamic Theming via AdwStyleManager (automatically follows system Dark/Light mode) */
    g_style_mgr = adw_style_manager_get_default();
    if (g_style_mgr) {
        adw_style_manager_set_color_scheme(g_style_mgr, ADW_COLOR_SCHEME_DEFAULT);
        g_signal_connect(g_style_mgr, "notify::dark", G_CALLBACK(on_theme_changed), NULL);
    }

    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (icon_theme) {
        gtk_icon_theme_add_search_path(icon_theme, "/usr/share/icons/hicolor");
        gtk_icon_theme_add_search_path(icon_theme, "icons/hicolor");
        gtk_icon_theme_add_search_path(icon_theme, "/home/phoenix/Documents/printtesting/icons/hicolor");
    }
    gtk_window_set_default_icon_name("canon-lbp2900");

    /* Window & Header Architecture: AdwApplicationWindow + AdwToolbarView + AdwHeaderBar + AdwViewSwitcher */
    g_win = adw_application_window_new(NULL);
    gtk_window_set_title(g_win, "LBP2900 Status Monitor");
    gtk_window_set_icon_name(g_win, "canon-lbp2900");
    gtk_window_set_default_size(g_win, 660, 380);
    g_signal_connect(g_win, "close-request", G_CALLBACK(on_window_close_request), NULL);

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    GtkWidget *header_bar = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(header_bar, 1);
    adw_header_bar_set_show_end_title_buttons(header_bar, 1);

    g_view_stack = adw_view_stack_new();
    adw_view_stack_add_titled_with_icon(g_view_stack,
                                        build_status_monitor_page(),
                                        "status",
                                        "CAPT Status Monitor",
                                        "printer-symbolic");
    adw_view_stack_add_titled_with_icon(g_view_stack,
                                        build_maintenance_page(),
                                        "maintenance",
                                        "Maintenance & Test",
                                        "preferences-system-symbolic");

    GtkWidget *view_switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(view_switcher, g_view_stack);
    adw_view_switcher_set_policy(view_switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(header_bar, view_switcher);

    adw_toolbar_view_add_top_bar(toolbar_view, header_bar);
    adw_toolbar_view_set_content(toolbar_view, g_view_stack);
    adw_application_window_set_content(g_win, toolbar_view);

    load_autopopup_pref();
    poll_daemon_status_timer(NULL);
    g_timeout_add(250, poll_daemon_status_timer, NULL);

    gtk_window_present(g_win);

    g_loop = g_main_loop_new(NULL, 0);
    g_main_loop_run(g_loop);
    return 0;
}
