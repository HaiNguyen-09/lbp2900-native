/*
 * Persistent USB Driver & CAPT v1.0 State Machine for Canon LASER SHOT LBP-2900
 *
 * Keeps ONE continuous USB session open across all jobs to eliminate the
 * 5th-job ReserveUnit (0x8c) hardware engine wedge, while streaming 600 DPI
 * Hi-SCoA compressed bands with hardware BufLevel (0..15) flow control.
 *
 * Hardened against:
 *  - Group 1: Sudden USB Cable Disconnect & Power Cut mid-transfer (0% CPU spin, instant abort & interface release)
 *  - Group 2: Paper Jam (CAPT_ENG_JAM) & Cover Open (CAPT_ENG_DOOR_OPEN) auto-recovery without duplicate pages
 *  - Group 3: 100% solid black / high-entropy oversized bands & invalid paper dimension reporting
 *  - Group 4: Mid-flight cancel with clean fuser sheet ejection
 *  - Group 6: Immediate <1s disconnect detection & ready handshake verification for FIFO queue replay
 */

#define _GNU_SOURCE
#include "capt_usb.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* Minimal libusb-1.0 ABI declarations (linked against /lib/x86_64-linux-gnu/libusb-1.0.so.0) */
typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_endpoint_descriptor {
    uint8_t  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval, bRefresh, bSynchAddress;
    const unsigned char *extra;
    int extra_length;
};
struct libusb_interface_descriptor {
    uint8_t  bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t  bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
    const struct libusb_endpoint_descriptor *endpoint;
    const unsigned char *extra;
    int extra_length;
};
struct libusb_interface {
    const struct libusb_interface_descriptor *altsetting;
    int num_altsetting;
};
struct libusb_config_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const struct libusb_interface *interface;
    const unsigned char *extra;
    int extra_length;
};

extern int libusb_init(libusb_context **ctx);
extern void libusb_exit(libusb_context *ctx);
extern libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t vendor_id, uint16_t product_id);
extern libusb_device *libusb_get_device(libusb_device_handle *dev_handle);
extern int libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **config);
extern void libusb_free_config_descriptor(struct libusb_config_descriptor *config);
extern int libusb_kernel_driver_active(libusb_device_handle *dev_handle, int interface_number);
extern int libusb_detach_kernel_driver(libusb_device_handle *dev_handle, int interface_number);
extern int libusb_claim_interface(libusb_device_handle *dev_handle, int interface_number);
extern int libusb_release_interface(libusb_device_handle *dev_handle, int interface_number);
extern void libusb_close(libusb_device_handle *dev_handle);
extern int libusb_bulk_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length, int *actual_length, unsigned int timeout);
extern const char *libusb_error_name(int errcode);

#define CAPT_RESYNC_MAX_BYTES 256
#define CAPT_WAIT_READY_TIMEOUT_MS 30000
#define CAPT_BUFLEVEL_DRAIN_TIMEOUT_MS 60000

static pthread_mutex_t g_usb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;

static libusb_context *g_usb_ctx = NULL;
static libusb_device_handle *g_devh = NULL;
static bool g_usb_io_failed = false;
static bool g_sim_disconnect = false;
static uint16_t g_sim_eng_fault = 0;
static uint8_t g_ep_out = 0x01;
static uint8_t g_ep_in  = 0x82;

static uint8_t g_iobuf[0x10000];
static size_t  g_iosize = 0;
static struct capt_status_s g_capt_status;
static uint64_t g_completed_msg_until_ms = 0;

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static struct daemon_live_status g_live = {
    .state = PRN_STATE_DISCONNECTED,
    .usb_connected = false,
    .headline = "No Connection / Disconnected",
    .detail = "Connecting to Canon LASER SHOT LBP-2900 (USB 04a9:2676)...",
    .buf_slots_free = 15,
};

static ui_popup_hook_fn g_popup_hook = NULL;

void capt_set_ui_popup_hook(ui_popup_hook_fn fn) {
    g_popup_hook = fn;
}

static size_t get_current_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    unsigned long size = 0, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) != 2)
        resident = 0;
    fclose(f);
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    return (size_t)(resident * (unsigned long)(page_kb > 0 ? page_kb : 4));
}

static void update_live_status(enum printer_ui_state st, const char *headline, const char *detail) {
    pthread_mutex_lock(&g_status_lock);
    g_completed_msg_until_ms = 0;
    g_live.state = st;
    g_live.usb_connected = (g_devh != NULL && !g_usb_io_failed);
    if (headline)
        snprintf(g_live.headline, sizeof(g_live.headline), "%s", headline);
    if (detail)
        snprintf(g_live.detail, sizeof(g_live.detail), "%s", detail);
    g_live.raw_capt = g_capt_status;
    g_live.buf_slots_free = CAPT_BUFLEVEL_SLOTS(g_capt_status.buf_level);
    g_live.daemon_rss_kb = get_current_rss_kb();
    pthread_mutex_unlock(&g_status_lock);
}

void capt_set_pending_jobs_count(int pending_jobs, const char *latest_job_name) {
    pthread_mutex_lock(&g_status_lock);
    g_live.pending_jobs = pending_jobs;
    if (!g_live.usb_connected || g_live.state == PRN_STATE_DISCONNECTED) {
        g_live.state = PRN_STATE_DISCONNECTED;
        if (pending_jobs > 0) {
            snprintf(g_live.headline, sizeof(g_live.headline),
                     "No Connection / Disconnected (%d Job%s Queued)",
                     pending_jobs, pending_jobs == 1 ? "" : "s");
            snprintf(g_live.detail, sizeof(g_live.detail),
                     "No connection available — '%s' held in pending FIFO queue (%d waiting). Connect or power on LBP-2900 to print.",
                     (latest_job_name && latest_job_name[0]) ? latest_job_name : "Document",
                     pending_jobs);
        } else {
            snprintf(g_live.headline, sizeof(g_live.headline),
                     "No Connection / Disconnected");
            snprintf(g_live.detail, sizeof(g_live.detail),
                     "Canon LASER SHOT LBP-2900 is powered off or unplugged from USB.");
        }
    }
    pthread_mutex_unlock(&g_status_lock);
}

void capt_get_daemon_live_status(struct daemon_live_status *out) {
    pthread_mutex_lock(&g_status_lock);
    if (g_completed_msg_until_ms > 0 && get_monotonic_ms() >= g_completed_msg_until_ms) {
        g_completed_msg_until_ms = 0;
        if (g_live.state == PRN_STATE_READY && g_live.active_job_id == 0) {
            snprintf(g_live.headline, sizeof(g_live.headline), "Ready to Print");
            snprintf(g_live.detail, sizeof(g_live.detail), "Printer is ready");
        }
    }
    g_live.daemon_rss_kb = get_current_rss_kb();
    *out = g_live;
    pthread_mutex_unlock(&g_status_lock);
}

void capt_request_pause(bool pause) {
    pthread_mutex_lock(&g_status_lock);
    g_live.paused = pause;
    if (pause && g_live.state == PRN_STATE_READY) {
        g_live.state = PRN_STATE_PAUSED;
        snprintf(g_live.headline, sizeof(g_live.headline), "Paused");
        snprintf(g_live.detail, sizeof(g_live.detail), "Print queue is paused by user.");
    } else if (!pause && g_live.state == PRN_STATE_PAUSED) {
        g_live.state = g_live.usb_connected ? PRN_STATE_READY : PRN_STATE_DISCONNECTED;
        snprintf(g_live.headline, sizeof(g_live.headline),
                 g_live.usb_connected ? "Ready to Print" : "No Connection / Disconnected");
        snprintf(g_live.detail, sizeof(g_live.detail),
                 g_live.usb_connected ? "Printer is ready"
                                      : "Canon LASER SHOT LBP-2900 is powered off or unplugged.");
    }
    pthread_mutex_unlock(&g_status_lock);
}

void capt_request_cancel(void) {
    pthread_mutex_lock(&g_status_lock);
    g_live.cancel_requested = true;
    pthread_mutex_unlock(&g_status_lock);
}

void capt_request_soft_resume(void) {
    pthread_mutex_lock(&g_status_lock);
    g_live.soft_resume_requested = true;
    pthread_mutex_unlock(&g_status_lock);
}

static void usb_force_close_locked(const char *reason);

void capt_test_set_simulated_disconnect(bool sim_disc) {
    pthread_mutex_lock(&g_usb_lock);
    g_sim_disconnect = sim_disc;
    if (sim_disc) {
        usb_force_close_locked("USB cable unplugged / printer powered off");
    }
    pthread_mutex_unlock(&g_usb_lock);
}

void capt_test_set_simulated_eng_fault(uint16_t sim_eng_mask) {
    pthread_mutex_lock(&g_status_lock);
    g_sim_eng_fault = sim_eng_mask;
    g_capt_status.xstat_eng = (g_capt_status.xstat_eng & ~(CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN | CAPT_ENG_NO_CARTRIDGE)) | sim_eng_mask;
    g_live.raw_capt = g_capt_status;
    pthread_mutex_unlock(&g_status_lock);
}

/*
 * Immediately release and close the USB handle on cable disconnect or power-off
 * so the kernel USB interface is NEVER locked with LIBUSB_ERROR_BUSY.
 */
static void usb_force_close_locked(const char *reason) {
    g_usb_io_failed = true;
    if (g_devh) {
        fprintf(stderr, "lbp2900d: [DISCONNECTED] No connection / Disconnected (%s) — releasing USB interface\n",
                reason ? reason : "USB I/O error");
        libusb_release_interface(g_devh, 0);
        libusb_close(g_devh);
        g_devh = NULL;
    }

    pthread_mutex_lock(&g_status_lock);
    int pending = g_live.pending_jobs;
    uint32_t active_job = g_live.active_job_id;
    int total_queued = pending + (active_job > 0 ? 1 : 0);
    g_live.usb_connected = false;
    g_live.paper_out_latched = false;
    g_live.state = PRN_STATE_DISCONNECTED;
    if (total_queued > 0) {
        snprintf(g_live.headline, sizeof(g_live.headline),
                 "No Connection / Disconnected (%d Job%s Queued)",
                 total_queued, total_queued == 1 ? "" : "s");
        snprintf(g_live.detail, sizeof(g_live.detail),
                 "Printer disconnected (%s). %d pending job(s) held in FIFO queue waiting for reconnection.",
                 reason ? reason : "USB offline", total_queued);
    } else {
        snprintf(g_live.headline, sizeof(g_live.headline),
                 "No Connection / Disconnected");
        snprintf(g_live.detail, sizeof(g_live.detail),
                 "Canon LASER SHOT LBP-2900 is powered off or unplugged from USB.");
    }
    pthread_mutex_unlock(&g_status_lock);

    /* Only popup Status Monitor on disconnect if there is an active or pending print job */
    if (g_popup_hook && total_queued > 0)
        g_popup_hook("disconnected");
}

/* Drain any unread bytes queued on the printer's USB IN endpoint */
static void usb_drain_stale_locked(void) {
    if (!g_devh)
        return;
    for (int rounds = 0; rounds < 50; ++rounds) {
        uint8_t junk[256];
        int actual = 0;
        int r = libusb_bulk_transfer(g_devh, g_ep_in, junk, sizeof(junk), &actual, 150);
        if (r != 0 || actual == 0)
            break;
        fprintf(stderr, "lbp2900d: drained %d stale USB IN byte(s)\n", actual);
    }
}

static bool usb_try_open_locked(void) {
    if (g_sim_disconnect)
        return false;
    if (g_devh && !g_usb_io_failed)
        return true;
    if (g_devh) {
        libusb_release_interface(g_devh, 0);
        libusb_close(g_devh);
        g_devh = NULL;
    }
    if (!g_usb_ctx) {
        if (libusb_init(&g_usb_ctx) != 0)
            return false;
    }
    g_devh = libusb_open_device_with_vid_pid(g_usb_ctx, LBP2900_VENDOR_ID, LBP2900_PRODUCT_ID);
    if (!g_devh)
        return false;

    libusb_device *dev = libusb_get_device(g_devh);
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
        for (int i = 0; i < cfg->bNumInterfaces; i++) {
            for (int j = 0; j < cfg->interface[i].num_altsetting; j++) {
                const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[j];
                for (int k = 0; k < id->bNumEndpoints; k++) {
                    const struct libusb_endpoint_descriptor *ep = &id->endpoint[k];
                    if ((ep->bmAttributes & 0x03) == 2) {
                        if (ep->bEndpointAddress & 0x80)
                            g_ep_in = ep->bEndpointAddress;
                        else
                            g_ep_out = ep->bEndpointAddress;
                    }
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }

    if (libusb_kernel_driver_active(g_devh, 0) == 1)
        libusb_detach_kernel_driver(g_devh, 0);

    if (libusb_claim_interface(g_devh, 0) != 0) {
        libusb_close(g_devh);
        g_devh = NULL;
        return false;
    }

    g_usb_io_failed = false;
    fprintf(stderr, "lbp2900d: Persistent USB session opened (04a9:2676, OUT=0x%02x IN=0x%02x)\n",
            g_ep_out, g_ep_in);
    usb_drain_stale_locked();
    return true;
}

/*
 * Low-level USB bulk send & receive (caller holds g_usb_lock)
 */
static bool capt_send_buf_timeout_locked(unsigned int timeout_ms) {
    if (!g_devh || g_sim_disconnect) {
        if (g_sim_disconnect)
            usb_force_close_locked("USB cable unplugged / printer powered off");
        return false;
    }
    size_t off = 0;
    while (off < g_iosize) {
        int chunk = (int)((g_iosize - off > 16384) ? 16384 : (g_iosize - off));
        int actual = 0;
        int r = libusb_bulk_transfer(g_devh, g_ep_out, g_iobuf + off, chunk, &actual, timeout_ms);
        if (r != 0) {
            fprintf(stderr, "lbp2900d: USB OUT error: %s\n", libusb_error_name(r));
            usb_force_close_locked(libusb_error_name(r));
            return false;
        }
        off += (size_t)actual;
    }
    return true;
}

static bool capt_send_buf_locked(void) {
    if (!g_devh && !usb_try_open_locked())
        return false;
    return capt_send_buf_timeout_locked(3000);
}

static bool capt_recv_buf_timeout_locked(size_t offset, size_t expected, unsigned int timeout_ms, int max_timeouts) {
    if (!g_devh || offset + expected > sizeof(g_iobuf))
        return false;
    size_t got = 0;
    int idle_rounds = 0;
    while (got < expected) {
        int actual = 0;
        int r = libusb_bulk_transfer(g_devh, g_ep_in, g_iobuf + offset + got,
                                     (int)(expected - got), &actual, timeout_ms);
        if (r == -7 /* LIBUSB_ERROR_TIMEOUT */) {
            if (++idle_rounds >= max_timeouts) {
                fprintf(stderr, "lbp2900d: USB IN timeout (got %zu of %zu bytes)\n", got, expected);
                usb_force_close_locked("USB transfer timeout");
                return false;
            }
            continue;
        }
        if (r != 0) {
            fprintf(stderr, "lbp2900d: USB IN error: %s\n", libusb_error_name(r));
            usb_force_close_locked(libusb_error_name(r));
            return false;
        }
        if (actual > 0) {
            idle_rounds = 0;
            got += (size_t)actual;
        }
    }
    g_iosize = offset + got;
    return true;
}

static bool capt_recv_buf_locked(size_t offset, size_t expected) {
    return capt_recv_buf_timeout_locked(offset, expected, 2500, 2);
}

static void capt_copy_cmd_locked(uint16_t cmd, const void *buf, size_t size) {
    if (g_iosize + 4 + size > sizeof(g_iobuf))
        return;
    if (buf && size > 0)
        memcpy(g_iobuf + g_iosize + 4, buf, size);
    else
        size = 0;
    g_iobuf[g_iosize + 0] = LO(cmd);
    g_iobuf[g_iosize + 1] = HI(cmd);
    g_iobuf[g_iosize + 2] = LO(size + 4);
    g_iobuf[g_iosize + 3] = HI(size + 4);
    g_iosize += size + 4;
}

static bool capt_send_locked(uint16_t cmd, const void *buf, size_t size) {
    g_iosize = 0;
    capt_copy_cmd_locked(cmd, buf, size);
    return capt_send_buf_locked();
}

static bool capt_sendrecv_locked(uint16_t cmd, const void *buf, size_t size,
                                 void *reply, size_t *reply_size) {
    if (!capt_send_locked(cmd, buf, size))
        return false;
    if (!capt_recv_buf_locked(0, 6))
        return false;

    if (g_iosize != 6 || WORD(g_iobuf[0], g_iobuf[1]) != cmd) {
        unsigned shifts = 0;
        bool resynced = false;
        while (g_iosize == 6 && shifts < CAPT_RESYNC_MAX_BYTES) {
            memmove(g_iobuf, g_iobuf + 1, 5);
            if (!capt_recv_buf_locked(5, 1))
                break;
            shifts++;
            if (g_iosize == 6 && WORD(g_iobuf[0], g_iobuf[1]) == cmd) {
                resynced = true;
                break;
            }
        }
        if (!resynced) {
            fprintf(stderr, "lbp2900d: ERROR: reply stream out of sync for cmd 0x%04x\n", cmd);
            usb_force_close_locked("CAPT protocol out of sync");
            return false;
        }
        fprintf(stderr, "lbp2900d: resynchronized reply stream after %u byte(s)\n", shifts);
    }

    while (1) {
        uint16_t expected_w = WORD(g_iobuf[2], g_iobuf[3]);
        uint16_t expected_bcd = (uint16_t)BCD(g_iobuf[2], g_iobuf[3]);
        if (expected_w == g_iosize || expected_bcd == g_iosize)
            break;
        if (expected_w > g_iosize && (g_iosize == 6 || g_iosize % 64 == 6)) {
            if (!capt_recv_buf_locked(g_iosize, expected_w - g_iosize))
                return false;
            continue;
        }
        break;
    }

    size_t payload_bytes = (g_iosize >= 4) ? (g_iosize - 4) : 0;
    if (reply) {
        size_t copysize = (reply_size && *reply_size < payload_bytes) ? *reply_size : payload_bytes;
        memcpy(reply, g_iobuf + 4, copysize);
    }
    if (reply_size)
        *reply_size = payload_bytes;
    return true;
}

static void capt_multi_begin_locked(uint16_t cmd) {
    g_iobuf[0] = LO(cmd);
    g_iobuf[1] = HI(cmd);
    g_iosize = 4;
}

static void capt_multi_add_locked(uint16_t cmd, const void *buf, size_t size) {
    capt_copy_cmd_locked(cmd, buf, size);
}

static bool capt_multi_send_locked(void) {
    g_iobuf[2] = LO(g_iosize);
    g_iobuf[3] = HI(g_iosize);
    return capt_send_buf_locked();
}

static void decode_status_locked(const uint8_t *s, size_t size) {
    if (size < 2)
        return;
    g_capt_status.status[0] = WORD(s[0], s[1]);
    if (size <= 2)
        return;
    if (size >= 6)
        g_capt_status.buf_level = s[4];
    if (size < 10)
        return;
    g_capt_status.status[1] = WORD(s[8], s[9]);
    if (size <= 10)
        return;

    g_capt_status.aux       = s[CAPT_XSTAT_AUX];
    g_capt_status.xstat_cnt = s[CAPT_XSTAT_CNT];
    g_capt_status.xstat_pap = s[CAPT_XSTAT_PAP];
    if (size >= 14)
        g_capt_status.xstat_eng = WORD(s[CAPT_XSTAT_ENG_LO], s[CAPT_XSTAT_ENG_HI]);

    g_capt_status.status[2] = WORD(s[10], s[11]);
    g_capt_status.status[3] = WORD(s[12], s[13]);

    if (size >= 22) {
        g_capt_status.page_decoding  = WORD(s[14], s[15]);
        g_capt_status.page_printing  = WORD(s[16], s[17]);
        g_capt_status.page_out       = WORD(s[18], s[19]);
        g_capt_status.page_completed = WORD(s[20], s[21]);
    }
    if (size >= 26)
        g_capt_status.status[4] = WORD(s[24], s[25]);
    if (size >= 32)
        g_capt_status.status[5] = WORD(s[30], s[31]);
    if (size >= 36)
        g_capt_status.page_received = WORD(s[34], s[35]);
    if (size >= 40)
        g_capt_status.status[6] = WORD(s[38], s[39]);

    pthread_mutex_lock(&g_status_lock);
    if (g_sim_eng_fault)
        g_capt_status.xstat_eng |= g_sim_eng_fault;
    g_live.raw_capt = g_capt_status;
    g_live.buf_slots_free = CAPT_BUFLEVEL_SLOTS(g_capt_status.buf_level);
    pthread_mutex_unlock(&g_status_lock);
}

static const struct capt_status_s *capt_get_basic_status_locked(void) {
    uint8_t buf[256];
    size_t sz = sizeof(buf);
    if (capt_sendrecv_locked(CAPT_GET_BASIC_STATUS, NULL, 0, buf, &sz))
        decode_status_locked(buf, sz);
    return &g_capt_status;
}

static const struct capt_status_s *capt_get_input_status_locked(void) {
    uint8_t buf[64];
    size_t sz = sizeof(buf);
    if (capt_sendrecv_locked(CAPT_GET_INPUT_STATUS, NULL, 0, buf, &sz) && sz >= 3) {
        g_capt_status.input_tray_byte = buf[2];
    }
    return &g_capt_status;
}

static const struct capt_status_s *capt_get_xstatus_only_locked(bool auto_ack_input) {
    uint8_t buf[256];
    size_t sz = sizeof(buf);
    if (capt_sendrecv_locked(CAPT_GET_EXTENDED_STATUS, NULL, 0, buf, &sz))
        decode_status_locked(buf, sz);
    if (!g_usb_io_failed && auto_ack_input && CAPT_FLAG(&g_capt_status, CAPT_FL_NEED_INPUT_STATUS)) {
        capt_get_input_status_locked();
    }
    return &g_capt_status;
}

static const struct capt_status_s *capt_get_xstatus_locked(void) {
    capt_get_basic_status_locked();
    if (!g_usb_io_failed && CAPT_FLAG(&g_capt_status, CAPT_FL_XSTATUS_CHANGED))
        capt_get_xstatus_only_locked(true);
    if (!g_usb_io_failed && CAPT_FLAG(&g_capt_status, CAPT_FL_NEED_INPUT_STATUS))
        capt_get_input_status_locked();
    return &g_capt_status;
}

static bool capt_wait_ready_locked(void) {
    unsigned waited_ms = 0;
    while (waited_ms < CAPT_WAIT_READY_TIMEOUT_MS) {
        if (!g_devh || g_usb_io_failed)
            return false;
        const struct capt_status_s *s = capt_get_basic_status_locked();
        if (!g_devh || g_usb_io_failed)
            return false;
        if (!CAPT_FLAG(s, CAPT_FL_CMD_BUSY))
            return true;
        usleep(100000);
        waited_ms += 100;
    }
    return false;
}

/*
 * Constant protocol payloads for Canon LBP2900
 */
static const uint8_t k_magic_reserve[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t k_magic_linux_online[8] = { 0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t k_magic_win_online[16] = {
    0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t k_lbp2900_gpio_blink[12] = {
    0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00
};
static const uint8_t k_lbp2900_gpio_init[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};
static const uint8_t k_lbp3000_job_init[2] = { 0x00, 0x00 };
static const uint8_t k_lbp6000_job_init[2] = { 0x01, 0x00 };

static uint16_t reserve_unit_with_recovery_locked(void);

static void clear_printer_led_and_state_locked(bool reserve_and_release) {
    uint16_t jid = 0;
    if (reserve_and_release) {
        capt_get_xstatus_only_locked(true);
        capt_get_xstatus_only_locked(true);
        jid = reserve_unit_with_recovery_locked();
    }
    capt_get_input_status_locked();
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_sendrecv_locked(CAPT_GO_OFFLINE, k_lbp3000_job_init, sizeof(k_lbp3000_job_init), NULL, 0);
    capt_get_basic_status_locked();
    capt_get_input_status_locked();
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);
    capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_linux_online, sizeof(k_magic_linux_online), NULL, 0);
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);
    if (reserve_and_release && jid != 0) {
        uint8_t jbuf[2] = { LO(jid), HI(jid) };
        capt_sendrecv_locked(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
        capt_get_xstatus_only_locked(true);
    }
}

/*
 * Fast 600ms live USB probe & CAPT ready handshake check.
 * Immediately detects if printer was powered off / unplugged, or if it was
 * just plugged in / powered on and is ready to replay queued FIFO jobs.
 */
bool capt_usb_verify_connection(int pending_jobs) {
    pthread_mutex_lock(&g_usb_lock);
    pthread_mutex_lock(&g_status_lock);
    g_live.pending_jobs = pending_jobs;
    pthread_mutex_unlock(&g_status_lock);

    bool was_connected = (g_devh != NULL && !g_usb_io_failed);
    if (!was_connected) {
        if (!usb_try_open_locked()) {
            pthread_mutex_unlock(&g_usb_lock);
            capt_set_pending_jobs_count(pending_jobs, NULL);
            return false;
        }
        /* Newly opened USB device: verify CAPT handshake & clear any stale LED/Out-of-Paper latch */
        uint8_t info_buf[64];
        size_t info_sz = sizeof(info_buf);
        if (!capt_sendrecv_locked(CAPT_GET_PRINTER_INFO, NULL, 0, info_buf, &info_sz)) {
            pthread_mutex_unlock(&g_usb_lock);
            return false;
        }
        clear_printer_led_and_state_locked(true);
        if (g_usb_io_failed || !g_devh) {
            pthread_mutex_unlock(&g_usb_lock);
            return false;
        }
        pthread_mutex_lock(&g_status_lock);
        g_live.paper_out_latched = false;
        pthread_mutex_unlock(&g_status_lock);

        fprintf(stderr, "lbp2900d: [RECONNECTED] Canon LBP-2900 CAPT handshake verified (pending_jobs=%d)\n",
                pending_jobs);
        pthread_mutex_unlock(&g_usb_lock);
        update_live_status(PRN_STATE_READY, "Ready to Print", "Printer is ready");
        return true;
    }

    /* Already open: send fast 600ms BasicStatus ping to detect sudden unplug / power-off */
    g_iosize = 0;
    capt_copy_cmd_locked(CAPT_GET_BASIC_STATUS, NULL, 0);
    if (!capt_send_buf_timeout_locked(600) || !capt_recv_buf_timeout_locked(0, 6, 600, 1)) {
        pthread_mutex_unlock(&g_usb_lock);
        return false;
    }
    if (g_iosize >= 6 && WORD(g_iobuf[0], g_iobuf[1]) == CAPT_GET_BASIC_STATUS) {
        decode_status_locked(g_iobuf + 4, g_iosize - 4);
    }
    pthread_mutex_unlock(&g_usb_lock);
    return true;
}

bool capt_usb_init_persistent(void) {
    return capt_usb_verify_connection(0);
}

void capt_usb_shutdown(void) {
    pthread_mutex_lock(&g_usb_lock);
    if (g_devh) {
        libusb_release_interface(g_devh, 0);
        libusb_close(g_devh);
        g_devh = NULL;
    }
    if (g_usb_ctx) {
        libusb_exit(g_usb_ctx);
        g_usb_ctx = NULL;
    }
    pthread_mutex_unlock(&g_usb_lock);
}

bool capt_usb_is_connected(void) {
    pthread_mutex_lock(&g_status_lock);
    bool conn = g_live.usb_connected && (g_live.state != PRN_STATE_DISCONNECTED);
    pthread_mutex_unlock(&g_status_lock);
    return conn;
}

bool capt_usb_poll_live_status(struct capt_status_s *out_status) {
    if (!capt_usb_verify_connection(g_live.pending_jobs))
        return false;
    pthread_mutex_lock(&g_usb_lock);
    if (g_live.active_job_id == 0 && !g_live.paper_out_latched) {
        capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    }
    const struct capt_status_s *s = capt_get_xstatus_only_locked(true);
    bool ok = (g_devh != NULL && !g_usb_io_failed);
    if (out_status && ok)
        *out_status = *s;
    if (ok && g_live.active_job_id == 0 && !g_live.paper_out_latched &&
        (s->xstat_eng & (CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN | CAPT_ENG_NO_CARTRIDGE)) == 0) {
        update_live_status(PRN_STATE_READY, "Ready to Print", "Printer is ready");
    }
    pthread_mutex_unlock(&g_usb_lock);
    return ok;
}

struct job_session_state {
    uint16_t capt_job_id;
    uint16_t printer_blk;
    uint16_t printer_buf;
    unsigned ipage;
    unsigned printer_page_offset;
    bool     startprint_sent;
    bool     sent_job_cont_once;
    unsigned cont_ticks;
    uint8_t  paper_sz;
};

static bool send_job_info2_locked(uint8_t job_flag, uint16_t capt_job_id) {
    uint8_t buf[72];
    build_set_job_info2_payload(buf, job_flag, capt_job_id);
    return capt_sendrecv_locked(CAPT_SET_JOB_INFO2, buf, sizeof(buf), NULL, 0);
}

static uint16_t reserve_unit_with_recovery_locked(void) {
    uint8_t buf[16] = {0};
    size_t sz = sizeof(buf);
    if (!capt_sendrecv_locked(CAPT_RESERVE_UNIT, k_magic_reserve, sizeof(k_magic_reserve), buf, &sz))
        return 0;

    if (sz >= 1 && (buf[0] & 0x80)) {
        fprintf(stderr, "lbp2900d: ReserveUnit returned 0x%02x — running canonical clear sequence\n", buf[0]);
        const struct capt_status_s *s = capt_get_xstatus_locked();
        if (!g_usb_io_failed && !CAPT_FLAG(s, CAPT_FL_OFFLINE)) {
            capt_sendrecv_locked(CAPT_GO_OFFLINE, k_lbp3000_job_init, sizeof(k_lbp3000_job_init), NULL, 0);
            for (int i = 0; i < 50 && !g_usb_io_failed; ++i) {
                s = capt_get_xstatus_locked();
                if (CAPT_FLAG(s, CAPT_FL_OFFLINE))
                    break;
                usleep(100000);
            }
        }
        capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
        capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
        capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
        capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_linux_online, sizeof(k_magic_linux_online), NULL, 0);
        capt_wait_ready_locked();

        sz = sizeof(buf);
        if (!capt_sendrecv_locked(CAPT_RESERVE_UNIT, k_magic_reserve, sizeof(k_magic_reserve), buf, &sz))
            return 0;
    }
    return (sz >= 4) ? WORD(buf[2], buf[3]) : 1;
}

static bool job_prologue_locked(struct job_session_state *js) {
    uint8_t info_buf[64];
    size_t info_sz = sizeof(info_buf);
    if (!capt_sendrecv_locked(CAPT_GET_PRINTER_INFO, NULL, 0, info_buf, &info_sz))
        return false;
    if (info_sz >= 10) {
        js->printer_blk = WORD(info_buf[6], info_buf[7]);
        js->printer_buf = WORD(info_buf[8], info_buf[9]);
    } else {
        js->printer_blk = 65520;
        js->printer_buf = 64;
    }

    memset(&g_capt_status, 0, sizeof(g_capt_status));
    capt_get_xstatus_locked();
    if (g_usb_io_failed || !g_devh)
        return false;

    for (int i = 0; i < 50 && !g_usb_io_failed; ++i) {
        const struct capt_status_s *s = capt_get_xstatus_locked();
        if (!CAPT_FLAG(s, CAPT_FL_NEED_INPUT_STATUS))
            break;
        capt_get_input_status_locked();
        usleep(150000);
    }

    /* Unconditional double GetExtendedStatus before ReserveUnit (matches genuine Canon Linux ccpd) */
    capt_get_xstatus_only_locked(true);
    capt_get_xstatus_only_locked(true);
    if (g_usb_io_failed || !g_devh)
        return false;

    js->capt_job_id = reserve_unit_with_recovery_locked();
    if (g_usb_io_failed || !g_devh || js->capt_job_id == 0)
        return false;

    fprintf(stderr, "lbp2900d: Reserved Unit (CAPT JobID=%u, Blk=%u, Buf=%u)\n",
            js->capt_job_id, js->printer_blk, js->printer_buf);

    /* Ensure any stale blinking Paper LED from a previously interrupted job is extinguished */
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_sendrecv_locked(CAPT_SET_POWER_DOWN, k_lbp6000_job_init, sizeof(k_lbp6000_job_init), NULL, 0);
    const struct capt_status_s *s = capt_get_xstatus_locked();
    if (!g_usb_io_failed && CAPT_FLAG(s, CAPT_FL_NOTREADY)) {
        for (int i = 0; i < 50 && !g_usb_io_failed; ++i) {
            s = capt_get_xstatus_locked();
            if (!CAPT_FLAG(s, CAPT_FL_NOTREADY))
                break;
            usleep(100000);
        }
        capt_get_xstatus_only_locked(true);
    }

    send_job_info2_locked(CAPT_JOBFLAG_START, js->capt_job_id);

    s = capt_get_xstatus_locked();
    if (!g_usb_io_failed && !CAPT_FLAG(s, CAPT_FL_OFFLINE)) {
        capt_sendrecv_locked(CAPT_GO_OFFLINE, k_lbp3000_job_init, sizeof(k_lbp3000_job_init), NULL, 0);
        for (int i = 0; i < 50 && !g_usb_io_failed; ++i) {
            s = capt_get_xstatus_locked();
            if (CAPT_FLAG(s, CAPT_FL_OFFLINE))
                break;
            usleep(100000);
        }
        capt_get_xstatus_only_locked(true);
    }

    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_linux_online, sizeof(k_magic_linux_online), NULL, 0);
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);

    if (!capt_wait_ready_locked())
        return false;
    capt_get_xstatus_only_locked(true);
    capt_get_xstatus_locked();
    return (!g_usb_io_failed && g_devh != NULL);
}

/*
 * Group 2: Mechanical Fault Recovery (Paper Jam / Cover Open / Missing Cartridge)
 * Waits for user to clear jam and close cover, then resumes without duplicate pages.
 */
static bool mechanical_fault_recovery_locked(struct job_session_state *js,
                                             unsigned pipage,
                                             bool *out_retry_page) {
    bool page_already_shipped = (g_capt_status.page_out >= pipage);
    uint16_t eng = g_capt_status.xstat_eng;

    const char *err_headline =
        (eng & CAPT_ENG_JAM) ? "Paper Jam Detected — Clear Jam & Close Cover" :
        (eng & CAPT_ENG_DOOR_OPEN) ? "Printer Cover is Open — Close Cover to Continue" :
        "Toner Cartridge Missing — Insert Cartridge";

    const char *err_detail =
        (eng & CAPT_ENG_JAM) ? "Open the printer cover, gently remove the jammed sheet, and close the cover. Printing will resume automatically." :
        (eng & CAPT_ENG_DOOR_OPEN) ? "Close the Canon LBP-2900 top cartridge cover. Printing will resume automatically without duplicate pages." :
        "Install the Canon 303 toner cartridge and close the cover to continue.";

    fprintf(stderr, "lbp2900d: [MECHANICAL FAULT] %s (eng=0x%04x, page=%u, shipped=%d)\n",
            err_headline, eng, js->ipage, (int)page_already_shipped);

    update_live_status(PRN_STATE_ERROR, err_headline, err_detail);
    if (g_popup_hook)
        g_popup_hook("error");

    /* Wait until user clears jam and closes cover */
    while (1) {
        if (!g_devh || g_usb_io_failed)
            return false;
        pthread_mutex_lock(&g_status_lock);
        bool canceled = g_live.cancel_requested;
        pthread_mutex_unlock(&g_status_lock);
        if (canceled)
            return false;

        const struct capt_status_s *s = capt_get_xstatus_locked();
        if (!g_devh || g_usb_io_failed)
            return false;

        if (s->page_out >= pipage)
            page_already_shipped = true;

        if ((s->xstat_eng & (CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN | CAPT_ENG_NO_CARTRIDGE)) == 0) {
            fprintf(stderr, "lbp2900d: [MECHANICAL FAULT] Cover closed & fault cleared! Waiting for fuser warm-up...\n");
            break;
        }
        usleep(200000);
    }

    update_live_status(PRN_STATE_PRINTING, "Recovering from Mechanical Fault...",
                       "Cover closed — waiting for Canon LBP-2900 fuser warm-up and clearing engine state...");

    /* Wait for post-cover-close fuser warm-up to finish */
    for (int i = 0; i < 150 && !g_usb_io_failed; ++i) {
        const struct capt_status_s *s = capt_get_xstatus_locked();
        if (!CAPT_FLAG(s, CAPT_FL_NOTREADY) && !CAPT_FLAG(s, CAPT_FL_CMD_BUSY))
            break;
        usleep(200000);
    }

    capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_linux_online, sizeof(k_magic_linux_online), NULL, 0);
    if (!capt_wait_ready_locked())
        return false;

    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);

    if (page_already_shipped) {
        js->printer_page_offset = js->ipage;
        *out_retry_page = false;
    } else {
        js->printer_page_offset = js->ipage - 1;
        *out_retry_page = true;
    }
    return (!g_usb_io_failed && g_devh != NULL);
}

/*
 * Out-of-Paper Recovery Sequence + Physical Printer Button Wait
 * Stops printing, blinks the Canon LBP-2900 LED, alerts the user in the UI,
 * and waits for the user to load paper and press the physical Paper button on the printer.
 */
static bool oop_recovery_and_wait_button_locked(struct job_session_state *js) {
    fprintf(stderr, "lbp2900d: [OUT OF PAPER] Starting Out-of-Paper recovery & button wait on page %u\n",
            js->ipage);

    pthread_mutex_lock(&g_status_lock);
    g_live.paper_out_latched = true;
    g_live.soft_resume_requested = false;
    g_live.button_pressed_seen = false;
    pthread_mutex_unlock(&g_status_lock);

    update_live_status(PRN_STATE_OUT_OF_PAPER,
                       "No Paper — Load Paper & Press Printer Button",
                       "Printing stopped: Out of Paper. Load paper into the tray and press the Paper/Resume button on the Canon LBP-2900 to continue.");

    if (g_popup_hook)
        g_popup_hook("out-of-paper");

    /* 1-3: Acknowledge input status and read extended status */
    capt_get_input_status_locked();
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);

    /* 4-6: Clear misprint, error, and discard unprinted page data */
    capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);

    /* 7-8: Verify status */
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);

    /* 9-12: Pulse GoOnline to reset internal page counters to 0 */
    capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_linux_online, sizeof(k_magic_linux_online), NULL, 0);
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);
    capt_get_xstatus_locked();

    /* 13-15: SetLEDStatus(blink) to blink the printer's indicator and enable the physical Paper button */
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_blink, sizeof(k_lbp2900_gpio_blink), NULL, 0);
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);

    /* 16-17: GoOffline while waiting for user action */
    capt_sendrecv_locked(CAPT_GO_OFFLINE, k_lbp3000_job_init, sizeof(k_lbp3000_job_init), NULL, 0);
    capt_get_basic_status_locked();

    /* Clear any pre-existing NEED_INPUT_STATUS edge before waiting for the user's button press */
    if (!g_usb_io_failed && CAPT_FLAG(&g_capt_status, CAPT_FL_NEED_INPUT_STATUS)) {
        capt_get_input_status_locked();
        capt_get_basic_status_locked();
    }

    fprintf(stderr, "lbp2900d: [OUT OF PAPER] Waiting for user to load paper and press the Paper button on the printer...\n");

    while (1) {
        if (!g_devh || g_usb_io_failed) {
            pthread_mutex_lock(&g_status_lock);
            g_live.paper_out_latched = false;
            pthread_mutex_unlock(&g_status_lock);
            return false;
        }

        pthread_mutex_lock(&g_status_lock);
        bool canceled = g_live.cancel_requested;
        bool soft_resume = g_live.soft_resume_requested;
        pthread_mutex_unlock(&g_status_lock);

        if (canceled) {
            fprintf(stderr, "lbp2900d: Job canceled while waiting for paper.\n");
            capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
            pthread_mutex_lock(&g_status_lock);
            g_live.paper_out_latched = false;
            pthread_mutex_unlock(&g_status_lock);
            return false;
        }

        /* Poll BasicStatus and ExtendedStatus without auto-consuming NEED_INPUT_STATUS */
        const struct capt_status_s *bs = capt_get_basic_status_locked();
        if (!g_devh || g_usb_io_failed) {
            pthread_mutex_lock(&g_status_lock);
            g_live.paper_out_latched = false;
            pthread_mutex_unlock(&g_status_lock);
            return false;
        }
        bool need_in = CAPT_FLAG(bs, CAPT_FL_NEED_INPUT_STATUS);

        const struct capt_status_s *xs = capt_get_xstatus_only_locked(false);
        if (!g_devh || g_usb_io_failed) {
            pthread_mutex_lock(&g_status_lock);
            g_live.paper_out_latched = false;
            pthread_mutex_unlock(&g_status_lock);
            return false;
        }
        bool btn_flag = CAPT_FLAG(xs, CAPT_FL_BUTTON) ||
                        CAPT_FLAG(xs, CAPT_FL_BUTTON1) ||
                        CAPT_FLAG(xs, CAPT_FL_NEED_INPUT_STATUS);

        if (need_in || btn_flag || soft_resume) {
            fprintf(stderr, "lbp2900d: [OUT OF PAPER] Paper button pressed (need_in=%d btn=%d soft=%d)! Resuming job...\n",
                    (int)need_in, (int)btn_flag, (int)soft_resume);
            pthread_mutex_lock(&g_status_lock);
            g_live.button_pressed_seen = true;
            g_live.soft_resume_requested = false;
            pthread_mutex_unlock(&g_status_lock);
            break;
        }
        usleep(120000);
    }

    /* 19: Acknowledge input status and turn off blinking LED */
    capt_get_input_status_locked();
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);

    capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
    capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);

    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);

    /* Bring printer back online (counters reset to 0) */
    capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_linux_online, sizeof(k_magic_linux_online), NULL, 0);
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_get_xstatus_locked();
    capt_get_xstatus_only_locked(true);

    js->printer_page_offset = js->ipage - 1;
    pthread_mutex_lock(&g_status_lock);
    g_live.paper_out_latched = false;
    pthread_mutex_unlock(&g_status_lock);

    char detail[256];
    snprintf(detail, sizeof(detail), "Paper loaded — resuming print from Page %u...", js->ipage);
    update_live_status(PRN_STATE_PRINTING, "Resuming Print Job...", detail);
    return (!g_usb_io_failed && g_devh != NULL);
}

static bool drain_until_buflevel_nonzero_locked(struct job_session_state *js, int *pbuflevel) {
    int slots = *pbuflevel;
    uint8_t prev_b1 = (uint8_t)(g_capt_status.status[0] & 0xFF);
    unsigned waited_ms = 0;
    unsigned poll_ms = 100;

    while (slots == 0) {
        if (!g_devh || g_usb_io_failed)
            return false;
        pthread_mutex_lock(&g_status_lock);
        bool canceled = g_live.cancel_requested;
        pthread_mutex_unlock(&g_status_lock);
        if (canceled)
            return false;

        usleep(poll_ms * 1000);
        waited_ms += poll_ms;
        if (poll_ms < 500)
            poll_ms += 100;

        const struct capt_status_s *s = capt_get_basic_status_locked();
        if (!g_devh || g_usb_io_failed)
            return false;
        slots = CAPT_BUFLEVEL_SLOTS(s->buf_level);

        uint8_t cur_b1 = (uint8_t)(s->status[0] & 0xFF);
        if (cur_b1 != prev_b1) {
            prev_b1 = cur_b1;
            s = capt_get_xstatus_only_locked(true);
            if (!g_devh || g_usb_io_failed)
                return false;
            slots = CAPT_BUFLEVEL_SLOTS(s->buf_level);
        }
        if (slots != 0)
            break;
        if (waited_ms >= CAPT_BUFLEVEL_DRAIN_TIMEOUT_MS) {
            fprintf(stderr, "lbp2900d: ERROR: BufLevel stuck at 0 for %ums on page %u\n", waited_ms, js->ipage);
            return false;
        }
    }
    *pbuflevel = slots;
    return true;
}

static bool send_band_chunk_locked(struct job_session_state *js, const uint8_t *data, size_t size) {
    unsigned pipage = js->ipage - js->printer_page_offset;
    size_t chunk_max = js->printer_blk ? (size_t)js->printer_blk : 65520;
    if (chunk_max > 0xFF00)
        chunk_max = 0xFF00;

    const struct capt_status_s *s = capt_get_basic_status_locked();
    if (!g_devh || g_usb_io_failed)
        return false;
    int slots = CAPT_BUFLEVEL_SLOTS(s->buf_level);
    int chunks_since_poll = 0;

    while (size > 0) {
        if (!g_devh || g_usb_io_failed)
            return false;
        pthread_mutex_lock(&g_status_lock);
        bool canceled = g_live.cancel_requested;
        pthread_mutex_unlock(&g_status_lock);
        if (canceled)
            return false;

        size_t send_sz = (size > chunk_max) ? chunk_max : size;

        if (slots == 0) {
            if (!js->startprint_sent) {
                uint8_t spbuf[2] = { LO(pipage), HI(pipage) };
                for (int w = 0; w < 20 && !g_usb_io_failed && g_capt_status.page_decoding != pipage; ++w) {
                    capt_get_xstatus_only_locked(true);
                    usleep(50000);
                }
                if (!g_usb_io_failed && g_capt_status.page_decoding == pipage) {
                    fprintf(stderr, "lbp2900d: [STREAMING MODE] Printer 2MB buffer full (BufLevel=0) on page %u — firing StartPrint(%u) mid-page\n",
                            js->ipage, pipage);
                    pthread_mutex_lock(&g_status_lock);
                    g_live.streaming_mode = true;
                    pthread_mutex_unlock(&g_status_lock);
                    capt_sendrecv_locked(CAPT_START_PRINT, spbuf, 2, NULL, 0);
                    js->startprint_sent = true;
                }
            }
            if (!drain_until_buflevel_nonzero_locked(js, &slots))
                return false;
            chunks_since_poll = 0;
        }

        if (!capt_send_locked(CAPT_IC_VIDEO_DATA, data, send_sz))
            return false;
        data += send_sz;
        size -= send_sz;
        chunks_since_poll++;

        int poll_interval = (slots >= 15) ? 5 : ((slots >= 8) ? 2 : 1);
        if (chunks_since_poll >= poll_interval) {
            s = capt_get_basic_status_locked();
            if (!g_devh || g_usb_io_failed)
                return false;
            slots = CAPT_BUFLEVEL_SLOTS(s->buf_level);
            if (CAPT_FLAG(s, CAPT_FL_XSTATUS_CHANGED))
                capt_get_xstatus_only_locked(true);
            if (CAPT_FLAG(s, CAPT_FL_NEED_INPUT_STATUS))
                capt_get_input_status_locked();
            chunks_since_poll = 0;
        }
    }

    if (slots == 0 && !js->startprint_sent) {
        uint8_t spbuf[2] = { LO(pipage), HI(pipage) };
        for (int w = 0; w < 20 && !g_usb_io_failed && g_capt_status.page_decoding != pipage; ++w) {
            capt_get_xstatus_only_locked(true);
            usleep(50000);
        }
        if (!g_usb_io_failed && g_capt_status.page_decoding == pipage) {
            pthread_mutex_lock(&g_status_lock);
            g_live.streaming_mode = true;
            pthread_mutex_unlock(&g_status_lock);
            capt_sendrecv_locked(CAPT_START_PRINT, spbuf, 2, NULL, 0);
            js->startprint_sent = true;
        }
    }
    if (slots == 0) {
        if (!drain_until_buflevel_nonzero_locked(js, &slots))
            return false;
    }
    return (!g_usb_io_failed && g_devh != NULL);
}

static bool print_rendered_page_once_locked(struct job_session_state *js, const struct rendered_page_s *page, bool *out_retry_page) {
    *out_retry_page = false;
    unsigned pipage = js->ipage - js->printer_page_offset;

    uint8_t pageparms[40];
    build_ic_begin_page_payload(pageparms, &page->dims, &js->paper_sz);
    js->startprint_sent = false;

    const struct capt_status_s *s = capt_get_xstatus_locked();
    if (!g_devh || g_usb_io_failed)
        return false;

    if (CAPT_FLAG(s, CAPT_FL_UNINIT1) || CAPT_FLAG(s, CAPT_FL_OFFLINE)) {
        capt_sendrecv_locked(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
        capt_sendrecv_locked(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
        capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
        if (!capt_wait_ready_locked())
            return false;
        capt_sendrecv_locked(CAPT_GO_ONLINE, k_magic_win_online, sizeof(k_magic_win_online), NULL, 0);
        if (!capt_wait_ready_locked())
            return false;
    }

    while (CAPT_FLAG(capt_get_xstatus_locked(), CAPT_FL_CMD_BUSY)) {
        if (!g_devh || g_usb_io_failed)
            return false;
        usleep(100000);
    }

    uint8_t hiscoa_hdr[16];
    size_t hsz = hiscoa_format_params(hiscoa_hdr, sizeof(hiscoa_hdr), &hiscoa_default_params);

    capt_multi_begin_locked(CAPT_MULTI_COMMAND);
    capt_multi_add_locked(CAPT_IC_BEGIN_PAGE, pageparms, sizeof(pageparms));
    capt_multi_add_locked(CAPT_IC_BLACK_PLANE, hiscoa_hdr, hsz);
    capt_multi_add_locked(CAPT_IC_BEGIN_DATA, NULL, 0);
    capt_multi_add_locked(CAPT_IC_END_PAGE, NULL, 0);
    if (!capt_multi_send_locked())
        return false;

    /* Group bands into chunks <= 0xFF00 without splitting normal bands,
     * while safely streaming any oversized high-entropy band > chunk_max directly! */
    size_t chunk_max = js->printer_blk ? (size_t)js->printer_blk : 65520;
    if (chunk_max > 0xFF00)
        chunk_max = 0xFF00;

    uint8_t *groupbuf = malloc(chunk_max);
    if (!groupbuf)
        return false;

    size_t group_size = 0;
    for (const struct capt_band_node *b = page->bands; b; b = b->next) {
        if (b->size == 0)
            continue;
        if (b->size > chunk_max) {
            if (group_size > 0) {
                if (!send_band_chunk_locked(js, groupbuf, group_size)) {
                    free(groupbuf);
                    return false;
                }
                group_size = 0;
            }
            if (!send_band_chunk_locked(js, b->data, b->size)) {
                free(groupbuf);
                return false;
            }
            continue;
        }
        if (group_size > 0 && group_size + b->size > chunk_max) {
            if (!send_band_chunk_locked(js, groupbuf, group_size)) {
                free(groupbuf);
                return false;
            }
            group_size = 0;
        }
        memcpy(groupbuf + group_size, b->data, b->size);
        group_size += b->size;
    }
    if (group_size > 0) {
        if (!send_band_chunk_locked(js, groupbuf, group_size)) {
            free(groupbuf);
            return false;
        }
    }
    free(groupbuf);

    /* End of page video data */
    if (!capt_send_locked(CAPT_IC_BLACK_END, NULL, 0))
        return false;

    if (!js->startprint_sent) {
        s = capt_get_xstatus_locked();
        if (!g_devh || g_usb_io_failed)
            return false;
        int wait_loops = 0;
        while (s->page_decoding != pipage && wait_loops < 150) {
            if (!g_devh || g_usb_io_failed)
                return false;
            usleep(100000);
            s = capt_get_xstatus_only_locked(true);
            wait_loops++;
        }
        uint8_t spbuf[2] = { LO(pipage), HI(pipage) };
        if (!capt_sendrecv_locked(CAPT_START_PRINT, spbuf, 2, NULL, 0))
            return false;
    }

    /*
     * Wait until paper passes the registration sensor (`page_out >= pipage`)
     * OR Out-of-Paper / Mechanical Fault is signaled by the engine.
     */
    while (1) {
        if (!g_devh || g_usb_io_failed)
            return false;

        pthread_mutex_lock(&g_status_lock);
        bool canceled = g_live.cancel_requested;
        pthread_mutex_unlock(&g_status_lock);
        if (canceled)
            return false;

        s = capt_get_xstatus_locked();
        if (!g_devh || g_usb_io_failed)
            return false;

        if (s->page_completed >= 1) {
            if (!js->sent_job_cont_once || js->cont_ticks >= 5) {
                send_job_info2_locked(CAPT_JOBFLAG_CONT, js->capt_job_id);
                js->sent_job_cont_once = true;
                js->cont_ticks = 0;
            } else {
                js->cont_ticks++;
            }
        }

        /* Group 2: Mechanical Fault Recovery (Paper Jam / Cover Open / Missing Cartridge) */
        if (s->xstat_eng & (CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN | CAPT_ENG_NO_CARTRIDGE)) {
            if (!mechanical_fault_recovery_locked(js, pipage, out_retry_page))
                return false;
            if (!(*out_retry_page)) {
                pthread_mutex_lock(&g_status_lock);
                g_live.printed_pages = (int)js->ipage;
                g_live.total_pages_lifetime++;
                pthread_mutex_unlock(&g_status_lock);
            }
            return true;
        }

        /* Check Out-of-Paper BEFORE declaring page done */
        if ((CAPT_FLAG(s, CAPT_FL_NOTREADY) || CAPT_FLAG(s, CAPT_FL_OFFLINE) || CAPT_FLAG(s, CAPT_FL_NOPAPER2)) &&
            !CAPT_FLAG(s, CAPT_FL_PRINTING) &&
            !CAPT_FLAG(s, CAPT_FL_PROCESSING1)) {
            s = capt_get_xstatus_only_locked(false);
            if (!g_devh || g_usb_io_failed)
                return false;
            if (s->xstat_eng & (CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN | CAPT_ENG_NO_CARTRIDGE)) {
                if (!mechanical_fault_recovery_locked(js, pipage, out_retry_page))
                    return false;
                return true;
            }
            if (((s->xstat_cnt & CAPT_CNT_PRINT_REJECTED) && s->xstat_pap == 0x00) ||
                CAPT_FLAG(s, CAPT_FL_NOPAPER2) || s->xstat_pap == 0x00) {
                fprintf(stderr, "lbp2900d: Out-of-Paper detected on page %u (Cnt=0x%02x, Pap=0x%02x)\n",
                        js->ipage, s->xstat_cnt, s->xstat_pap);
                if (!oop_recovery_and_wait_button_locked(js))
                    return false;
                *out_retry_page = true;
                return true;
            }
        }

        /* Once paper has passed registration and been shipped (`page_out >= pipage`),
         * we know paper was physically fed (not empty tray!) and we can pipeline the next page! */
        if (s->page_out >= pipage) {
            pthread_mutex_lock(&g_status_lock);
            g_live.printed_pages = (int)js->ipage;
            g_live.total_pages_lifetime++;
            pthread_mutex_unlock(&g_status_lock);
            return true;
        }

        usleep(100000);
    }
}

static void job_epilogue_locked(struct job_session_state *js) {
    if (!g_devh || g_usb_io_failed)
        return;
    uint8_t jbuf[2] = { LO(js->capt_job_id), HI(js->capt_job_id) };
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_get_xstatus_only_locked(true);
    send_job_info2_locked(CAPT_JOBFLAG_END_LIN, js->capt_job_id);
    capt_sendrecv_locked(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);

    /* Wait for physical paper delivery of the final sheet to complete */
    for (int i = 0; i < 300 && !g_usb_io_failed && g_devh; ++i) {
        capt_get_input_status_locked();
        const struct capt_status_s *s = capt_get_xstatus_only_locked(true);
        if (!g_devh || g_usb_io_failed)
            break;
        if (!(s->aux & CAPT_AUX_PAPER_DELIVERY) && !CAPT_FLAG(s, CAPT_FL_PRINTING))
            break;
        usleep(100000);
    }

    if (g_devh && !g_usb_io_failed) {
        usleep(250000);
        usb_drain_stale_locked();
    }
}

/*
 * Group 4.2: Mid-Flight Cancel Cleanup
 * Discards buffered bands in the printer's RAM, allows any sheet currently inside
 * the fuser unit to finish ejecting cleanly so it is never trapped, and releases the unit.
 */
static void job_cancel_cleanup_locked(struct job_session_state *js) {
    if (!g_devh || g_usb_io_failed)
        return;
    fprintf(stderr, "lbp2900d: [CANCEL] Cleaning up mid-flight job & ejecting active fuser sheet...\n");
    uint8_t jbuf[2] = { LO(js->capt_job_id), HI(js->capt_job_id) };
    capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
    capt_sendrecv_locked(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);

    /* Wait up to 8s for any sheet already inside the fuser rollers to finish ejecting */
    for (int i = 0; i < 80 && !g_usb_io_failed && g_devh; ++i) {
        const struct capt_status_s *s = capt_get_xstatus_only_locked(true);
        if (!g_devh || g_usb_io_failed)
            break;
        if (!(s->aux & CAPT_AUX_PAPER_DELIVERY) && !CAPT_FLAG(s, CAPT_FL_PRINTING))
            break;
        usleep(100000);
    }

    send_job_info2_locked(CAPT_JOBFLAG_ABORT, js->capt_job_id);
    capt_sendrecv_locked(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
    if (g_devh && !g_usb_io_failed) {
        usleep(200000);
        usb_drain_stale_locked();
    }
}

struct stream_job_ctx {
    struct job_session_state js;
    bool in_job;
    bool ok;
};

static bool on_rendered_page_callback(struct rendered_page_s *page, int page_idx, void *user_ctx) {
    struct stream_job_ctx *ctx = (struct stream_job_ctx *)user_ctx;

    /* Wait if user paused the queue */
    while (1) {
        pthread_mutex_lock(&g_status_lock);
        bool paused = g_live.paused;
        bool canceled = g_live.cancel_requested;
        pthread_mutex_unlock(&g_status_lock);
        if (canceled) {
            ctx->ok = false;
            return false;
        }
        if (!paused)
            break;
        usleep(150000);
    }

    pthread_mutex_lock(&g_usb_lock);

    if (!ctx->in_job) {
        update_live_status(PRN_STATE_PRINTING, "Starting Print Job...",
                           "Reserving Canon LBP-2900 laser engine and initializing CAPT session...");
        if (!job_prologue_locked(&ctx->js)) {
            pthread_mutex_unlock(&g_usb_lock);
            ctx->ok = false;
            return false;
        }
        ctx->in_job = true;
    }

    ctx->js.ipage = (unsigned)page_idx;

    /* Manual duplex wait before even pages if manual_duplex is enabled */
    if (page->dims.manual_duplex && ctx->js.ipage > 1 && (ctx->js.ipage % 2 == 0)) {
        update_live_status(PRN_STATE_MANUAL_DUPLEX_WAIT,
                           "Manual Duplex — Re-insert Paper & Press Button",
                           "Flip the printed sheet, place it back into the tray, and press the Paper button on the printer.");
        if (g_popup_hook)
            g_popup_hook("manual-duplex");
        capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_blink, sizeof(k_lbp2900_gpio_blink), NULL, 0);
        capt_wait_ready_locked();
        while (1) {
            if (!g_devh || g_usb_io_failed) {
                pthread_mutex_unlock(&g_usb_lock);
                ctx->ok = false;
                return false;
            }
            const struct capt_status_s *s = capt_get_xstatus_only_locked(false);
            if (!g_devh || g_usb_io_failed) {
                pthread_mutex_unlock(&g_usb_lock);
                ctx->ok = false;
                return false;
            }
            pthread_mutex_lock(&g_status_lock);
            bool soft = g_live.soft_resume_requested;
            bool canc = g_live.cancel_requested;
            pthread_mutex_unlock(&g_status_lock);
            if (canc) {
                pthread_mutex_unlock(&g_usb_lock);
                ctx->ok = false;
                return false;
            }
            if (CAPT_FLAG(s, CAPT_FL_BUTTON) || CAPT_FLAG(s, CAPT_FL_BUTTON1) ||
                CAPT_FLAG(s, CAPT_FL_NEED_INPUT_STATUS) || soft) {
                pthread_mutex_lock(&g_status_lock);
                g_live.soft_resume_requested = false;
                pthread_mutex_unlock(&g_status_lock);
                break;
            }
            usleep(120000);
        }
        capt_sendrecv_locked(CAPT_SET_LED_STATUS, k_lbp2900_gpio_init, sizeof(k_lbp2900_gpio_init), NULL, 0);
        capt_wait_ready_locked();
    }

    while (1) {
        char headline[128], detail[256];
        pthread_mutex_lock(&g_status_lock);
        g_live.current_page = page_idx;
        if (g_live.total_pages < page_idx)
            g_live.total_pages = page_idx;
        snprintf(headline, sizeof(headline), "Printing Page %d...", page_idx);
        snprintf(detail, sizeof(detail),
                  "Job #%u (%s) — Streaming %u Hi-SCoA bands (%zu KB compressed) to LBP-2900",
                  g_live.active_job_id, g_live.active_job_name,
                  page->band_count, (page->total_compressed_bytes + 1023) / 1024);
        pthread_mutex_unlock(&g_status_lock);
        update_live_status(PRN_STATE_PRINTING, headline, detail);

        bool retry_page = false;
        if (!print_rendered_page_once_locked(&ctx->js, page, &retry_page)) {
            pthread_mutex_unlock(&g_usb_lock);
            ctx->ok = false;
            return false;
        }
        if (!retry_page)
            break;
        fprintf(stderr, "lbp2900d: Reprinting page %d after hardware recovery\n", page_idx);
    }

    pthread_mutex_unlock(&g_usb_lock);
    return true;
}

bool capt_execute_print_job(uint32_t job_id,
                            const char *job_name,
                            const char *job_user,
                            const char *filepath,
                            const struct lbp2900_prefs *prefs) {
    pthread_mutex_lock(&g_status_lock);
    g_live.active_job_id = job_id;
    snprintf(g_live.active_job_name, sizeof(g_live.active_job_name), "%s", job_name ? job_name : "Document");
    snprintf(g_live.active_job_user, sizeof(g_live.active_job_user), "%s", job_user ? job_user : "user");
    g_live.current_page = 0;
    g_live.total_pages = 0;
    g_live.printed_pages = 0;
    g_live.streaming_mode = false;
    g_live.paper_out_latched = false;
    g_live.cancel_requested = false;
    g_live.soft_resume_requested = false;
    pthread_mutex_unlock(&g_status_lock);

    if ((!prefs || prefs->auto_popup_ui) && g_popup_hook) {
        g_popup_hook("job-start");
    }

    update_live_status(PRN_STATE_RENDERING, "Host Rendering Document...",
                       "Computer is rasterizing 600 DPI bands and compressing with Hi-SCoA...");

    struct stream_job_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ok = true;

    int rendered = render_document_streaming(filepath, prefs, on_rendered_page_callback, &ctx);

    pthread_mutex_lock(&g_usb_lock);
    if (ctx.in_job) {
        if (ctx.ok && rendered > 0) {
            update_live_status(PRN_STATE_PRINTING, "Finishing Print Job...",
                               "Waiting for final sheet delivery from Canon LBP-2900...");
            job_epilogue_locked(&ctx.js);
        } else {
            job_cancel_cleanup_locked(&ctx.js);
        }
    }
    pthread_mutex_unlock(&g_usb_lock);

    if (rendered == RENDER_ERR_INVALID_DIMENSIONS) {
        pthread_mutex_lock(&g_status_lock);
        g_live.active_job_id = 0;
        pthread_mutex_unlock(&g_status_lock);
        update_live_status(PRN_STATE_ERROR, "Rejected: Unsupported Paper Dimensions",
                           "Document page dimensions are outside Canon LBP-2900 physical limits (76x127 mm to 216x356 mm).");
        if (g_popup_hook)
            g_popup_hook("error");
        return false;
    }

    bool success = (ctx.ok && rendered > 0 && !g_usb_io_failed);
    if (success) {
        pthread_mutex_lock(&g_status_lock);
        g_live.total_jobs_completed++;
        g_live.active_job_id = 0;
        pthread_mutex_unlock(&g_status_lock);

        char done_detail[256];
        snprintf(done_detail, sizeof(done_detail),
                 "Completed '%s' (%d page%s)",
                 job_name ? job_name : "Document", rendered, rendered == 1 ? "" : "s");
        update_live_status(PRN_STATE_READY, "Ready to Print", done_detail);
        pthread_mutex_lock(&g_status_lock);
        g_completed_msg_until_ms = get_monotonic_ms() + 2000ULL;
        pthread_mutex_unlock(&g_status_lock);
    } else {
        pthread_mutex_lock(&g_status_lock);
        bool was_canceled = g_live.cancel_requested;
        g_live.active_job_id = 0;
        g_live.paper_out_latched = false;
        pthread_mutex_unlock(&g_status_lock);

        if (was_canceled) {
            update_live_status(PRN_STATE_READY, "Print Job Canceled",
                               "Job was canceled cleanly by user and active sheet ejected.");
        }
    }

    return success;
}

bool capt_execute_cleaning(void) {
    pthread_mutex_lock(&g_usb_lock);
    if (!usb_try_open_locked()) {
        pthread_mutex_unlock(&g_usb_lock);
        return false;
    }
    update_live_status(PRN_STATE_CLEANING, "Cleaning Fixing Assembly...",
                       "Feeding cleaning page through Canon LBP-2900 fuser unit...");
    uint8_t clean_param[2] = { 0x00, 0x00 };
    bool ok = capt_sendrecv_locked(CAPT_CLEANING, clean_param, sizeof(clean_param), NULL, 0);
    capt_wait_ready_locked();
    pthread_mutex_unlock(&g_usb_lock);

    if (ok && !g_usb_io_failed) {
        update_live_status(PRN_STATE_READY, "Ready to Print",
                           "Fuser roller cleaning command completed.");
    }
    return ok;
}
