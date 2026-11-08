/*
 * Canon LASER SHOT LBP-2900 Standalone CAPT Driver & CUPS Replacement
 * Copyright (C) 2026 Clean-Room Reverse Engineered CAPT v1.0 Suite
 *
 * Dedicated exclusively to Canon LBP2900 / LBP2900B (USB ID 04a9:2676).
 */

#ifndef CAPT_DEFS_H
#define CAPT_DEFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LBP2900_VENDOR_ID   0x04a9
#define LBP2900_PRODUCT_ID  0x2676

#define LBP2900_CUPS_SOCK    "/run/cups/cups.sock"
#define LBP2900_ALT_SOCK     "/tmp/lbp2900-cups.sock"
#define LBP2900_CTRL_SOCK    "/tmp/lbp2900-control.sock"
#define LBP2900_CONFIG_PATH  "/tmp/lbp2900-prefs.conf"
#define LBP2900_SPOOL_DIR    "/tmp/lbp2900-spool"
#define LBP2900_IPP_PORT     6310

#define RENDER_ERR_IO                 (-1)
#define RENDER_ERR_INVALID_DIMENSIONS (-2)

#define LO(x) ((uint8_t)((x) & 0xFF))
#define HI(x) ((uint8_t)(((x) >> 8) & 0xFF))
#define WORD(lo, hi) ((uint16_t)(((uint8_t)(lo)) | (((uint16_t)(uint8_t)(hi)) << 8)))
#define BCD(lo, hi) ((((lo) & 0x0F) + (((lo) >> 4) & 0x0F) * 10 + \
                     (((hi) & 0x0F) * 100) + ((((hi) >> 4) & 0x0F) * 1000)))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* CAPT v1.0 Command Opcodes (16-bit LE) */
enum capt_command {
    CAPT_NOP                 = 0xA0A0,
    CAPT_GET_INPUT_STATUS    = 0xA0A1,
    CAPT_GET_EXTENDED_STATUS = 0xA0A8,
    CAPT_GET_IEEE1284_ID     = 0xA1A0,
    CAPT_GET_PRINTER_INFO    = 0xA1A1,
    CAPT_RESERVE_UNIT        = 0xA2A0,
    CAPT_START_0             = 0xA3A2,

    CAPT_IC_VIDEO_DATA       = 0xC0A0,
    CAPT_IC_BLACK_END        = 0xC0A4,

    CAPT_IC_BEGIN_PAGE       = 0xD0A0,
    CAPT_IC_BEGIN_DATA       = 0xD0A1,
    CAPT_IC_END_PAGE         = 0xD0A2,
    CAPT_IC_BLACK_PLANE      = 0xD0A4,
    CAPT_MULTI_COMMAND       = 0xD0A9,

    CAPT_GET_BASIC_STATUS    = 0xE0A0,
    CAPT_CLEAR_ERROR         = 0xE0A2,
    CAPT_CLEAR_MIS_PRINT     = 0xE0A3,
    CAPT_DISCARD_DATA        = 0xE0A4,
    CAPT_GO_ONLINE           = 0xE0A5,
    CAPT_GO_OFFLINE          = 0xE0A6,
    CAPT_START_PRINT         = 0xE0A7,
    CAPT_RELEASE_UNIT        = 0xE0A9,
    CAPT_CLEANING            = 0xE0AD,
    CAPT_SET_POWER_DOWN      = 0xE0BA,

    CAPT_SET_JOB_INFO2       = 0xE1A1,
    CAPT_SET_LED_STATUS      = 0xE1A2,
};

/* Status flags packed into status[0..6] */
#define _CAPT_FL(s, b) (((uint32_t)(s) << 16) | (1u << (b)))
enum capt_flags {
    /* status[0] low byte: bBasicStatus */
    CAPT_FL_PRINTER_FREE      = _CAPT_FL(0, 0),  /* 0x01 */
    CAPT_FL_NOTREADY          = _CAPT_FL(0, 1),  /* 0x02 */
    CAPT_FL_CMD_BUSY          = _CAPT_FL(0, 2),  /* 0x04 */
    CAPT_FL_IM_DATA_BUSY      = _CAPT_FL(0, 3),  /* 0x08 */
    CAPT_FL_OFFLINE           = _CAPT_FL(0, 4),  /* 0x10 */
    CAPT_FL_UNINIT1           = _CAPT_FL(0, 5),  /* 0x20 */
    CAPT_FL_UNIT_FREE         = _CAPT_FL(0, 6),  /* 0x40 */
    CAPT_FL_ERROR             = _CAPT_FL(0, 7),  /* 0x80 */
    /* status[0] high byte: secondary flags */
    CAPT_FL_XSTATUS_CHANGED   = _CAPT_FL(0, 8),  /* Bas1 & 0x01 */
    CAPT_FL_NEED_INPUT_STATUS = _CAPT_FL(0, 9),  /* Bas1 & 0x02 */
    CAPT_FL_READY2            = _CAPT_FL(0, 12),
    CAPT_FL_READY1            = _CAPT_FL(0, 15),
    /* status[1] */
    CAPT_FL_POWERUP           = _CAPT_FL(1, 0),
    CAPT_FL_PRINTING          = _CAPT_FL(1, 2),
    CAPT_FL_BUTTON            = _CAPT_FL(1, 5),
    CAPT_FL_PROCESSING1       = _CAPT_FL(1, 7),
    CAPT_FL_NOPAPER2          = _CAPT_FL(1, 14),
    /* status[2] */
    CAPT_FL_nERROR            = _CAPT_FL(2, 7),
    CAPT_FL_BUTTON1           = _CAPT_FL(2, 8),
    /* status[3] */
    CAPT_FL_POWERUP1          = _CAPT_FL(3, 12),
    /* status[4] */
    CAPT_FL_BUTTON_ON         = _CAPT_FL(4, 0),
};

/* Extended status offsets & bitmasks */
#define CAPT_XSTAT_AUX      8
#define CAPT_XSTAT_CNT      9
#define CAPT_XSTAT_PAP      10
#define CAPT_XSTAT_ENG_LO   12
#define CAPT_XSTAT_ENG_HI   13

#define CAPT_AUX_PRINTER_BUSY   0x02
#define CAPT_AUX_PAPER_DELIVERY 0x04
#define CAPT_AUX_SAFE_TIMER     0x80

#define CAPT_CNT_OVERRUN            0x01
#define CAPT_CNT_UNDERRUN           0x02
#define CAPT_CNT_MISSING_EOP        0x04
#define CAPT_CNT_INVALID_DATA       0x08
#define CAPT_CNT_ENGINE_COMM_ERROR  0x10
#define CAPT_CNT_ENGINE_RESET       0x20
#define CAPT_CNT_PRINT_REJECTED     0x40  /* Out-of-paper rejection */

#define CAPT_ENG_SERVICE_CALL   0x0002
#define CAPT_ENG_CLEANING       0x0004
#define CAPT_ENG_JAM            0x0100
#define CAPT_ENG_NO_CARTRIDGE   0x2000
#define CAPT_ENG_DOOR_OPEN      0x4000

#define CAPT_PAP_READY          0x80
#define CAPT_BUFLEVEL_SLOTS(raw) ((raw) & 0x0F)

/* SetJobInfo2 flags */
#define CAPT_JOBFLAG_START      1
#define CAPT_JOBFLAG_CONT       2
#define CAPT_JOBFLAG_END_LIN    3
#define CAPT_JOBFLAG_ABORT      4
#define CAPT_JOBFLAG_END_WIN    6

struct capt_status_s {
    uint16_t status[7];
    uint16_t page_decoding;   /* Start counter */
    uint16_t page_printing;   /* Printing counter */
    uint16_t page_out;        /* Shipped counter */
    uint16_t page_completed;  /* Printed counter */
    uint16_t page_received;
    uint8_t  buf_level;       /* Low nibble: 0..15 free slots */
    uint8_t  xstat_cnt;       /* Engine comm / PRINT_REJECTED */
    uint8_t  xstat_pap;       /* 0x80 = paper ready, 0x00 = paper out */
    uint8_t  aux;             /* 0x04 = paper delivery active */
    uint16_t xstat_eng;       /* Jam / Door open / No cartridge */
    uint8_t  input_tray_byte; /* GetInputStatus byte 2 (0xC0 ready, 0x80 empty) */
};

static inline bool CAPT_FLAG(const struct capt_status_s *s, enum capt_flags flag) {
    return (s->status[flag >> 16] & (flag & 0xFFFFu)) != 0;
}

/* Hi-SCoA Compression Parameters */
struct hiscoa_params {
    int origin_0;
    int origin_2;
    int origin_3;
    int origin_4;
    int origin_5;
};

enum hiscoa_eob_type {
    HISCOA_EOB_BAND = 0,
    HISCOA_EOB_PAGE = 1,
};

extern const struct hiscoa_params hiscoa_default_params;

size_t hiscoa_compress_band(void *out_buf, size_t out_size,
                            const void *band, unsigned line_size, unsigned nlines,
                            enum hiscoa_eob_type eob_type,
                            const struct hiscoa_params *params);

size_t hiscoa_decompress_band(void *out_raw, size_t out_max_size,
                              const void *comp_buf, size_t comp_size,
                              unsigned line_size,
                              const struct hiscoa_params *params,
                              int *out_eob_type);

size_t hiscoa_format_params(void *buf, size_t size, const struct hiscoa_params *params);

/* Page dimensions & printing preferences */
struct page_dims_s {
    char     media_size[32];  /* "A4", "Letter", "Legal", "A5", "B5", "Executive", "EnvC5", "EnvDL", "EnvMonarch", "Env10" */
    uint8_t  media_type;      /* 0x80=Plain(LBP2900), 1=Heavy, 2=PlainL, 3=HeavyH, 4=Transparency, 5=Envelope */
    uint8_t  media_adapt;     /* 0x11 for 600dpi */
    uint8_t  toner_save;      /* 0=Off, 1=On */
    uint8_t  ink_k;           /* 1..15 (default 7 -> 0x1C/0x1F) */
    uint8_t  manual_duplex;   /* 0=Off, 1=On */
    uint16_t paper_width;     /* in 600dpi pixels */
    uint16_t paper_height;    /* in 600dpi pixels */
    uint16_t margin_width;    /* left margin in pixels */
    uint16_t margin_height;   /* top margin in pixels */
    uint16_t line_size;       /* bytes per scanline (multiple of 8/16, e.g. 592 for A4) */
    uint16_t num_lines;       /* printable scanlines */
    uint16_t band_size;       /* 70 lines per band */
};

/* User-configurable Printing Preferences (like Windows Canon CAPT Driver) */
struct lbp2900_prefs {
    char     media_size[32];  /* default "A4" */
    int      media_type;      /* 0=Plain, 1=Heavy, 2=Plain L, 3=Heavy H, 4=Transparency, 5=Envelope */
    int      copies;          /* 1..99 */
    int      collate;         /* 0 or 1 */
    int      orientation;     /* 0=Portrait, 1=Landscape */
    int      nup;             /* 1, 2, 4 pages per sheet */
    int      manual_duplex;   /* 0=1-sided, 1=Manual 2-sided */
    int      toner_save;      /* 0=Off, 1=On */
    int      density;         /* 1..15, default 7 */
    int      dither_mode;     /* 0=Standard Halftone, 1=Fine Error Diffusion, 2=Text Crisp */
    int      auto_popup_ui;   /* 1=Pop up Status Monitor when printing starts (Windows style) */
    char     page_range[64];  /* "" = All, or e.g. "1-3,5" */
};

/* High-level Daemon / Printer State exported to UI and CLI */
enum printer_ui_state {
    PRN_STATE_DISCONNECTED = 0,
    PRN_STATE_READY        = 1,
    PRN_STATE_RENDERING    = 2,
    PRN_STATE_PRINTING     = 3,
    PRN_STATE_OUT_OF_PAPER = 4,
    PRN_STATE_MANUAL_DUPLEX_WAIT = 5,
    PRN_STATE_PAUSED       = 6,
    PRN_STATE_CLEANING     = 7,
    PRN_STATE_ERROR        = 8,
};

struct daemon_live_status {
    enum printer_ui_state state;
    bool     usb_connected;
    char     headline[128];
    char     detail[256];
    uint32_t active_job_id;
    char     active_job_name[128];
    char     active_job_user[64];
    int      current_page;
    int      total_pages;
    int      printed_pages;
    int      buf_slots_free;      /* 0..15 hardware buffer slots */
    bool     streaming_mode;      /* True when computer streams mid-page to save printer RAM */
    bool     paper_out_latched;   /* Waiting for user to load paper & press printer button */
    bool     button_pressed_seen;
    bool     paused;
    bool     cancel_requested;
    bool     soft_resume_requested;
    uint32_t total_jobs_completed;
    uint32_t total_pages_lifetime;
    int      pending_jobs;
    size_t   daemon_rss_kb;
    struct capt_status_s raw_capt;
};

/* Band list for a single rendered page (kept small in host RAM) */
struct capt_band_node {
    struct capt_band_node *next;
    size_t size;
    uint8_t data[];
};

struct rendered_page_s {
    struct page_dims_s dims;
    unsigned page_number;
    size_t total_compressed_bytes;
    unsigned band_count;
    struct capt_band_node *bands;
};

void free_rendered_page(struct rendered_page_s *page);
void fill_page_dims_for_size(struct page_dims_s *dims, const char *media_size,
                             const struct lbp2900_prefs *prefs);
void build_ic_begin_page_payload(uint8_t out40[40], const struct page_dims_s *dims, uint8_t *out_sz);
void build_set_job_info2_payload(uint8_t out72[72], uint8_t job_flag, uint16_t job_id);

#endif /* CAPT_DEFS_H */
