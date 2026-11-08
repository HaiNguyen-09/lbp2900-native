/*
 * lbp2900_ui.c — Canon LASER SHOT LBP-2900
 * CAPT Status Monitor & Maintenance UI (Compiled C + GTK4 + Cairo)
 *
 * Ultra-lightweight native GTK4 binary (no Python/Electron overhead).
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
 * Minimal GTK4 / GDK4 / GObject / GLib / Cairo C ABI declarations
 * (Compiles directly against /lib/x86_64-linux-gnu/libgtk-4.so.1)
 */
typedef void GtkWidget;
typedef void GtkWindow;
typedef void GtkBox;
typedef void GtkNotebook;
typedef void GtkLabel;
typedef void GtkButton;
typedef void GtkCheckButton;
typedef void GtkProgressBar;
typedef void GtkDrawingArea;
typedef void GtkCssProvider;
typedef void GdkDisplay;
typedef void GMainLoop;
typedef void GMainContext;
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

extern void gtk_init(void);
extern GMainLoop *g_main_loop_new(GMainContext *context, gboolean is_running);
extern void g_main_loop_run(GMainLoop *loop);
extern void g_main_loop_quit(GMainLoop *loop);

extern GtkWidget *gtk_window_new(void);
extern void gtk_window_set_title(GtkWindow *window, const char *title);
extern void gtk_window_set_default_size(GtkWindow *window, int width, int height);
extern void gtk_window_set_resizable(GtkWindow *window, gboolean resizable);
extern void gtk_window_set_child(GtkWindow *window, GtkWidget *child);
extern void gtk_window_present(GtkWindow *window);
extern void gtk_window_set_default_icon_name(const char *name);
extern void gtk_window_set_icon_name(GtkWindow *window, const char *name);

typedef void GtkIconTheme;
extern GtkIconTheme *gtk_icon_theme_get_for_display(GdkDisplay *display);
extern void gtk_icon_theme_add_search_path(GtkIconTheme *self, const char *path);
extern void g_set_prgname(const char *prgname);
extern void g_set_application_name(const char *application_name);

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

extern GtkWidget *gtk_notebook_new(void);
extern int gtk_notebook_append_page(GtkNotebook *notebook, GtkWidget *child, GtkWidget *tab_label);
extern void gtk_notebook_set_current_page(GtkNotebook *notebook, int page_num);

extern GtkWidget *gtk_separator_new(int orientation);

extern GtkWidget *gtk_label_new(const char *str);
extern void gtk_label_set_text(GtkLabel *label, const char *str);
extern void gtk_label_set_markup(GtkLabel *label, const char *str);
extern void gtk_label_set_wrap(GtkLabel *label, gboolean wrap);
extern void gtk_label_set_xalign(GtkLabel *label, float xalign);

extern GtkWidget *gtk_button_new_with_label(const char *label);
extern void gtk_button_set_label(GtkButton *button, const char *label);

extern GtkWidget *gtk_check_button_new_with_label(const char *label);
extern void gtk_check_button_set_active(GtkCheckButton *self, gboolean setting);
extern gboolean gtk_check_button_get_active(GtkCheckButton *self);

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

extern GtkCssProvider *gtk_css_provider_new(void);
extern void gtk_css_provider_load_from_string(GtkCssProvider *css_provider, const char *string);
extern GdkDisplay *gdk_display_get_default(void);
extern void gtk_style_context_add_provider_for_display(GdkDisplay *display,
                                                       void *provider,
                                                       guint priority);

extern gulong g_signal_connect_data(gpointer instance, const gchar *detailed_signal,
                                    GCallback c_handler, gpointer data, void *destroy_data, int connect_flags);
extern guint g_timeout_add(guint interval, GSourceFunc function, gpointer data);

#define g_signal_connect(instance, detailed_signal, c_handler, data) \
    g_signal_connect_data((instance), (detailed_signal), (c_handler), (data), NULL, 0)

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
extern void cairo_arc(cairo_t *cr, double xc, double yc, double radius, double angle1, double angle2);
extern void cairo_select_font_face(cairo_t *cr, const char *family, int slant, int weight);
extern void cairo_set_font_size(cairo_t *cr, double size);
extern void cairo_show_text(cairo_t *cr, const char *utf8);

/*
 * UI State & Widgets
 */
static GMainLoop *g_loop = NULL;
static GtkWidget *g_win = NULL;
static GtkWidget *g_notebook = NULL;
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
static GtkWidget *g_chk_autopopup = NULL;
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
 * with Live LED Status Lights and Animated Paper Feed
 * (Log text is placed above USB 04a9:2676 in the footer)
 */
static void on_draw_printer(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    (void)area;
    (void)data;

    /* Strict single-preview state machine: only 1 action preview can be active at a time */
    bool is_disconnected = (!g_cur_usb || g_cur_state == PRN_STATE_DISCONNECTED);
    bool is_out_of_paper = (!is_disconnected && g_cur_state == PRN_STATE_OUT_OF_PAPER);
    bool is_error        = (!is_disconnected && !is_out_of_paper && g_cur_state == PRN_STATE_ERROR);
    bool is_printing     = (!is_disconnected && !is_out_of_paper && !is_error &&
                            (g_cur_state == PRN_STATE_PRINTING || g_cur_state == PRN_STATE_RENDERING ||
                             g_cur_state == PRN_STATE_CLEANING));
    bool is_duplex_wait  = (!is_disconnected && !is_out_of_paper && !is_error && !is_printing &&
                            g_cur_state == PRN_STATE_MANUAL_DUPLEX_WAIT);

    /* Background card */
    cairo_set_source_rgb(cr, 0.95, 0.96, 0.98);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.80, 0.83, 0.88);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);

    double cx = w * 0.5;
    double cy = h * 0.54;

    /* Preview 1: Top output paper sheet (ONLY when actively printing/rendering) */
    if (is_printing) {
        double slide = (g_anim_tick % 10) * 1.4;
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
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
    cairo_set_source_rgb(cr, 0.86, 0.87, 0.89);
    cairo_rectangle(cr, cx - 72, cy - 36, 144, 68);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.38, 0.40, 0.44);
    cairo_set_line_width(cr, 1.8);
    cairo_stroke(cr);

    /* Top Recessed Output Tray Slope (Dark grey/blue-grey like real LBP-2900) */
    cairo_set_source_rgb(cr, 0.46, 0.49, 0.54);
    cairo_rectangle(cr, cx - 52, cy - 36, 104, 22);
    cairo_fill(cr);

    /* Front Multi-Purpose Paper Tray Slot */
    cairo_set_source_rgb(cr, 0.68, 0.70, 0.74);
    cairo_rectangle(cr, cx - 48, cy + 12, 96, 20);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.40, 0.42, 0.46);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* Preview 2: Paper in Lower Input Tray (red X ONLY when in Out-of-Paper state) */
    if (!is_out_of_paper) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_rectangle(cr, cx - 36, cy + 20, 72, 24);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.55, 0.58, 0.62);
        cairo_stroke(cr);
    } else {
        /* Flashing red empty tray indicator */
        if (g_anim_tick % 2 == 0) {
            cairo_set_source_rgba(cr, 0.90, 0.15, 0.15, 0.22);
            cairo_rectangle(cr, cx - 40, cy + 16, 80, 28);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, 0.85, 0.10, 0.10);
        cairo_set_line_width(cr, 2.5);
        cairo_move_to(cr, cx - 12, cy + 20);
        cairo_line_to(cr, cx + 12, cy + 40);
        cairo_move_to(cr, cx + 12, cy + 20);
        cairo_line_to(cr, cx - 12, cy + 40);
        cairo_stroke(cr);
    }

    /* Canon Brand Label on Front Cover */
    cairo_set_source_rgb(cr, 0.15, 0.18, 0.24);
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
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.55); /* Off */
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
        cairo_set_source_rgb(cr, 0.72, 0.74, 0.77);
    }
    cairo_arc(cr, cx + 56, cy - 4, 6.5, 0, 2 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    /* Preview 3: Unplugged USB Connector Plug Icon Preview (ONLY when disconnected) */
    if (is_disconnected) {
        cairo_set_source_rgba(cr, 0.08, 0.10, 0.14, 0.18);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);

        double ux = 34.0, uy = h - 34.0;
        /* Detached cable tail */
        cairo_set_source_rgb(cr, 0.25, 0.28, 0.32);
        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, 6, uy);
        cairo_line_to(cr, ux - 14, uy);
        cairo_stroke(cr);

        /* USB Plug Body */
        cairo_set_source_rgb(cr, 0.96, 0.97, 0.99);
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
        cairo_set_source_rgba(cr, 0.88, 0.12, 0.12, (g_anim_tick % 2 == 0) ? 0.95 : 0.65);
        cairo_set_line_width(cr, 2.6);
        cairo_arc(cr, ux + 2, uy, 16.0, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, ux - 9, uy - 11);
        cairo_line_to(cr, ux + 13, uy + 11);
        cairo_stroke(cr);
    }
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
            if (strcmp(k, "auto_popup_ui") == 0 && g_chk_autopopup) {
                gtk_check_button_set_active(g_chk_autopopup, atoi(v) != 0);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    g_loading_prefs = 0;
}

static void on_autopopup_toggled(GtkCheckButton *chk, gpointer d) {
    (void)d;
    if (g_loading_prefs)
        return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "SET_PREF auto_popup_ui=%d",
             gtk_check_button_get_active(chk) ? 1 : 0);
    send_ctrl_cmd(cmd, NULL, 0);
}

static void on_btn_test_page_1(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("TEST_PAGE 1", NULL, 0);
    gtk_notebook_set_current_page(g_notebook, 0);
}

static void on_btn_test_page_3(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_ctrl_cmd("TEST_PAGE 3", NULL, 0);
    gtk_notebook_set_current_page(g_notebook, 0);
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
    gtk_notebook_set_current_page(g_notebook, 0);
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

static gboolean poll_daemon_status_timer(gpointer data) {
    (void)data;
    g_anim_tick++;
    char resp[4096];
    if (send_ctrl_cmd("STATUS", resp, sizeof(resp)) != 0) {
        gtk_label_set_markup(g_lbl_headline,
                             "<span size='large' weight='bold' foreground='#b91c1c'>Daemon Not Running (lbp2900d)</span>");
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
            gtk_notebook_set_current_page(g_notebook, 0);
            gtk_window_present(g_win);
        }
        g_prev_state = g_cur_state;
        g_prev_pending = g_pending_jobs;
    }

    gtk_widget_remove_css_class(g_banner_box, "banner-ready");
    gtk_widget_remove_css_class(g_banner_box, "banner-printing");
    gtk_widget_remove_css_class(g_banner_box, "banner-alert");

    const char *color_hex = "#0f172a";
    if (!g_cur_usb || g_cur_state == PRN_STATE_DISCONNECTED ||
        g_cur_state == PRN_STATE_OUT_OF_PAPER || g_cur_state == PRN_STATE_ERROR) {
        gtk_widget_add_css_class(g_banner_box, "banner-alert");
        color_hex = "#b91c1c";
    } else if (g_cur_state == PRN_STATE_PRINTING || g_cur_state == PRN_STATE_RENDERING ||
               g_cur_state == PRN_STATE_CLEANING || g_cur_state == PRN_STATE_MANUAL_DUPLEX_WAIT) {
        gtk_widget_add_css_class(g_banner_box, "banner-printing");
        color_hex = "#1d4ed8";
    } else {
        gtk_widget_add_css_class(g_banner_box, "banner-ready");
        color_hex = "#15803d";
    }

    char safe_headline[256];
    size_t sp = 0;
    for (size_t i = 0; headline[i] && sp + 6 < sizeof(safe_headline); ++i) {
        if (headline[i] == '&') {
            memcpy(safe_headline + sp, "&amp;", 5);
            sp += 5;
        } else {
            safe_headline[sp++] = headline[i];
        }
    }
    safe_headline[sp] = '\0';

    char markup[384];
    snprintf(markup, sizeof(markup),
             "<span size='large' weight='bold' foreground='%s'>%s</span>",
             color_hex, safe_headline);
    gtk_label_set_markup(g_lbl_headline, markup);
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

static GtkWidget *build_status_monitor_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_widget_margins(vbox, 12, 12, 12, 12);

    /* Top Row: Left = Printer Illustration (normal 210x175), Right = Live Status Box */
    GtkWidget *top_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(vbox, top_hbox);

    g_drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(g_drawing_area, 210);
    gtk_drawing_area_set_content_height(g_drawing_area, 175);
    gtk_widget_set_size_request(g_drawing_area, 210, 175);
    gtk_widget_set_hexpand(g_drawing_area, 0);
    gtk_widget_set_vexpand(g_drawing_area, 0);
    gtk_drawing_area_set_draw_func(g_drawing_area, on_draw_printer, NULL, NULL);
    gtk_box_append(top_hbox, g_drawing_area);

    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(right_vbox, 1);
    gtk_box_append(top_hbox, right_vbox);

    g_banner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_vexpand(g_banner_box, 1);
    gtk_widget_add_css_class(g_banner_box, "status-card");
    gtk_widget_add_css_class(g_banner_box, "banner-ready");

    g_lbl_headline = gtk_label_new("Ready to Print");
    gtk_label_set_xalign(g_lbl_headline, 0.0f);
    gtk_box_append(g_banner_box, g_lbl_headline);

    g_lbl_detail = gtk_label_new("Printer is ready");
    gtk_label_set_xalign(g_lbl_detail, 0.0f);
    gtk_label_set_wrap(g_lbl_detail, 1);
    gtk_box_append(g_banner_box, g_lbl_detail);

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
    gtk_widget_set_size_request(g_btn_resume_paper, 200, 36);
    gtk_widget_set_hexpand(g_btn_resume_paper, 0);
    g_signal_connect(g_btn_resume_paper, "clicked", G_CALLBACK(on_btn_resume_paper_clicked), NULL);
    gtk_box_append(btn_hbox, g_btn_resume_paper);

    g_btn_pause = gtk_button_new_with_label("⏸ Pause Queue");
    gtk_widget_set_size_request(g_btn_pause, 150, 36);
    gtk_widget_set_hexpand(g_btn_pause, 0);
    g_signal_connect(g_btn_pause, "clicked", G_CALLBACK(on_btn_pause_toggle), NULL);
    gtk_box_append(btn_hbox, g_btn_pause);

    g_btn_cancel = gtk_button_new_with_label("✕ Cancel Job");
    gtk_widget_set_size_request(g_btn_cancel, 150, 36);
    gtk_widget_set_hexpand(g_btn_cancel, 0);
    g_signal_connect(g_btn_cancel, "clicked", G_CALLBACK(on_btn_cancel_clicked), NULL);
    gtk_box_append(btn_hbox, g_btn_cancel);

    /* Bottom telemetry footer: Log placed directly above "USB 04a9:..." */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(vbox, sep);

    g_lbl_job_info = gtk_label_new("No active print job in queue  |  Completed Jobs: 0  |  Pages Printed: 0");
    gtk_label_set_xalign(g_lbl_job_info, 0.0f);
    gtk_box_append(vbox, g_lbl_job_info);

    g_lbl_tech_stats = gtk_label_new("USB 04a9:2676: Checking...");
    gtk_label_set_xalign(g_lbl_tech_stats, 0.0f);
    gtk_box_append(vbox, g_lbl_tech_stats);

    return vbox;
}

static GtkWidget *build_maintenance_tab(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_widget_margins(vbox, 14, 14, 14, 14);

    GtkWidget *lbl_intro = gtk_label_new(NULL);
    gtk_label_set_markup(lbl_intro,
        "<b>Canon LASER SHOT LBP-2900 Diagnostic &amp; Maintenance Utility</b>\n"
        "Verify single-page printing, multi-page continuous pipelining, Out-of-Paper recovery,\n"
        "and fixing assembly cleaning directly over USB (04a9:2676).");
    gtk_label_set_xalign(lbl_intro, 0.0f);
    gtk_box_append(vbox, lbl_intro);

    g_chk_autopopup = gtk_check_button_new_with_label(
        "Automatically open Status Monitor when printing from any application");
    gtk_check_button_set_active(g_chk_autopopup, 1);
    g_signal_connect(g_chk_autopopup, "toggled", G_CALLBACK(on_autopopup_toggled), NULL);
    gtk_box_append(vbox, g_chk_autopopup);

    GtkWidget *btn_test1 = gtk_button_new_with_label("Print Windows-Style Canon LBP-2900 Test Page (1 Page)");
    gtk_widget_set_size_request(btn_test1, 430, 36);
    gtk_widget_set_hexpand(btn_test1, 0);
    gtk_widget_set_halign(btn_test1, GTK_ALIGN_START);
    g_signal_connect(btn_test1, "clicked", G_CALLBACK(on_btn_test_page_1), NULL);
    gtk_box_append(vbox, btn_test1);

    GtkWidget *btn_test3 = gtk_button_new_with_label("Print Multi-Page Continuous Pipeline Test (3 Pages)");
    gtk_widget_set_size_request(btn_test3, 430, 36);
    gtk_widget_set_hexpand(btn_test3, 0);
    gtk_widget_set_halign(btn_test3, GTK_ALIGN_START);
    g_signal_connect(btn_test3, "clicked", G_CALLBACK(on_btn_test_page_3), NULL);
    gtk_box_append(vbox, btn_test3);

    GtkWidget *btn_clean = gtk_button_new_with_label("Clean Fixing Assembly (Fuser Roller Cleaning Cycle)");
    gtk_widget_set_size_request(btn_clean, 430, 36);
    gtk_widget_set_hexpand(btn_clean, 0);
    gtk_widget_set_halign(btn_clean, GTK_ALIGN_START);
    g_signal_connect(btn_clean, "clicked", G_CALLBACK(on_btn_clean_clicked), NULL);
    gtk_box_append(vbox, btn_clean);

    GtkWidget *btn_poll = gtk_button_new_with_label("Query Live USB Extended Status (0xA0A8)");
    gtk_widget_set_size_request(btn_poll, 430, 36);
    gtk_widget_set_hexpand(btn_poll, 0);
    gtk_widget_set_halign(btn_poll, GTK_ALIGN_START);
    g_signal_connect(btn_poll, "clicked", G_CALLBACK(on_btn_poll_usb), NULL);
    gtk_box_append(vbox, btn_poll);

    return vbox;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    g_set_prgname("canon-lbp2900");
    g_set_application_name("Canon LBP2900 Status Monitor");
    gtk_init();

    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (icon_theme) {
        gtk_icon_theme_add_search_path(icon_theme, "/usr/share/icons/hicolor");
        gtk_icon_theme_add_search_path(icon_theme, "icons/hicolor");
        gtk_icon_theme_add_search_path(icon_theme, "/home/phoenix/Documents/printtesting/icons/hicolor");
    }
    gtk_window_set_default_icon_name("canon-lbp2900");

    const char *css =
        ".status-card { padding: 10px 14px; border-radius: 6px; border: 1px solid #cbd5e1; }\n"
        ".banner-ready { background-color: #f0fdf4; border-color: #86efac; }\n"
        ".banner-printing { background-color: #eff6ff; border-color: #93c5fd; }\n"
        ".banner-alert { background-color: #fef2f2; border-color: #f87171; border-width: 2px; }\n";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), provider, 600);

    g_win = gtk_window_new();
    gtk_window_set_title(g_win, "LBP2900 Status Monitor");
    gtk_window_set_icon_name(g_win, "canon-lbp2900");
    gtk_window_set_default_size(g_win, 680, 400);
    g_signal_connect(g_win, "close-request", G_CALLBACK(on_window_close_request), NULL);

    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(g_win, root_vbox);

    g_notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(g_notebook, 1);
    gtk_box_append(root_vbox, g_notebook);

    gtk_notebook_append_page(g_notebook, build_status_monitor_tab(),
                             gtk_label_new("CAPT Status Monitor"));
    gtk_notebook_append_page(g_notebook, build_maintenance_tab(),
                             gtk_label_new("Maintenance & Test"));

    load_autopopup_pref();
    poll_daemon_status_timer(NULL);
    g_timeout_add(250, poll_daemon_status_timer, NULL);

    gtk_window_present(g_win);

    g_loop = g_main_loop_new(NULL, 0);
    g_main_loop_run(g_loop);
    return 0;
}
