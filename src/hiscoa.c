/*
 * Hi-SCoA Compressor, Independent Decompressor, and CAPT Payload Builders
 * for Canon LASER SHOT LBP-2900 (CAPT v1.0)
 */

#include "capt_defs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

const struct hiscoa_params hiscoa_default_params = {
    .origin_0 = 0,
    .origin_2 = -7,
    .origin_3 = 1,
    .origin_4 = 0,
    .origin_5 = 4,
};

struct hiscoa_enc_state {
    uint8_t xorval;
    const uint8_t *input_buf;
    size_t input_size;
    size_t input_pos;

    uint8_t *output_buf;
    size_t output_bitsize;
    unsigned bitpos;

    unsigned line_size;
    unsigned nbytes;
    uint8_t bytes[16];
    unsigned origin[6];
};

static inline void push_bits(struct hiscoa_enc_state *state, uint32_t bits, unsigned count) {
    while (count && state->bitpos < state->output_bitsize) {
        unsigned word = state->bitpos >> 3;
        unsigned spc = 8 - (state->bitpos & 7);
        unsigned cnt = (count > spc) ? spc : count;
        uint8_t mask = (uint8_t)((0xFFu >> (8 - cnt)) << (spc - cnt));
        uint32_t val = (bits >> (count - cnt)) << (spc - cnt);
        uint8_t x = state->output_buf[word];
        x ^= state->xorval;
        x &= (uint8_t)~mask;
        x |= (uint8_t)(mask & val);
        x ^= state->xorval;
        state->output_buf[word] = x;
        state->bitpos += cnt;
        count -= cnt;
    }
}

static inline unsigned try_match(const struct hiscoa_enc_state *state, unsigned diff) {
    unsigned pos = (unsigned)state->input_pos;
    if (pos < diff)
        return 0;
    unsigned max_pos = (unsigned)state->input_size;
    if (max_pos > pos + 512 + 127)
        max_pos = pos + 512 + 127;

    while (pos < max_pos && state->input_buf[pos] == state->input_buf[pos - diff]) {
        ++pos;
        if (pos % state->line_size == 0)
            break;
    }
    return pos - (unsigned)state->input_pos;
}

static inline void swap_u(unsigned *a, unsigned *b) {
    unsigned t = *a;
    *a = *b;
    *b = t;
}

static inline unsigned find_msb(unsigned val) {
    if (val == 0)
        return 0;
    return (unsigned)(32 - __builtin_clz(val));
}

static bool try_write_longrepeat(struct hiscoa_enc_state *state) {
    unsigned best_cmd = 0;
    unsigned best_len = 0;

    for (unsigned cmd = 0; cmd < 6; ++cmd) {
        if (state->origin[cmd]) {
            unsigned len = try_match(state, state->origin[cmd]);
            if (len > best_len) {
                best_cmd = cmd;
                best_len = len;
            }
        }
    }

    if (best_len > 1) {
        unsigned nbits;
        unsigned len = best_len;

        if (len > 127) {
            unsigned val = len / 128;
            nbits = find_msb(val) - 1;
            push_bits(state, 0xFC, 8);
            push_bits(state, nbits, 2);
            push_bits(state, ~val, nbits);
            len %= 128;
        }

        push_bits(state, 0xFFFFFFFEu, best_cmd + 1);
        if (best_cmd == 2)
            push_bits(state, 0x0, 1);

        switch (len) {
        case 0:
            push_bits(state, 0x3F, 6);
            break;
        case 1:
            push_bits(state, 0x0, 2);
            break;
        case 2:
            push_bits(state, 0x3, 3);
            break;
        case 3:
            push_bits(state, 0x2, 3);
            break;
        default:
            nbits = find_msb(len) - 1;
            push_bits(state, 0xFFFFFFFEu, nbits);
            push_bits(state, ~len, nbits);
            break;
        }

        if (best_cmd == 2)
            swap_u(&state->origin[2], &state->origin[0]);
        if (best_cmd == 5)
            swap_u(&state->origin[5], &state->origin[3]);

        state->input_pos += best_len;
        return true;
    }
    return false;
}

static bool try_write_byterepeat(struct hiscoa_enc_state *state) {
    uint8_t byte = state->input_buf[state->input_pos];
    for (unsigned i = 0; i < state->nbytes; ++i) {
        if (byte == state->bytes[i]) {
            for (unsigned j = i; j > 0; --j)
                state->bytes[j] = state->bytes[j - 1];
            state->bytes[0] = byte;
            push_bits(state, 0x20u | ((15u - i) & 0xFu), 6);
            state->input_pos += 1;
            return true;
        }
    }
    return false;
}

static void write_simple_byte(struct hiscoa_enc_state *state) {
    uint8_t byte = state->input_buf[state->input_pos];
    if (state->nbytes < 16)
        state->nbytes += 1;
    for (unsigned i = state->nbytes - 1; i > 0; --i)
        state->bytes[i] = state->bytes[i - 1];
    state->bytes[0] = byte;

    if (byte == 0) {
        push_bits(state, 0xFDu, 8);
    } else {
        push_bits(state, 0xD00u | byte, 12);
    }
    state->input_pos += 1;
}

size_t hiscoa_compress_band(void *out_buf, size_t out_size,
                            const void *band, unsigned line_size, unsigned nlines,
                            enum hiscoa_eob_type eob_type,
                            const struct hiscoa_params *params) {
    if (!params)
        params = &hiscoa_default_params;

    struct hiscoa_enc_state state = {
        .xorval = 0x43,
        .input_buf = (const uint8_t *)band,
        .input_size = (size_t)line_size * (size_t)nlines,
        .input_pos = 0,
        .output_buf = (uint8_t *)out_buf,
        .output_bitsize = 8 * out_size,
        .bitpos = 0,
        .line_size = line_size,
        .nbytes = 0,
        .origin = {
            [0] = (unsigned)((int)line_size + params->origin_0),
            [1] = 0,
            [2] = (unsigned)((int)line_size + params->origin_2),
            [3] = (unsigned)params->origin_3,
            [4] = (unsigned)params->origin_4,
            [5] = (unsigned)params->origin_5,
        }
    };

    while (state.input_pos < state.input_size) {
        if (try_write_longrepeat(&state))
            continue;
        if (try_write_byterepeat(&state))
            continue;
        write_simple_byte(&state);
    }

    push_bits(&state, 0xFEu, 8);
    push_bits(&state, (unsigned)eob_type, 2);
    push_bits(&state, 0xFFFFFFFFu, 32 - (state.bitpos % 32));

    return state.bitpos / 8;
}

/*
 * Independent Clean-Room Hi-SCoA Decompressor (for Verification Harness)
 */
struct hiscoa_dec_reader {
    const uint8_t *buf;
    size_t total_bits;
    size_t bitpos;
};

static inline int read_bit(struct hiscoa_dec_reader *r) {
    if (r->bitpos >= r->total_bits)
        return -1;
    uint8_t b = r->buf[r->bitpos >> 3] ^ 0x43u;
    int bit = (b >> (7 - (r->bitpos & 7))) & 1;
    r->bitpos++;
    return bit;
}

static inline uint32_t read_bits(struct hiscoa_dec_reader *r, unsigned count) {
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++) {
        int b = read_bit(r);
        if (b < 0)
            return 0;
        v = (v << 1) | (uint32_t)b;
    }
    return v;
}

static unsigned decode_longrep_len(struct hiscoa_dec_reader *r) {
    unsigned ones = 0;
    while (ones < 6) {
        int b = read_bit(r);
        if (b <= 0)
            break;
        ones++;
    }
    if (ones == 6)
        return 0;
    if (ones == 0) {
        int b1 = read_bit(r);
        if (b1 == 0)
            return 1;
        int b2 = read_bit(r);
        return (b2 == 1) ? 2 : 3;
    }
    unsigned nbits = ones + 1;
    uint32_t n = read_bits(r, nbits);
    return (1u << (nbits + 1)) - 1u - n;
}

size_t hiscoa_decompress_band(void *out_raw, size_t out_max_size,
                              const void *comp_buf, size_t comp_size,
                              unsigned line_size,
                              const struct hiscoa_params *params,
                              int *out_eob_type) {
    if (!params)
        params = &hiscoa_default_params;

    struct hiscoa_dec_reader r = {
        .buf = (const uint8_t *)comp_buf,
        .total_bits = comp_size * 8,
        .bitpos = 0,
    };

    uint8_t *out = (uint8_t *)out_raw;
    size_t out_pos = 0;
    uint8_t stash[16] = {0};
    unsigned stash_len = 0;
    unsigned prefix = 0;

    unsigned origin[6] = {
        [0] = (unsigned)((int)line_size + params->origin_0),
        [1] = 0,
        [2] = (unsigned)((int)line_size + params->origin_2),
        [3] = (unsigned)params->origin_3,
        [4] = (unsigned)params->origin_4,
        [5] = (unsigned)params->origin_5,
    };

    while (r.bitpos < r.total_bits) {
        unsigned ones = 0;
        while (ones < 8) {
            int b = read_bit(&r);
            if (b < 0)
                return out_pos;
            if (b == 0)
                break;
            ones++;
        }

        if (ones == 8) {
            /* 0xFF alignment padding */
            continue;
        }

        if (ones == 7) {
            /* END command (11111110) */
            uint32_t eob = read_bits(&r, 2);
            if (out_eob_type)
                *out_eob_type = (int)eob;
            break;
        }

        if (ones == 6) {
            int sub = read_bit(&r);
            if (sub == 1) {
                /* ZEROBYTE (11111101 = 0xFD) */
                if (stash_len < 16)
                    stash_len++;
                for (unsigned i = stash_len - 1; i > 0; --i)
                    stash[i] = stash[i - 1];
                stash[0] = 0;
                if (out_pos < out_max_size)
                    out[out_pos++] = 0;
                prefix = 0;
            } else {
                /* PREFIX (11111100 = 0xFC) */
                unsigned nbits = read_bits(&r, 2);
                uint32_t n = read_bits(&r, nbits);
                unsigned val = (1u << (nbits + 1)) - 1u - n;
                prefix = val * 128u;
            }
            continue;
        }

        if (ones == 1) {
            /* REPBYTE (10 + 4-bit index) */
            unsigned v = read_bits(&r, 4);
            unsigned idx = 15u - v;
            uint8_t byte = (idx < stash_len) ? stash[idx] : 0;
            for (unsigned j = idx; j > 0; --j)
                stash[j] = stash[j - 1];
            stash[0] = byte;
            if (out_pos < out_max_size)
                out[out_pos++] = byte;
            prefix = 0;
            continue;
        }

        unsigned longrep_cmd = 0;
        if (ones == 0) {
            longrep_cmd = 0;
        } else if (ones == 2) {
            int sub = read_bit(&r);
            if (sub == 1) {
                /* BYTE (1101 + 8-bit literal) */
                uint8_t byte = (uint8_t)read_bits(&r, 8);
                if (stash_len < 16)
                    stash_len++;
                for (unsigned i = stash_len - 1; i > 0; --i)
                    stash[i] = stash[i - 1];
                stash[0] = byte;
                if (out_pos < out_max_size)
                    out[out_pos++] = byte;
                prefix = 0;
                continue;
            }
            longrep_cmd = 2;
        } else {
            longrep_cmd = ones; /* 3, 4, or 5 */
        }

        unsigned count = prefix + decode_longrep_len(&r);
        prefix = 0;
        unsigned diff = origin[longrep_cmd];
        for (unsigned k = 0; k < count; ++k) {
            uint8_t b = (out_pos >= diff) ? out[out_pos - diff] : 0;
            if (out_pos < out_max_size)
                out[out_pos++] = b;
        }
        if (longrep_cmd == 2)
            swap_u(&origin[2], &origin[0]);
        if (longrep_cmd == 5)
            swap_u(&origin[5], &origin[3]);
    }

    return out_pos;
}

size_t hiscoa_format_params(void *buf, size_t size, const struct hiscoa_params *params) {
    if (!params)
        params = &hiscoa_default_params;
    uint8_t b[8] = {
        (uint8_t)(int8_t)params->origin_3,
        (uint8_t)(int8_t)params->origin_5,
        0x01,
        0x01,
        (uint8_t)(int8_t)params->origin_0,
        (uint8_t)(int8_t)params->origin_2,
        LO((uint16_t)(int16_t)params->origin_4),
        HI((uint16_t)(int16_t)params->origin_4),
    };
    if (size < sizeof(b))
        return 0;
    memcpy(buf, b, sizeof(b));
    return sizeof(b);
}

void free_rendered_page(struct rendered_page_s *page) {
    if (!page)
        return;
    struct capt_band_node *cur = page->bands;
    while (cur) {
        struct capt_band_node *next = cur->next;
        free(cur);
        cur = next;
    }
    free(page);
}

/*
 * Standard Paper Size Table at 600 DPI for Canon LBP2900
 * Margins: Left 5mm (118 px), Top 4mm (94 px), Right 4.5mm (106 px), Bottom 4mm (94 px)
 */
struct paper_spec {
    const char *name;
    double width_mm;
    double height_mm;
    uint16_t line_size_bytes;
};

static const struct paper_spec k_paper_specs[] = {
    { "A4",         210.0, 297.0, 592 }, /* 4736 px printable width, 7016 px paper height */
    { "Letter",     215.9, 279.4, 608 }, /* 4864 px printable width, 6600 px paper height */
    { "Legal",      215.9, 355.6, 608 }, /* 4864 px printable width, 8400 px paper height */
    { "Executive",  184.15, 266.7, 516 },
    { "A5",         148.0, 210.0, 408 },
    { "B5",         182.0, 257.0, 508 },
    { "EnvC5",      162.0, 229.0, 444 },
    { "EnvDL",      110.0, 220.0, 288 },
    { "EnvMonarch", 98.4,  190.5, 256 },
    { "Env10",      104.8, 241.3, 272 },
};

void fill_page_dims_for_size(struct page_dims_s *dims, const char *media_size,
                             const struct lbp2900_prefs *prefs) {
    memset(dims, 0, sizeof(*dims));
    const struct paper_spec *spec = &k_paper_specs[0]; /* Default A4 */
    if (media_size && media_size[0]) {
        for (size_t i = 0; i < ARRAY_SIZE(k_paper_specs); ++i) {
            if (strcasecmp(media_size, k_paper_specs[i].name) == 0) {
                spec = &k_paper_specs[i];
                break;
            }
        }
    }

    snprintf(dims->media_size, sizeof(dims->media_size), "%s", spec->name);
    dims->paper_width   = (uint16_t)((spec->width_mm / 25.4) * 600.0 + 0.5);
    dims->paper_height  = (uint16_t)((spec->height_mm / 25.4) * 600.0 + 0.5);
    dims->margin_width  = 118; /* 5.0 mm at 600 DPI */
    dims->margin_height = 94;  /* 4.0 mm at 600 DPI */
    dims->line_size     = spec->line_size_bytes;
    dims->num_lines     = (uint16_t)(dims->paper_height - 2 * dims->margin_height);
    dims->band_size     = 70;
    dims->media_adapt   = 0x11; /* 600 DPI constant */

    if (prefs) {
        dims->toner_save    = prefs->toner_save ? 1 : 0;
        dims->ink_k         = (prefs->density >= 1 && prefs->density <= 15) ? (uint8_t)prefs->density : 7;
        dims->manual_duplex = prefs->manual_duplex ? 1 : 0;
        switch (prefs->media_type) {
            case 0: dims->media_type = 0x80; break; /* LBP2900 Plain Paper */
            case 1: dims->media_type = 0x01; break; /* Heavy Paper */
            case 2: dims->media_type = 0x02; break; /* Plain Paper L */
            case 3: dims->media_type = 0x03; break; /* Heavy Paper H */
            case 4: dims->media_type = 0x04; break; /* Transparency */
            case 5: dims->media_type = 0x05; break; /* Envelope */
            default: dims->media_type = 0x80; break;
        }
    } else {
        dims->toner_save    = 0;
        dims->ink_k         = 7;
        dims->manual_duplex = 0;
        dims->media_type    = 0x80;
    }
}

void build_ic_begin_page_payload(uint8_t out40[40], const struct page_dims_s *dims, uint8_t *out_sz) {
    uint8_t sz = 0x02; /* A4 default */
    if (strncmp(dims->media_size, "A4", 2) == 0) sz = 0x02;
    else if (strncmp(dims->media_size, "A5", 2) == 0) sz = 0x03;
    else if (strncmp(dims->media_size, "B5", 2) == 0) sz = 0x07;
    else if (strncmp(dims->media_size, "Executive", 9) == 0) sz = 0x0A;
    else if (strncmp(dims->media_size, "Legal", 5) == 0) sz = 0x0C;
    else if (strncmp(dims->media_size, "Letter", 6) == 0) sz = 0x0D;
    else if (strncmp(dims->media_size, "EnvC5", 5) == 0) sz = 0x15;
    else if (strncmp(dims->media_size, "Env10", 5) == 0) sz = 0x16;
    else if (strncmp(dims->media_size, "EnvMonarch", 10) == 0) sz = 0x17;
    else if (strncmp(dims->media_size, "EnvDL", 5) == 0) sz = 0x18;
    else if (strncmp(dims->media_size, "3x5", 3) == 0) sz = 0x40;
    else if (strncmp(dims->media_size, "PRC16K", 6) == 0) sz = 0xD4;

    if (out_sz)
        *out_sz = sz;

    uint8_t fm = 0x01;
    uint8_t paper_type = 0x00;
    switch (dims->media_type) {
        case 0x80: fm = 0x01; paper_type = 0x00; break; /* Plain Paper (LBP2900) */
        case 0x00: fm = 0x01; paper_type = 0x01; break;
        case 0x02: fm = 0x02; paper_type = 0x04; break; /* Plain L */
        case 0x01: fm = 0x01; paper_type = 0x03; break; /* Heavy */
        case 0x03: fm = 0x03; paper_type = 0x05; break; /* Heavy H */
        case 0x04: fm = 0x13; paper_type = 0x24; break; /* Transparency */
        case 0x05: fm = 0x05; paper_type = 0x07; break; /* Envelope */
        default:   fm = 0x01; paper_type = 0x00; break;
    }

    uint8_t td = (dims->ink_k > 0) ? (uint8_t)(dims->ink_k << 2) : 0x1f;
    uint8_t pageparms[40] = {
        0x00, 0x00, 0x30, 0x2A, sz, 0x01, 0x00, 0x00,
        td, td, td, td, paper_type, dims->media_adapt, 0x04, 0x00,
        0x01, 0x01, 0x02, dims->toner_save, 0x01, 0x00,
        LO(dims->margin_width), HI(dims->margin_width),
        LO(dims->margin_height), HI(dims->margin_height),
        LO(dims->line_size), HI(dims->line_size),
        LO(dims->num_lines), HI(dims->num_lines),
        LO(dims->paper_width), HI(dims->paper_width),
        LO(dims->paper_height), HI(dims->paper_height),
        0x00, 0x00, fm, 0x00, 0x00, 0x00,
    };
    memcpy(out40, pageparms, 40);
}

void build_set_job_info2_payload(uint8_t out72[72], uint8_t job_flag, uint16_t job_id) {
    time_t rawtime = time(NULL);
    struct tm tm_buf;
    const struct tm *tm = localtime_r(&rawtime, &tm_buf);
    int16_t tz = (int16_t)(timezone / 60);
    uint16_t year = (uint16_t)(tm->tm_year + 1900);
    uint8_t month = (uint8_t)(tm->tm_mon + 1);

    memset(out72, 0, 72);
    out72[16] = job_flag;
    out72[17] = 0x01; /* NumberUp */
    out72[18] = LO(job_id);
    out72[19] = HI(job_id);
    out72[20] = (uint8_t)(tz & 0xFF);
    out72[21] = (uint8_t)((tz >> 8) & 0xFF);
    out72[22] = (uint8_t)(tz & 0xFF);
    out72[23] = (uint8_t)((tz >> 8) & 0xFF);
    out72[24] = LO(year);
    out72[25] = HI(year);
    out72[26] = month;
    out72[27] = (uint8_t)tm->tm_mday;
    out72[28] = (uint8_t)tm->tm_hour;
    out72[29] = (uint8_t)tm->tm_min;
    out72[30] = (uint8_t)tm->tm_sec;
}
