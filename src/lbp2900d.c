/*
 * lbp2900d.c — Standalone CUPS-Replacement Daemon & CAPT Print Engine
 * for Canon LASER SHOT LBP-2900 (04a9:2676)
 *
 * Replaces cupsd completely:
 *  - Listens on /run/cups/cups.sock, /tmp/lbp2900-cups.sock, and 127.0.0.1:6310
 *    speaking native IPP/1.1 & IPP/2.0 + PPD HTTP so every Linux app (GTK, Qt,
 *    Chromium/Helium, OnlyOffice, LibreOffice, lp/lpr) prints directly to it.
 *  - Listens on /tmp/lbp2900-control.sock for the Windows-style Status Monitor
 *    and Printing Preferences UI (lbp2900-ui) and CLI tools.
 *  - Automatically pops open the Canon LBP-2900 Status Monitor window the moment
 *    any application clicks Print or when an Out-of-Paper / Disconnect alert occurs.
 *  - Hardened for Groups 1..6:
 *      * Immediate <0.8s USB disconnect / power-off detection
 *      * Instant pre-job warning when disconnected (never hangs)
 *      * Strict FIFO spooling while disconnected & automatic sequential replay upon reconnect
 *      * Ephemeral spool cleanup on daemon startup, logout, shutdown, or OS reboot
 *      * Atomic per-job spooling under 15–50+ concurrent job bursts
 */

#define _GNU_SOURCE
#include "capt_defs.h"
#include "capt_usb.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cups/cups.h>
#include <cups/ipp.h>

static volatile sig_atomic_t g_daemon_running = 1;
static pthread_mutex_t g_prefs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lbp2900_prefs g_prefs = {
    .media_size = "A4",
    .media_type = 0,
    .copies = 1,
    .collate = 1,
    .orientation = 0,
    .nup = 1,
    .manual_duplex = 0,
    .toner_save = 0,
    .density = 7,
    .dither_mode = 0,
    .auto_popup_ui = 1,
    .page_range = "",
};

/* FIFO Spool Job Queue */
struct print_job_item {
    uint32_t job_id;
    char     job_name[128];
    char     job_user[64];
    char     filepath[256];
    bool     delete_file_after;
    struct lbp2900_prefs job_prefs;
    int      ipp_job_state; /* 3=pending, 5=processing, 7=canceled, 9=completed */
    struct print_job_item *next;
};

static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond = PTHREAD_COND_INITIALIZER;
static struct print_job_item *g_queue_head = NULL;
static struct print_job_item *g_queue_tail = NULL;
static int      g_pending_jobs_count = 0;
static uint32_t g_next_job_id = 1;
static uint32_t g_last_created_job_id = 0;

static char g_ui_binary_path[512] = "/home/phoenix/Documents/printtesting/bin/lbp2900-ui";

/*
 * Group 6.3: Ephemeral Spool Cleanup on Session Logout / Shutdown / Reboot / Startup
 * Purges all pending jobs and unlinks all files in /tmp/lbp2900-spool.
 */
static void purge_spool_directory_and_queue(void) {
    mkdir(LBP2900_SPOOL_DIR, 0755);

    pthread_mutex_lock(&g_queue_lock);
    struct print_job_item *cur = g_queue_head;
    while (cur) {
        struct print_job_item *nxt = cur->next;
        if (cur->delete_file_after && cur->filepath[0]) {
            unlink(cur->filepath);
        }
        free(cur);
        cur = nxt;
    }
    g_queue_head = NULL;
    g_queue_tail = NULL;
    g_pending_jobs_count = 0;
    pthread_mutex_unlock(&g_queue_lock);

    DIR *d = opendir(LBP2900_SPOOL_DIR);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", LBP2900_SPOOL_DIR, de->d_name);
            unlink(p);
        }
        closedir(d);
    }
    capt_set_pending_jobs_count(0, NULL);
}

static bool copy_file_to_spool(const char *src_path, const char *dst_path) {
    FILE *in = fopen(src_path, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            unlink(dst_path);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
}

static void load_prefs(void) {
    pthread_mutex_lock(&g_prefs_lock);
    FILE *f = fopen(LBP2900_CONFIG_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = line;
            char *v = eq + 1;
            size_t vl = strlen(v);
            while (vl > 0 && (v[vl - 1] == '\n' || v[vl - 1] == '\r'))
                v[--vl] = '\0';
            if (strcmp(k, "media_size") == 0) snprintf(g_prefs.media_size, sizeof(g_prefs.media_size), "%s", v);
            else if (strcmp(k, "media_type") == 0) g_prefs.media_type = atoi(v);
            else if (strcmp(k, "copies") == 0) g_prefs.copies = atoi(v);
            else if (strcmp(k, "collate") == 0) g_prefs.collate = atoi(v);
            else if (strcmp(k, "orientation") == 0) g_prefs.orientation = atoi(v);
            else if (strcmp(k, "nup") == 0) g_prefs.nup = atoi(v);
            else if (strcmp(k, "manual_duplex") == 0) g_prefs.manual_duplex = atoi(v);
            else if (strcmp(k, "toner_save") == 0) g_prefs.toner_save = atoi(v);
            else if (strcmp(k, "density") == 0) g_prefs.density = atoi(v);
            else if (strcmp(k, "dither_mode") == 0) g_prefs.dither_mode = atoi(v);
            else if (strcmp(k, "auto_popup_ui") == 0) g_prefs.auto_popup_ui = atoi(v);
            else if (strcmp(k, "page_range") == 0) snprintf(g_prefs.page_range, sizeof(g_prefs.page_range), "%s", v);
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_prefs_lock);
}

static void save_prefs(void) {
    pthread_mutex_lock(&g_prefs_lock);
    FILE *f = fopen(LBP2900_CONFIG_PATH, "w");
    if (f) {
        fprintf(f, "media_size=%s\n", g_prefs.media_size);
        fprintf(f, "media_type=%d\n", g_prefs.media_type);
        fprintf(f, "copies=%d\n", g_prefs.copies);
        fprintf(f, "collate=%d\n", g_prefs.collate);
        fprintf(f, "orientation=%d\n", g_prefs.orientation);
        fprintf(f, "nup=%d\n", g_prefs.nup);
        fprintf(f, "manual_duplex=%d\n", g_prefs.manual_duplex);
        fprintf(f, "toner_save=%d\n", g_prefs.toner_save);
        fprintf(f, "density=%d\n", g_prefs.density);
        fprintf(f, "dither_mode=%d\n", g_prefs.dither_mode);
        fprintf(f, "auto_popup_ui=%d\n", g_prefs.auto_popup_ui);
        fprintf(f, "page_range=%s\n", g_prefs.page_range);
        fclose(f);
    }
    pthread_mutex_unlock(&g_prefs_lock);
}

/*
 * Spawn or raise the Windows-style Canon LBP-2900 CAPT Status Monitor UI
 */
static pthread_mutex_t g_ui_spawn_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_last_ui_spawn_time = 0;

static void trigger_ui_popup(const char *reason) {
    (void)reason;
    if (access(g_ui_binary_path, X_OK) != 0)
        return;

    pthread_mutex_lock(&g_ui_spawn_lock);
    time_t now = time(NULL);
    if (now - g_last_ui_spawn_time < 3) {
        pthread_mutex_unlock(&g_ui_spawn_lock);
        return;
    }

    /* Check if lbp2900-ui is already running; if not, double-fork and spawn it in background */
    int rc = system("pgrep -x 'lbp2900-ui' >/dev/null 2>&1");
    if (rc != 0) {
        g_last_ui_spawn_time = now;
        pid_t pid = fork();
        if (pid == 0) {
            if (fork() == 0) {
                setsid();
                for (int cfd = 3; cfd < 1024; ++cfd)
                    close(cfd);
                if (!getenv("DISPLAY"))
                    setenv("DISPLAY", ":0", 1);
                if (!getenv("WAYLAND_DISPLAY"))
                    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
                if (!getenv("XDG_RUNTIME_DIR"))
                    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
                execl(g_ui_binary_path, "lbp2900-ui", "--status-monitor", NULL);
                _exit(0);
            }
            _exit(0);
        } else if (pid > 0) {
            waitpid(pid, NULL, 0);
        }
    }
    pthread_mutex_unlock(&g_ui_spawn_lock);
}

/*
 * Enqueue a print job atomically into the ephemeral FIFO spool (/tmp/lbp2900-spool/job-%u.dat).
 * Safe under high-concurrency job bursts (15–50+ simultaneous requests).
 */
static uint32_t enqueue_print_job(const char *name, const char *user,
                                  const char *filepath, bool move_file,
                                  const struct lbp2900_prefs *override_prefs) {
    mkdir(LBP2900_SPOOL_DIR, 0755);

    struct print_job_item *item = calloc(1, sizeof(*item));
    if (!item)
        return 0;

    pthread_mutex_lock(&g_queue_lock);
    uint32_t jid = g_next_job_id++;
    pthread_mutex_unlock(&g_queue_lock);

    char spool_path[256];
    snprintf(spool_path, sizeof(spool_path), "%s/job-%u.dat", LBP2900_SPOOL_DIR, jid);

    if (move_file) {
        if (rename(filepath, spool_path) != 0) {
            if (!copy_file_to_spool(filepath, spool_path)) {
                unlink(filepath);
                free(item);
                return 0;
            }
            unlink(filepath);
        }
    } else {
        if (!copy_file_to_spool(filepath, spool_path)) {
            free(item);
            return 0;
        }
    }

    pthread_mutex_lock(&g_queue_lock);
    item->job_id = jid;
    snprintf(item->job_name, sizeof(item->job_name), "%s", (name && name[0]) ? name : "Document");
    snprintf(item->job_user, sizeof(item->job_user), "%s", (user && user[0]) ? user : "phoenix");
    snprintf(item->filepath, sizeof(item->filepath), "%s", spool_path);
    item->delete_file_after = true;
    item->ipp_job_state = 3; /* IPP_JSTATE_PENDING */

    if (override_prefs) {
        item->job_prefs = *override_prefs;
    } else {
        pthread_mutex_lock(&g_prefs_lock);
        item->job_prefs = g_prefs;
        pthread_mutex_unlock(&g_prefs_lock);
    }

    if (!g_queue_tail) {
        g_queue_head = g_queue_tail = item;
    } else {
        g_queue_tail->next = item;
        g_queue_tail = item;
    }
    g_pending_jobs_count++;
    int cur_pending = g_pending_jobs_count;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_lock);

    bool is_conn = capt_usb_is_connected();
    capt_set_pending_jobs_count(cur_pending, item->job_name);

    if (!is_conn) {
        fprintf(stderr,
                "lbp2900d: [NO CONNECTION] Printer is disconnected! Job #%u '%s' held in pending FIFO queue (%d waiting)\n",
                jid, item->job_name, cur_pending);
        trigger_ui_popup("job-queued-offline");
    } else {
        fprintf(stderr, "lbp2900d: Enqueued Job #%u '%s' (%s, %d in FIFO)\n",
                jid, item->job_name, item->filepath, cur_pending);
        trigger_ui_popup("job-queued");
    }
    return jid;
}

static uint32_t enqueue_print_job_bytes(const char *name, const char *user,
                                        const uint8_t *data, size_t len,
                                        const struct lbp2900_prefs *override_prefs) {
    mkdir(LBP2900_SPOOL_DIR, 0755);

    pthread_mutex_lock(&g_queue_lock);
    uint32_t tmp_id = g_next_job_id;
    pthread_mutex_unlock(&g_queue_lock);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/incoming-%u-%lu.tmp",
             LBP2900_SPOOL_DIR, tmp_id, (unsigned long)pthread_self());
    FILE *sf = fopen(tmp_path, "wb");
    if (!sf)
        return 0;
    if (len > 0 && fwrite(data, 1, len, sf) != len) {
        fclose(sf);
        unlink(tmp_path);
        return 0;
    }
    fclose(sf);
    return enqueue_print_job(name, user, tmp_path, true, override_prefs);
}

/*
 * Serialized Worker Thread & Continuous USB Connection Monitor:
 *  - Every 750ms while idle or while waiting for reconnection, verifies the USB
 *    connection so power-off / cable unplug is detected in < 0.8s.
 *  - Holds pending jobs in strict FIFO order while disconnected.
 *  - Automatically dequeues and prints pending jobs sequentially in exact submission
 *    order as soon as the Canon LBP-2900 is plugged in / powered on and ready.
 */
static void *job_worker_thread(void *arg) {
    (void)arg;
    while (g_daemon_running) {
        pthread_mutex_lock(&g_queue_lock);
        if (!g_queue_head && g_daemon_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 750000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_queue_cond, &g_queue_lock, &ts);
        }
        if (!g_daemon_running) {
            pthread_mutex_unlock(&g_queue_lock);
            break;
        }
        int pending_now = g_pending_jobs_count;
        bool has_job = (g_queue_head != NULL);
        pthread_mutex_unlock(&g_queue_lock);

        /* Verify USB connection & CAPT handshake before dequeuing any job */
        bool usb_ready = capt_usb_verify_connection(pending_now);
        if (!has_job) {
            continue;
        }
        if (!usb_ready) {
            /* Printer is unplugged or powered off: keep all jobs in FIFO queue and wait */
            usleep(450000);
            continue;
        }

        /* Also wait if user paused the queue */
        struct daemon_live_status ls;
        capt_get_daemon_live_status(&ls);
        if (ls.paused) {
            usleep(300000);
            continue;
        }

        /* Printer is connected and ready: pop the oldest job from the FIFO head */
        pthread_mutex_lock(&g_queue_lock);
        struct print_job_item *job = g_queue_head;
        if (job) {
            g_queue_head = job->next;
            if (!g_queue_head)
                g_queue_tail = NULL;
            if (g_pending_jobs_count > 0)
                g_pending_jobs_count--;
            job->ipp_job_state = 5; /* IPP_JSTATE_PROCESSING */
        }
        int rem_pending = g_pending_jobs_count;
        pthread_mutex_unlock(&g_queue_lock);

        if (!job)
            continue;

        capt_set_pending_jobs_count(rem_pending, NULL);
        fprintf(stderr, "lbp2900d: [FIFO DEQUEUE] Starting Job #%u '%s' (%d remaining in FIFO)\n",
                job->job_id, job->job_name, rem_pending);

        bool job_ok = capt_execute_print_job(job->job_id, job->job_name, job->job_user,
                                             job->filepath, &job->job_prefs);

        struct daemon_live_status after_ls;
        capt_get_daemon_live_status(&after_ls);

        /* If interrupted by USB cable unplug / power-off (even while waiting on Out of Paper),
         * keep the job at the head of the FIFO queue so it automatically continues on reconnect! */
        if (!job_ok && !capt_usb_is_connected() && !after_ls.cancel_requested &&
            after_ls.state != PRN_STATE_ERROR) {
            if (after_ls.printed_pages > 0 && job->job_prefs.page_range[0] == '\0') {
                snprintf(job->job_prefs.page_range, sizeof(job->job_prefs.page_range),
                         "%d-99999", after_ls.printed_pages + 1);
            }
            pthread_mutex_lock(&g_queue_lock);
            job->ipp_job_state = 3; /* IPP_JSTATE_PENDING */
            job->next = g_queue_head;
            g_queue_head = job;
            if (!g_queue_tail)
                g_queue_tail = job;
            g_pending_jobs_count++;
            int new_pend = g_pending_jobs_count;
            pthread_mutex_unlock(&g_queue_lock);

            capt_set_pending_jobs_count(new_pend, job->job_name);
            fprintf(stderr, "lbp2900d: [RE-QUEUED ON DISCONNECT] Job #%u '%s' retained at head of FIFO queue (%d waiting) — will resume automatically on USB reconnect\n",
                    job->job_id, job->job_name, new_pend);
            continue;
        }

        if (job->delete_file_after && job->filepath[0]) {
            unlink(job->filepath);
        }
        free(job);
    }
    return NULL;
}

/*
 * Built-in Canon LASER SHOT LBP-2900 PPD for GTK / Qt / Chromium / LibreOffice Print Dialogs
 */
static const char *k_canon_lbp2900_ppd =
    "*PPD-Adobe: \"4.3\"\n"
    "*FormatVersion: \"4.3\"\n"
    "*FileVersion: \"2.0\"\n"
    "*LanguageVersion: English\n"
    "*LanguageEncoding: ISOLatin1\n"
    "*PCFileName: \"CNLB2K9.PPD\"\n"
    "*Manufacturer: \"Canon\"\n"
    "*Product: \"(LASER SHOT LBP-2900)\"\n"
    "*ModelName: \"Canon LASER SHOT LBP-2900 CAPT\"\n"
    "*ShortNickName: \"Canon LBP-2900\"\n"
    "*NickName: \"Canon LASER SHOT LBP-2900 CAPT Standalone (600 DPI)\"\n"
    "*PSVersion: \"(3010.000) 0\"\n"
    "*LanguageLevel: \"3\"\n"
    "*ColorDevice: False\n"
    "*DefaultColorSpace: Gray\n"
    "*FileSystem: False\n"
    "*Throughput: \"12\"\n"
    "*TTRasterizer: Type42\n"
    "*cupsVersion: 2.4\n"
    "*cupsModelNumber: 2900\n"
    "*cupsManualCopies: True\n"
    "*cupsFilter: \"application/pdf 0 -\"\n"
    "*cupsFilter: \"application/postscript 0 -\"\n"
    "*cupsFilter: \"application/vnd.cups-raster 0 -\"\n"
    "*OpenUI *PageSize/Page Size: PickOne\n"
    "*OrderDependency: 10 AnySetup *PageSize\n"
    "*DefaultPageSize: A4\n"
    "*PageSize A4/A4: \"<</PageSize[595 842]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize Letter/US Letter: \"<</PageSize[612 792]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize Legal/US Legal: \"<</PageSize[612 1008]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize Executive/Executive: \"<</PageSize[522 756]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize A5/A5: \"<</PageSize[420 595]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize B5/B5 (JIS): \"<</PageSize[516 729]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize EnvC5/Envelope C5: \"<</PageSize[459 649]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize EnvDL/Envelope DL: \"<</PageSize[312 624]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize EnvMonarch/Envelope Monarch: \"<</PageSize[279 540]/ImagingBBox null>>setpagedevice\"\n"
    "*PageSize Env10/Envelope #10: \"<</PageSize[297 684]/ImagingBBox null>>setpagedevice\"\n"
    "*CloseUI: *PageSize\n"
    "*OpenUI *PageRegion/Page Region: PickOne\n"
    "*OrderDependency: 10 AnySetup *PageRegion\n"
    "*DefaultPageRegion: A4\n"
    "*PageRegion A4/A4: \"<</PageSize[595 842]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion Letter/US Letter: \"<</PageSize[612 792]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion Legal/US Legal: \"<</PageSize[612 1008]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion Executive/Executive: \"<</PageSize[522 756]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion A5/A5: \"<</PageSize[420 595]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion B5/B5 (JIS): \"<</PageSize[516 729]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion EnvC5/Envelope C5: \"<</PageSize[459 649]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion EnvDL/Envelope DL: \"<</PageSize[312 624]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion EnvMonarch/Envelope Monarch: \"<</PageSize[279 540]/ImagingBBox null>>setpagedevice\"\n"
    "*PageRegion Env10/Envelope #10: \"<</PageSize[297 684]/ImagingBBox null>>setpagedevice\"\n"
    "*CloseUI: *PageRegion\n"
    "*DefaultImageableArea: A4\n"
    "*ImageableArea A4/A4: \"14.17 11.34 582.24 830.66\"\n"
    "*ImageableArea Letter/US Letter: \"14.17 11.34 599.24 780.66\"\n"
    "*ImageableArea Legal/US Legal: \"14.17 11.34 599.24 996.66\"\n"
    "*ImageableArea Executive/Executive: \"14.17 11.34 509.24 744.66\"\n"
    "*ImageableArea A5/A5: \"14.17 11.34 407.24 583.66\"\n"
    "*ImageableArea B5/B5 (JIS): \"14.17 11.34 503.24 717.66\"\n"
    "*ImageableArea EnvC5/Envelope C5: \"28.35 28.35 430.65 620.65\"\n"
    "*ImageableArea EnvDL/Envelope DL: \"28.35 28.35 283.65 595.65\"\n"
    "*ImageableArea EnvMonarch/Envelope Monarch: \"28.35 28.35 250.65 511.65\"\n"
    "*ImageableArea Env10/Envelope #10: \"28.35 28.35 268.65 655.65\"\n"
    "*DefaultPaperDimension: A4\n"
    "*PaperDimension A4/A4: \"595 842\"\n"
    "*PaperDimension Letter/US Letter: \"612 792\"\n"
    "*PaperDimension Legal/US Legal: \"612 1008\"\n"
    "*PaperDimension Executive/Executive: \"522 756\"\n"
    "*PaperDimension A5/A5: \"420 595\"\n"
    "*PaperDimension B5/B5 (JIS): \"516 729\"\n"
    "*PaperDimension EnvC5/Envelope C5: \"459 649\"\n"
    "*PaperDimension EnvDL/Envelope DL: \"312 624\"\n"
    "*PaperDimension EnvMonarch/Envelope Monarch: \"279 540\"\n"
    "*PaperDimension Env10/Envelope #10: \"297 684\"\n"
    "*OpenUI *MediaType/Paper Type: PickOne\n"
    "*OrderDependency: 10 AnySetup *MediaType\n"
    "*DefaultMediaType: Plain\n"
    "*MediaType Plain/Plain Paper: \"<</MediaType(Plain)/cupsMediaType 128>>setpagedevice\"\n"
    "*MediaType Heavy/Heavy Paper: \"<</MediaType(Heavy)/cupsMediaType 1>>setpagedevice\"\n"
    "*MediaType PlainL/Plain Paper L: \"<</MediaType(PlainL)/cupsMediaType 2>>setpagedevice\"\n"
    "*MediaType HeavyH/Heavy Paper H: \"<</MediaType(HeavyH)/cupsMediaType 3>>setpagedevice\"\n"
    "*MediaType Transparency/Transparency: \"<</MediaType(Transparency)/cupsMediaType 4>>setpagedevice\"\n"
    "*MediaType Envelope/Envelope: \"<</MediaType(Envelope)/cupsMediaType 5>>setpagedevice\"\n"
    "*CloseUI: *MediaType\n"
    "*OpenUI *Resolution/Resolution: PickOne\n"
    "*OrderDependency: 10 AnySetup *Resolution\n"
    "*DefaultResolution: 600dpi\n"
    "*Resolution 600dpi/600 DPI High Resolution: \"<</HWResolution[600 600]>>setpagedevice\"\n"
    "*CloseUI: *Resolution\n"
    "*OpenUI *captTonerSave/Toner Save: Boolean\n"
    "*DefaultcaptTonerSave: False\n"
    "*captTonerSave False/Disabled: \"\"\n"
    "*captTonerSave True/Enabled: \"\"\n"
    "*CloseUI: *captTonerSave\n"
    "*OpenUI *captDarkness/Toner Density: PickOne\n"
    "*DefaultcaptDarkness: 7\n"
    "*captDarkness 1/1 (Extra Light): \"\"\n"
    "*captDarkness 4/4 (Light): \"\"\n"
    "*captDarkness 7/7 (Normal): \"\"\n"
    "*captDarkness 11/11 (Dark): \"\"\n"
    "*captDarkness 15/15 (Extra Dark): \"\"\n"
    "*CloseUI: *captDarkness\n"
    "*DefaultFont: Courier\n";

/*
 * IPP memory buffer I/O callbacks for libcups ippReadIO / ippWriteIO
 */
struct mem_ipp_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    size_t   pos;
};

static ssize_t mem_ipp_read_cb(struct mem_ipp_buf *mb, ipp_uchar_t *buffer, size_t bytes) {
    if (mb->pos >= mb->len)
        return 0;
    size_t avail = mb->len - mb->pos;
    if (bytes > avail)
        bytes = avail;
    memcpy(buffer, mb->data + mb->pos, bytes);
    mb->pos += bytes;
    return (ssize_t)bytes;
}

static ssize_t mem_ipp_write_cb(struct mem_ipp_buf *mb, ipp_uchar_t *buffer, size_t bytes) {
    if (mb->len + bytes > mb->cap) {
        size_t new_cap = (mb->cap ? mb->cap * 2 : 4096) + bytes;
        uint8_t *nd = realloc(mb->data, new_cap);
        if (!nd)
            return -1;
        mb->data = nd;
        mb->cap = new_cap;
    }
    memcpy(mb->data + mb->len, buffer, bytes);
    mb->len += bytes;
    return (ssize_t)bytes;
}

static void populate_printer_attributes(ipp_t *resp) {
    struct daemon_live_status ls;
    capt_get_daemon_live_status(&ls);

    static const int supported_ops[] = {
        IPP_OP_PRINT_JOB,
        IPP_OP_VALIDATE_JOB,
        IPP_OP_CREATE_JOB,
        IPP_OP_SEND_DOCUMENT,
        IPP_OP_CANCEL_JOB,
        IPP_OP_GET_JOB_ATTRIBUTES,
        IPP_OP_GET_JOBS,
        IPP_OP_GET_PRINTER_ATTRIBUTES,
        IPP_OP_CUPS_GET_DEFAULT,
        IPP_OP_CUPS_GET_PRINTERS,
        IPP_OP_CUPS_GET_PPD,
    };

    static const char * const media_supported[] = {
        "iso_a4_210x297mm", "na_letter_8.5x11in", "na_legal_8.5x14in",
        "na_executive_7.25x10.5in", "iso_a5_148x210mm", "jis_b5_182x257mm",
        "iso_c5_162x229mm", "iso_dl_110x220mm", "na_monarch_3.875x7.5in",
        "na_number-10_4.125x9.5in", "A4", "Letter", "Legal", "Executive", "A5", "B5"
    };

    static const char * const doc_formats[] = {
        "application/pdf",
        "application/postscript",
        "application/vnd.cups-raster",
        "image/pwg-raster",
        "image/urf",
        "image/png",
        "image/jpeg",
        "text/plain",
        "application/octet-stream"
    };

    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", NULL,
                 "ipp://localhost/printers/Canon_LBP2900");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "uri-security-supported", NULL, "none");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "uri-authentication-supported", NULL, "none");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, "Canon_LBP2900");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", NULL, "Canon LASER SHOT LBP-2900");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-make-and-model", NULL,
                 "Canon LASER SHOT LBP-2900 CAPT Standalone");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-location", NULL, "USB Direct (04a9:2676)");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", NULL, "usb://Canon/LBP2900");

    int pstate = IPP_PSTATE_IDLE;
    const char *preason = "none";
    if (ls.state == PRN_STATE_PRINTING || ls.state == PRN_STATE_RENDERING) {
        pstate = IPP_PSTATE_PROCESSING;
    } else if (ls.state == PRN_STATE_OUT_OF_PAPER) {
        pstate = IPP_PSTATE_STOPPED;
        preason = "media-empty-error";
    } else if (ls.state == PRN_STATE_DISCONNECTED || !ls.usb_connected) {
        pstate = IPP_PSTATE_IDLE;
        preason = "offline-report";
    } else if (ls.state == PRN_STATE_PAUSED) {
        pstate = IPP_PSTATE_STOPPED;
        preason = "paused";
    }

    ippAddInteger(resp, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", pstate);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", NULL, preason);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-state-message", NULL, ls.headline);
    ippAddBoolean(resp, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
    ippAddBoolean(resp, IPP_TAG_PRINTER, "printer-is-shared", 0);
    ippAddInteger(resp, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "printer-type",
                  0x00000004 /* CUPS_PRINTER_BW */ | 0x00010000 /* CUPS_PRINTER_DEFAULT */);

    ippAddIntegers(resp, IPP_TAG_PRINTER, IPP_TAG_ENUM, "operations-supported",
                   (int)ARRAY_SIZE(supported_ops), supported_ops);
    ippAddStrings(resp, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-supported",
                  (int)ARRAY_SIZE(doc_formats), NULL, doc_formats);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_MIMETYPE, "document-format-default", NULL,
                 "application/pdf");
    ippAddStrings(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-supported",
                  (int)ARRAY_SIZE(media_supported), NULL, media_supported);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-default", NULL, "iso_a4_210x297mm");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "color-supported", NULL, "false");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-color-mode-default", NULL, "monochrome");
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "print-color-mode-supported", NULL, "monochrome");
    ippAddResolution(resp, IPP_TAG_PRINTER, "printer-resolution-default", IPP_RES_PER_INCH, 600, 600);
    ippAddResolution(resp, IPP_TAG_PRINTER, "printer-resolution-supported", IPP_RES_PER_INCH, 600, 600);
    ippAddRange(resp, IPP_TAG_PRINTER, "copies-supported", 1, 99);
    ippAddInteger(resp, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "copies-default", 1);
    ippAddString(resp, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "pdl-override-supported", NULL, "attempted");
}

static void extract_job_prefs_from_ipp(ipp_t *req, struct lbp2900_prefs *out_prefs) {
    pthread_mutex_lock(&g_prefs_lock);
    int auto_popup = g_prefs.auto_popup_ui;
    pthread_mutex_unlock(&g_prefs_lock);

    /* Listen directly to the printing application's request attributes */
    memset(out_prefs, 0, sizeof(*out_prefs));
    snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "A4");
    out_prefs->media_type = 0;
    out_prefs->copies = 1;
    out_prefs->collate = 1;
    out_prefs->orientation = 0;
    out_prefs->nup = 1;
    out_prefs->manual_duplex = 0;
    out_prefs->toner_save = 0;
    out_prefs->density = 8;
    out_prefs->dither_mode = 0;
    out_prefs->auto_popup_ui = auto_popup;

    ipp_attribute_t *attr;
    if ((attr = ippFindAttribute(req, "copies", IPP_TAG_INTEGER)) != NULL) {
        int c = ippGetInteger(attr, 0);
        if (c >= 1 && c <= 99)
            out_prefs->copies = c;
    }
    if ((attr = ippFindAttribute(req, "PageSize", IPP_TAG_ZERO)) != NULL ||
        (attr = ippFindAttribute(req, "media", IPP_TAG_ZERO)) != NULL) {
        const char *m = ippGetString(attr, 0, NULL);
        if (m) {
            if (strcasestr(m, "letter")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "Letter");
            else if (strcasestr(m, "legal")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "Legal");
            else if (strcasestr(m, "executive")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "Executive");
            else if (strcasestr(m, "a5")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "A5");
            else if (strcasestr(m, "b5")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "B5");
            else if (strcasestr(m, "c5")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "EnvC5");
            else if (strcasestr(m, "dl")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "EnvDL");
            else if (strcasestr(m, "monarch")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "EnvMonarch");
            else if (strcasestr(m, "10")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "Env10");
            else if (strcasestr(m, "a4")) snprintf(out_prefs->media_size, sizeof(out_prefs->media_size), "A4");
        }
    }
    if ((attr = ippFindAttribute(req, "MediaType", IPP_TAG_ZERO)) != NULL ||
        (attr = ippFindAttribute(req, "media-type", IPP_TAG_ZERO)) != NULL) {
        const char *mt = ippGetString(attr, 0, NULL);
        if (mt) {
            if (strcasestr(mt, "HeavyH")) out_prefs->media_type = 3;
            else if (strcasestr(mt, "Heavy")) out_prefs->media_type = 1;
            else if (strcasestr(mt, "PlainL")) out_prefs->media_type = 2;
            else if (strcasestr(mt, "Transparency")) out_prefs->media_type = 4;
            else if (strcasestr(mt, "Envelope")) out_prefs->media_type = 5;
            else out_prefs->media_type = 0;
        }
    }
    if ((attr = ippFindAttribute(req, "orientation-requested", IPP_TAG_ZERO)) != NULL) {
        int o = ippGetInteger(attr, 0);
        if (o == 4 || o == 5)
            out_prefs->orientation = 1;
    }
    if ((attr = ippFindAttribute(req, "sides", IPP_TAG_ZERO)) != NULL) {
        const char *s = ippGetString(attr, 0, NULL);
        if (s && strcasestr(s, "two-sided"))
            out_prefs->manual_duplex = 1;
    }
    if ((attr = ippFindAttribute(req, "captTonerSave", IPP_TAG_ZERO)) != NULL) {
        const char *v = ippGetString(attr, 0, NULL);
        if (v && (strcasecmp(v, "True") == 0 || strcmp(v, "1") == 0))
            out_prefs->toner_save = 1;
        else if (v && (strcasecmp(v, "False") == 0 || strcmp(v, "0") == 0))
            out_prefs->toner_save = 0;
    }
    if ((attr = ippFindAttribute(req, "captDarkness", IPP_TAG_ZERO)) != NULL) {
        const char *v = ippGetString(attr, 0, NULL);
        if (v && atoi(v) >= 1 && atoi(v) <= 15)
            out_prefs->density = atoi(v);
    }
}

static void handle_ipp_post(int fd, const uint8_t *body, size_t body_len) {
    struct mem_ipp_buf in_mb = { .data = (uint8_t *)body, .len = body_len, .cap = body_len, .pos = 0 };
    ipp_t *req = ippNew();
    ipp_state_t st = ippReadIO(&in_mb, (ipp_iocb_t)mem_ipp_read_cb, 1, NULL, req);
    if (st == IPP_STATE_ERROR) {
        ippDelete(req);
        const char *err_http = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        send(fd, err_http, strlen(err_http), MSG_NOSIGNAL);
        return;
    }

    ipp_op_t op = ippGetOperation(req);
    int req_id = ippGetRequestId(req);

    ipp_t *resp = ippNew();
    ippSetVersion(resp, 2, 0);
    ippSetStatusCode(resp, IPP_STATUS_OK);
    ippSetRequestId(resp, req_id);
    ippAddString(resp, IPP_TAG_OPERATION, IPP_TAG_CHARSET, "attributes-charset", NULL, "utf-8");
    ippAddString(resp, IPP_TAG_OPERATION, IPP_TAG_LANGUAGE, "attributes-natural-language", NULL, "en-us");

    const uint8_t *doc_bytes = body + in_mb.pos;
    size_t doc_len = (body_len > in_mb.pos) ? (body_len - in_mb.pos) : 0;

    if (op == IPP_OP_CUPS_GET_PRINTERS || op == IPP_OP_CUPS_GET_DEFAULT || op == IPP_OP_GET_PRINTER_ATTRIBUTES) {
        populate_printer_attributes(resp);
    } else if (op == IPP_OP_CUPS_GET_PPD) {
        doc_bytes = (const uint8_t *)k_canon_lbp2900_ppd;
        doc_len = strlen(k_canon_lbp2900_ppd);
    } else if (op == IPP_OP_VALIDATE_JOB) {
        /* Always valid */
    } else if (op == IPP_OP_CREATE_JOB) {
        pthread_mutex_lock(&g_queue_lock);
        uint32_t jid = g_next_job_id;
        g_last_created_job_id = jid;
        pthread_mutex_unlock(&g_queue_lock);

        char juri[128];
        snprintf(juri, sizeof(juri), "ipp://localhost/jobs/%u", jid);
        ippAddString(resp, IPP_TAG_JOB, IPP_TAG_URI, "job-uri", NULL, juri);
        ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-id", (int)jid);
        ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state", IPP_JSTATE_PENDING);
        ippAddString(resp, IPP_TAG_JOB, IPP_TAG_KEYWORD, "job-state-reasons", NULL, "none");
    } else if (op == IPP_OP_PRINT_JOB || op == IPP_OP_SEND_DOCUMENT) {
        ipp_attribute_t *jname_attr = ippFindAttribute(req, "job-name", IPP_TAG_ZERO);
        ipp_attribute_t *juser_attr = ippFindAttribute(req, "requesting-user-name", IPP_TAG_ZERO);
        const char *raw_jname = jname_attr ? ippGetString(jname_attr, 0, NULL) : "Application Print Job";
        const char *juser = juser_attr ? ippGetString(juser_attr, 0, NULL) : "phoenix";

        char clean_jname[128];
        snprintf(clean_jname, sizeof(clean_jname), "%s", raw_jname ? raw_jname : "Application Print Job");
        char *dash_empty = strstr(clean_jname, " \xE2\x80\x94 ()");
        if (!dash_empty)
            dash_empty = strstr(clean_jname, " - ()");
        if (dash_empty)
            *dash_empty = '\0';

        struct lbp2900_prefs jprefs;
        extract_job_prefs_from_ipp(req, &jprefs);

        uint32_t jid = 0;
        if (doc_len > 0) {
            jid = enqueue_print_job_bytes(clean_jname, juser, doc_bytes, doc_len, &jprefs);
        }
        if (jid == 0)
            jid = g_last_created_job_id ? g_last_created_job_id : 1;

        char juri[128];
        snprintf(juri, sizeof(juri), "ipp://localhost/jobs/%u", jid);
        ippAddString(resp, IPP_TAG_JOB, IPP_TAG_URI, "job-uri", NULL, juri);
        ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-id", (int)jid);
        bool conn = capt_usb_is_connected();
        ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state",
                      conn ? IPP_JSTATE_PROCESSING : IPP_JSTATE_PENDING);
        ippAddString(resp, IPP_TAG_JOB, IPP_TAG_KEYWORD, "job-state-reasons", NULL,
                     conn ? "job-printing" : "printer-stopped");
    } else if (op == IPP_OP_GET_JOB_ATTRIBUTES || op == IPP_OP_GET_JOBS) {
        struct daemon_live_status ls;
        capt_get_daemon_live_status(&ls);
        ipp_attribute_t *jid_attr = ippFindAttribute(req, "job-id", IPP_TAG_INTEGER);
        int jid = jid_attr ? ippGetInteger(jid_attr, 0) : (int)(ls.active_job_id ? ls.active_job_id : 1);

        if (op == IPP_OP_GET_JOB_ATTRIBUTES || ls.active_job_id != 0) {
            char juri[128];
            snprintf(juri, sizeof(juri), "ipp://localhost/jobs/%d", jid);
            ippAddString(resp, IPP_TAG_JOB, IPP_TAG_URI, "job-uri", NULL, juri);
            ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-id", jid);
            int jst = (ls.active_job_id == (uint32_t)jid) ? IPP_JSTATE_PROCESSING : IPP_JSTATE_COMPLETED;
            ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state", jst);
            ippAddString(resp, IPP_TAG_JOB, IPP_TAG_KEYWORD, "job-state-reasons", NULL,
                         (jst == IPP_JSTATE_PROCESSING) ? "job-printing" : "job-completed-successfully");
            ippAddInteger(resp, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-media-sheets-completed", ls.printed_pages);
        }
    } else if (op == IPP_OP_CANCEL_JOB) {
        capt_request_cancel();
    } else {
        populate_printer_attributes(resp);
    }

    struct mem_ipp_buf out_mb = {0};
    ippSetState(resp, IPP_STATE_IDLE);
    ippWriteIO(&out_mb, (ipp_iocb_t)mem_ipp_write_cb, 1, NULL, resp);

    size_t extra_len = (op == IPP_OP_CUPS_GET_PPD) ? strlen(k_canon_lbp2900_ppd) : 0;
    char http_hdr[256];
    int hlen = snprintf(http_hdr, sizeof(http_hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/ipp\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        out_mb.len + extra_len);
    send(fd, http_hdr, (size_t)hlen, MSG_NOSIGNAL);
    if (out_mb.len > 0)
        send(fd, out_mb.data, out_mb.len, MSG_NOSIGNAL);
    if (extra_len > 0)
        send(fd, k_canon_lbp2900_ppd, extra_len, MSG_NOSIGNAL);

    free(out_mb.data);
    ippDelete(resp);
    ippDelete(req);
}

/*
 * HTTP/1.1 + IPP connection handler (supports both Content-Length and Transfer-Encoding: chunked)
 */
static void *http_ipp_client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t *rx = malloc(65536);
    size_t rx_cap = 65536;
    size_t rx_len = 0;

    while (g_daemon_running) {
        char *hdr_end = NULL;
        while (g_daemon_running) {
            if (rx_len > 0) {
                rx[rx_len] = '\0';
                hdr_end = strstr((char *)rx, "\r\n\r\n");
                if (hdr_end)
                    break;
            }
            if (rx_len + 4096 >= rx_cap) {
                rx_cap *= 2;
                rx = realloc(rx, rx_cap);
            }
            ssize_t n = recv(fd, rx + rx_len, rx_cap - rx_len - 1, 0);
            if (n <= 0)
                goto done;
            rx_len += (size_t)n;
        }
        if (!hdr_end)
            break;

        size_t hdr_bytes = (size_t)(hdr_end - (char *)rx) + 4;
        char hdr_copy[4096];
        size_t cplen = (hdr_bytes < sizeof(hdr_copy) - 1) ? hdr_bytes : (sizeof(hdr_copy) - 1);
        memcpy(hdr_copy, rx, cplen);
        hdr_copy[cplen] = '\0';

        if (strcasestr(hdr_copy, "Expect: 100-continue")) {
            const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
            send(fd, cont, strlen(cont), MSG_NOSIGNAL);
        }

        if (strncasecmp(hdr_copy, "GET ", 4) == 0) {
            if (strcasestr(hdr_copy, ".ppd")) {
                size_t ppd_len = strlen(k_canon_lbp2900_ppd);
                char resp_hdr[256];
                int hlen = snprintf(resp_hdr, sizeof(resp_hdr),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/vnd.cups-ppd\r\n"
                                    "Content-Length: %zu\r\n\r\n", ppd_len);
                send(fd, resp_hdr, (size_t)hlen, MSG_NOSIGNAL);
                send(fd, k_canon_lbp2900_ppd, ppd_len, MSG_NOSIGNAL);
            } else {
                struct daemon_live_status ls;
                capt_get_daemon_live_status(&ls);
                char html[512];
                int blen = snprintf(html, sizeof(html),
                                    "<html><body><h2>Canon LASER SHOT LBP-2900 CAPT Server</h2>"
                                    "<p>Status: <b>%s</b> (%s)</p></body></html>",
                                    ls.headline, ls.detail);
                char resp_hdr[256];
                int hlen = snprintf(resp_hdr, sizeof(resp_hdr),
                                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", blen);
                send(fd, resp_hdr, (size_t)hlen, MSG_NOSIGNAL);
                send(fd, html, (size_t)blen, MSG_NOSIGNAL);
            }
            memmove(rx, rx + hdr_bytes, rx_len - hdr_bytes);
            rx_len -= hdr_bytes;
            continue;
        }

        bool is_chunked = (strcasestr(hdr_copy, "Transfer-Encoding: chunked") != NULL);
        size_t content_len = 0;
        const char *cl_ptr = strcasestr(hdr_copy, "Content-Length:");
        if (cl_ptr) {
            content_len = (size_t)strtoull(cl_ptr + 15, NULL, 10);
        }

        if (!is_chunked) {
            while (rx_len < hdr_bytes + content_len) {
                if (hdr_bytes + content_len + 4096 >= rx_cap) {
                    rx_cap = hdr_bytes + content_len + 65536;
                    rx = realloc(rx, rx_cap);
                }
                ssize_t n = recv(fd, rx + rx_len, rx_cap - rx_len - 1, 0);
                if (n <= 0)
                    break;
                rx_len += (size_t)n;
            }
            size_t actual_body = (rx_len >= hdr_bytes + content_len) ? content_len :
                                 ((rx_len > hdr_bytes) ? (rx_len - hdr_bytes) : 0);
            handle_ipp_post(fd, rx + hdr_bytes, actual_body);
            size_t consumed = hdr_bytes + actual_body;
            if (rx_len >= consumed) {
                memmove(rx, rx + consumed, rx_len - consumed);
                rx_len -= consumed;
            } else {
                rx_len = 0;
            }
        } else {
            uint8_t *unchunked = malloc(65536);
            size_t uc_cap = 65536, uc_len = 0;
            size_t parse_pos = hdr_bytes;

            while (g_daemon_running) {
                while (1) {
                    rx[rx_len] = '\0';
                    char *line_end = strstr((char *)rx + parse_pos, "\r\n");
                    if (line_end)
                        break;
                    if (rx_len + 8192 >= rx_cap) {
                        rx_cap *= 2;
                        rx = realloc(rx, rx_cap);
                    }
                    ssize_t n = recv(fd, rx + rx_len, rx_cap - rx_len - 1, 0);
                    if (n <= 0)
                        goto chunk_done;
                    rx_len += (size_t)n;
                }
                char *line_end = strstr((char *)rx + parse_pos, "\r\n");
                size_t chunk_sz = (size_t)strtoull((char *)rx + parse_pos, NULL, 16);
                parse_pos = (size_t)(line_end - (char *)rx) + 2;

                if (chunk_sz == 0) {
                    while (rx_len < parse_pos + 2) {
                        ssize_t n = recv(fd, rx + rx_len, rx_cap - rx_len - 1, 0);
                        if (n <= 0) break;
                        rx_len += (size_t)n;
                    }
                    if (rx_len >= parse_pos + 2)
                        parse_pos += 2;
                    break;
                }

                while (rx_len < parse_pos + chunk_sz + 2) {
                    if (parse_pos + chunk_sz + 16384 >= rx_cap) {
                        rx_cap = parse_pos + chunk_sz + 65536;
                        rx = realloc(rx, rx_cap);
                    }
                    ssize_t n = recv(fd, rx + rx_len, rx_cap - rx_len - 1, 0);
                    if (n <= 0)
                        goto chunk_done;
                    rx_len += (size_t)n;
                }
                if (uc_len + chunk_sz >= uc_cap) {
                    uc_cap = (uc_cap * 2) + chunk_sz;
                    unchunked = realloc(unchunked, uc_cap);
                }
                memcpy(unchunked + uc_len, rx + parse_pos, chunk_sz);
                uc_len += chunk_sz;
                parse_pos += chunk_sz + 2;
            }
        chunk_done:
            handle_ipp_post(fd, unchunked, uc_len);
            free(unchunked);
            if (rx_len > parse_pos) {
                memmove(rx, rx + parse_pos, rx_len - parse_pos);
                rx_len -= parse_pos;
            } else {
                rx_len = 0;
            }
        }
    }

done:
    free(rx);
    close(fd);
    return NULL;
}

/*
 * Fast Control Socket Handler (/tmp/lbp2900-control.sock) for UI and CLI
 */
static void *ctrl_client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(fd);
        return NULL;
    }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';

    char reply[4096];
    reply[0] = '\0';

    if (strcmp(buf, "STATUS") == 0) {
        struct daemon_live_status ls;
        capt_get_daemon_live_status(&ls);
        pthread_mutex_lock(&g_queue_lock);
        int pend = g_pending_jobs_count;
        pthread_mutex_unlock(&g_queue_lock);
        snprintf(reply, sizeof(reply),
                 "state=%d\n"
                 "usb_connected=%d\n"
                 "headline=%s\n"
                 "detail=%s\n"
                 "active_job_id=%u\n"
                 "active_job_name=%s\n"
                 "active_job_user=%s\n"
                 "current_page=%d\n"
                 "total_pages=%d\n"
                 "printed_pages=%d\n"
                 "buf_slots_free=%d\n"
                 "streaming_mode=%d\n"
                 "paper_out_latched=%d\n"
                 "paused=%d\n"
                 "pending_jobs=%d\n"
                 "total_jobs_completed=%u\n"
                 "total_pages_lifetime=%u\n"
                 "daemon_rss_kb=%zu\n"
                 "capt_bas=0x%04x\n"
                 "capt_cnt=0x%02x\n"
                 "capt_pap=0x%02x\n"
                 "capt_eng=0x%04x\n"
                 "capt_led=0x%04x\n",
                 (int)ls.state, (int)ls.usb_connected, ls.headline, ls.detail,
                 ls.active_job_id, ls.active_job_name, ls.active_job_user,
                 ls.current_page, ls.total_pages, ls.printed_pages,
                 ls.buf_slots_free, (int)ls.streaming_mode, (int)ls.paper_out_latched,
                 (int)ls.paused, pend, ls.total_jobs_completed, ls.total_pages_lifetime,
                 ls.daemon_rss_kb,
                 ls.raw_capt.status[0], ls.raw_capt.xstat_cnt, ls.raw_capt.xstat_pap, ls.raw_capt.xstat_eng,
                 ls.raw_capt.status[4]);
    } else if (strcmp(buf, "QUEUE") == 0) {
        pthread_mutex_lock(&g_queue_lock);
        int off = snprintf(reply, sizeof(reply), "pending_jobs=%d\n", g_pending_jobs_count);
        for (struct print_job_item *it = g_queue_head; it && off < (int)sizeof(reply) - 128; it = it->next) {
            off += snprintf(reply + off, sizeof(reply) - (size_t)off,
                            "job=%u|%s|%s\n", it->job_id, it->job_name, it->filepath);
        }
        pthread_mutex_unlock(&g_queue_lock);
    } else if (strcmp(buf, "PURGE_QUEUE") == 0) {
        purge_spool_directory_and_queue();
        snprintf(reply, sizeof(reply), "OK purged=1\n");
    } else if (strncmp(buf, "SIMULATE_DISCONNECT ", 20) == 0) {
        bool disc = (atoi(buf + 20) != 0);
        capt_test_set_simulated_disconnect(disc);
        pthread_mutex_lock(&g_queue_lock);
        int pend = g_pending_jobs_count;
        pthread_cond_signal(&g_queue_cond);
        pthread_mutex_unlock(&g_queue_lock);
        if (!disc)
            capt_usb_verify_connection(pend);
        snprintf(reply, sizeof(reply), "OK simulated_disconnect=%d\n", (int)disc);
    } else if (strncmp(buf, "SIMULATE_FAULT ", 15) == 0) {
        const char *ft = buf + 15;
        uint16_t mask = 0;
        if (strcasecmp(ft, "jam") == 0) mask = CAPT_ENG_JAM;
        else if (strcasecmp(ft, "door") == 0 || strcasecmp(ft, "cover") == 0) mask = CAPT_ENG_DOOR_OPEN;
        else if (strcasecmp(ft, "cartridge") == 0) mask = CAPT_ENG_NO_CARTRIDGE;
        capt_test_set_simulated_eng_fault(mask);
        snprintf(reply, sizeof(reply), "OK simulated_eng=0x%04x\n", mask);
    } else if (strcmp(buf, "POLL_USB") == 0) {
        struct capt_status_s cs;
        bool ok = capt_usb_poll_live_status(&cs);
        snprintf(reply, sizeof(reply), "ok=%d\nbuf_slots=%d\npap=0x%02x\n",
                 (int)ok, CAPT_BUFLEVEL_SLOTS(cs.buf_level), cs.xstat_pap);
    } else if (strcmp(buf, "GET_PREFS") == 0) {
        pthread_mutex_lock(&g_prefs_lock);
        snprintf(reply, sizeof(reply),
                 "media_size=%s\n"
                 "media_type=%d\n"
                 "copies=%d\n"
                 "collate=%d\n"
                 "orientation=%d\n"
                 "nup=%d\n"
                 "manual_duplex=%d\n"
                 "toner_save=%d\n"
                 "density=%d\n"
                 "dither_mode=%d\n"
                 "auto_popup_ui=%d\n"
                 "page_range=%s\n",
                 g_prefs.media_size, g_prefs.media_type, g_prefs.copies,
                 g_prefs.collate, g_prefs.orientation, g_prefs.nup,
                 g_prefs.manual_duplex, g_prefs.toner_save, g_prefs.density,
                 g_prefs.dither_mode, g_prefs.auto_popup_ui, g_prefs.page_range);
        pthread_mutex_unlock(&g_prefs_lock);
    } else if (strncmp(buf, "SET_PREF ", 9) == 0) {
        char *kv = buf + 9;
        char *eq = strchr(kv, '=');
        if (eq) {
            *eq = '\0';
            const char *k = kv;
            const char *v = eq + 1;
            pthread_mutex_lock(&g_prefs_lock);
            if (strcmp(k, "media_size") == 0) snprintf(g_prefs.media_size, sizeof(g_prefs.media_size), "%s", v);
            else if (strcmp(k, "media_type") == 0) g_prefs.media_type = atoi(v);
            else if (strcmp(k, "copies") == 0) g_prefs.copies = atoi(v);
            else if (strcmp(k, "collate") == 0) g_prefs.collate = atoi(v);
            else if (strcmp(k, "orientation") == 0) g_prefs.orientation = atoi(v);
            else if (strcmp(k, "nup") == 0) g_prefs.nup = atoi(v);
            else if (strcmp(k, "manual_duplex") == 0) g_prefs.manual_duplex = atoi(v);
            else if (strcmp(k, "toner_save") == 0) g_prefs.toner_save = atoi(v);
            else if (strcmp(k, "density") == 0) g_prefs.density = atoi(v);
            else if (strcmp(k, "dither_mode") == 0) g_prefs.dither_mode = atoi(v);
            else if (strcmp(k, "auto_popup_ui") == 0) g_prefs.auto_popup_ui = atoi(v);
            else if (strcmp(k, "page_range") == 0) snprintf(g_prefs.page_range, sizeof(g_prefs.page_range), "%s", v);
            pthread_mutex_unlock(&g_prefs_lock);
            save_prefs();
            snprintf(reply, sizeof(reply), "OK\n");
        }
    } else if (strncmp(buf, "PRINT ", 6) == 0) {
        char *arg_str = buf + 6;
        char *sep = strchr(arg_str, '|');
        const char *title = "Document";
        if (sep) {
            *sep = '\0';
            title = sep + 1;
        }
        bool conn = capt_usb_is_connected();
        uint32_t jid = enqueue_print_job(title, "phoenix", arg_str, false, NULL);
        pthread_mutex_lock(&g_queue_lock);
        int pend = g_pending_jobs_count;
        pthread_mutex_unlock(&g_queue_lock);
        if (jid == 0) {
            snprintf(reply, sizeof(reply), "ERR failed to spool file\n");
        } else if (!conn) {
            snprintf(reply, sizeof(reply),
                     "OK job_id=%u usb_connected=0 pending_jobs=%d warning=No_Connection_Held_In_FIFO_Queue\n",
                     jid, pend);
        } else {
            snprintf(reply, sizeof(reply), "OK job_id=%u usb_connected=1 pending_jobs=%d\n", jid, pend);
        }
    } else if (strncmp(buf, "TEST_PAGE", 9) == 0) {
        int npages = 1;
        if (buf[9] == ' ')
            npages = atoi(buf + 10);
        if (npages < 1)
            npages = 1;
        mkdir(LBP2900_SPOOL_DIR, 0755);
        pthread_mutex_lock(&g_queue_lock);
        uint32_t seq_id = g_next_job_id;
        pthread_mutex_unlock(&g_queue_lock);

        char ps_path[256];
        snprintf(ps_path, sizeof(ps_path), "%s/test-%u-%lu.ps",
                 LBP2900_SPOOL_DIR, seq_id, (unsigned long)pthread_self());
        struct lbp2900_prefs cur_p;
        pthread_mutex_lock(&g_prefs_lock);
        cur_p = g_prefs;
        pthread_mutex_unlock(&g_prefs_lock);
        cur_p.copies = 1;
        cur_p.page_range[0] = '\0';
        generate_test_document_ps(ps_path, npages, &cur_p);
        char title[64];
        snprintf(title, sizeof(title), "Canon LBP-2900 Test Page (%d page%s)", npages, npages == 1 ? "" : "s");
        bool conn = capt_usb_is_connected();
        uint32_t jid = enqueue_print_job(title, "phoenix", ps_path, true, &cur_p);
        pthread_mutex_lock(&g_queue_lock);
        int pend = g_pending_jobs_count;
        pthread_mutex_unlock(&g_queue_lock);
        snprintf(reply, sizeof(reply), "OK job_id=%u usb_connected=%d pending_jobs=%d\n",
                 jid, (int)conn, pend);
    } else if (strncmp(buf, "PAUSE ", 6) == 0) {
        capt_request_pause(atoi(buf + 6) != 0);
        snprintf(reply, sizeof(reply), "OK\n");
    } else if (strcmp(buf, "CANCEL") == 0) {
        capt_request_cancel();
        snprintf(reply, sizeof(reply), "OK\n");
    } else if (strcmp(buf, "RESUME_PAPER") == 0) {
        capt_request_soft_resume();
        snprintf(reply, sizeof(reply), "OK\n");
    } else if (strcmp(buf, "CLEAN") == 0) {
        capt_execute_cleaning();
        snprintf(reply, sizeof(reply), "OK\n");
    } else {
        snprintf(reply, sizeof(reply), "ERR unknown command\n");
    }

    if (reply[0])
        send(fd, reply, strlen(reply), MSG_NOSIGNAL);
    close(fd);
    return NULL;
}

static int create_unix_listener(const char *path) {
    unlink(path);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    chmod(path, 0666);
    if (listen(s, 128) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static int create_tcp_listener(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    if (listen(s, 128) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static void configure_client_conf(const char *sock_path) {
    const char *home = getenv("HOME");
    if (!home)
        home = "/home/phoenix";
    char dir[256], conf[300];
    snprintf(dir, sizeof(dir), "%s/.cups", home);
    mkdir(dir, 0755);
    snprintf(conf, sizeof(conf), "%s/client.conf", dir);
    FILE *f = fopen(conf, "w");
    if (f) {
        fprintf(f, "ServerName %s\n", sock_path);
        fclose(f);
    }
}

static void handle_sig(int sig) {
    (void)sig;
    g_daemon_running = 0;
    pthread_cond_broadcast(&g_queue_cond);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ui-bin") == 0 && i + 1 < argc) {
            snprintf(g_ui_binary_path, sizeof(g_ui_binary_path), "%s", argv[++i]);
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGHUP, handle_sig);

    /* Group 6.3: Purge any stale spool files from previous session/boot */
    purge_spool_directory_and_queue();

    load_prefs();
    save_prefs();
    capt_set_ui_popup_hook(trigger_ui_popup);
    capt_usb_init_persistent();

    mkdir("/run/cups", 0777);
    int sock_cups = create_unix_listener(LBP2900_CUPS_SOCK);
    int sock_alt  = create_unix_listener(LBP2900_ALT_SOCK);
    int sock_ctrl = create_unix_listener(LBP2900_CTRL_SOCK);
    int sock_tcp  = create_tcp_listener(LBP2900_IPP_PORT);

    const char *active_ipp_sock = (sock_cups >= 0) ? LBP2900_CUPS_SOCK : LBP2900_ALT_SOCK;
    configure_client_conf(active_ipp_sock);

    fprintf(stderr, "lbp2900d: Standalone CAPT Print Server started (PID %d)\n", (int)getpid());
    fprintf(stderr, "lbp2900d:   IPP Socket : %s (fallback %s)\n",
            (sock_cups >= 0) ? LBP2900_CUPS_SOCK : "N/A", LBP2900_ALT_SOCK);
    fprintf(stderr, "lbp2900d:   IPP TCP    : 127.0.0.1:%d\n", LBP2900_IPP_PORT);
    fprintf(stderr, "lbp2900d:   Ctrl Socket: %s\n", LBP2900_CTRL_SOCK);

    pthread_t worker_tid;
    pthread_create(&worker_tid, NULL, job_worker_thread, NULL);

    while (g_daemon_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (sock_cups >= 0) { FD_SET(sock_cups, &rfds); if (sock_cups > maxfd) maxfd = sock_cups; }
        if (sock_alt >= 0)  { FD_SET(sock_alt, &rfds);  if (sock_alt > maxfd)  maxfd = sock_alt; }
        if (sock_ctrl >= 0) { FD_SET(sock_ctrl, &rfds); if (sock_ctrl > maxfd) maxfd = sock_ctrl; }
        if (sock_tcp >= 0)  { FD_SET(sock_tcp, &rfds);  if (sock_tcp > maxfd)  maxfd = sock_tcp; }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            continue;

        int ipp_listeners[3] = { sock_cups, sock_alt, sock_tcp };
        for (int i = 0; i < 3; ++i) {
            int ls = ipp_listeners[i];
            if (ls >= 0 && FD_ISSET(ls, &rfds)) {
                int cfd = accept(ls, NULL, NULL);
                if (cfd >= 0) {
                    pthread_t t;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    pthread_create(&t, &attr, http_ipp_client_thread, (void *)(intptr_t)cfd);
                    pthread_attr_destroy(&attr);
                }
            }
        }

        if (sock_ctrl >= 0 && FD_ISSET(sock_ctrl, &rfds)) {
            int cfd = accept(sock_ctrl, NULL, NULL);
            if (cfd >= 0) {
                pthread_t t;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                pthread_create(&t, &attr, ctrl_client_thread, (void *)(intptr_t)cfd);
                pthread_attr_destroy(&attr);
            }
        }
    }

    if (sock_cups >= 0) { close(sock_cups); unlink(LBP2900_CUPS_SOCK); }
    if (sock_alt >= 0)  { close(sock_alt);  unlink(LBP2900_ALT_SOCK); }
    if (sock_ctrl >= 0) { close(sock_ctrl); unlink(LBP2900_CTRL_SOCK); }
    if (sock_tcp >= 0)  { close(sock_tcp); }

    /* Group 6.3: Ephemeral spool cleanup on daemon shutdown / logout / reboot */
    purge_spool_directory_and_queue();
    capt_usb_shutdown();
    return 0;
}
