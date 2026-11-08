/*
 * Computer-Assisted Host Renderer for Canon LASER SHOT LBP-2900
 * Streams 600x600 DPI 1-bit monochrome bands via Hi-SCoA compression.
 *
 * Hardened against:
 *  - Group 3.1: Blank white page (0x00) vs 100% solid black (0xFF) & high-entropy noise bands
 *  - Group 3.2: Irregular paper dimensions (auto-detects DL Envelope / A5 / Legal; cleanly rejects
 *               impossible physical sizes like 1x1cm or 210x600mm banners before roller feed)
 *  - Group 3.3: Ultra-large vector / 1200 DPI raster payloads via Ghostscript 8MB clist band cap
 *  - Group 4.1: Thread-safe atomic temp file naming under concurrent job bursts
 *  - Group 4.2: Instant SIGKILL termination of Ghostscript child process on mid-flight job cancel
 */

#define _GNU_SOURCE
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <cups/raster.h>

static unsigned g_tmp_seq = 0;

static bool page_in_range(int page_num, const char *range_str) {
    if (!range_str || !range_str[0])
        return true;
    const char *p = range_str;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (!*p)
            break;
        char *end = NULL;
        long start = strtol(p, &end, 10);
        if (end == p)
            break;
        p = end;
        long finish = start;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '-') {
            p++;
            finish = strtol(p, &end, 10);
            if (end == p)
                finish = 999999;
            else
                p = end;
        }
        if (page_num >= start && page_num <= finish)
            return true;
    }
    return false;
}

static size_t center_offset_bytes(size_t small_bytes, size_t large_bytes) {
    if (small_bytes >= large_bytes)
        return 0;
    return (large_bytes - small_bytes) / 2;
}

/*
 * Validate physical dimensions (in mm) against Canon LBP-2900 hardware roller limits:
 *   Width : 76.2 mm (3.0 in) .. 215.9 mm (8.5 in)   [with 2.5mm tolerance: 73.5 .. 218.5 mm]
 *   Length: 127.0 mm (5.0 in) .. 355.6 mm (14.0 in) [with 2.5mm tolerance: 124.5 .. 358.5 mm]
 * Also auto-matches standard envelope / paper sizes (e.g. DL Envelope 110x220mm).
 */
static bool validate_and_map_dimensions_mm(double w_mm, double h_mm, char *inout_media_size, size_t media_sz_cap) {
    double short_mm = (w_mm < h_mm) ? w_mm : h_mm;
    double long_mm  = (w_mm < h_mm) ? h_mm : w_mm;

    if (short_mm < 73.5 || short_mm > 218.5 || long_mm < 124.5 || long_mm > 358.5) {
        fprintf(stderr,
                "lbp2900d: [REJECTED] Invalid paper dimensions %.1f x %.1f mm "
                "(Canon LBP-2900 physical limits: 76.2x127.0 mm to 215.9x355.6 mm)\n",
                w_mm, h_mm);
        return false;
    }

    if (inout_media_size && (inout_media_size[0] == '\0' || strcmp(inout_media_size, "A4") == 0)) {
        struct {
            const char *name;
            double w;
            double h;
        } known[] = {
            { "A4",         210.0, 297.0 },
            { "Letter",     215.9, 279.4 },
            { "Legal",      215.9, 355.6 },
            { "Executive",  184.2, 266.7 },
            { "B5",         182.0, 257.0 },
            { "A5",         148.0, 210.0 },
            { "EnvC5",      162.0, 229.0 },
            { "EnvDL",      110.0, 220.0 },
            { "EnvMonarch",  98.4, 190.5 },
            { "Env10",      104.8, 241.3 },
            { "3x5",         76.2, 127.0 },
        };
        for (size_t i = 0; i < ARRAY_SIZE(known); ++i) {
            if (fabs(short_mm - known[i].w) <= 4.0 && fabs(long_mm - known[i].h) <= 4.0) {
                snprintf(inout_media_size, media_sz_cap, "%s", known[i].name);
                break;
            }
        }
    }
    return true;
}

/*
 * Scan PDF (/MediaBox) or PostScript (/PageSize or %%BoundingBox) for page dimensions in points (1/72 inch)
 */
static bool inspect_pdf_or_ps_dimensions(const char *filepath, char *inout_media_size, size_t media_sz_cap) {
    FILE *f = fopen(filepath, "rb");
    if (!f)
        return true;

    /* Read up to first 64 KB + last 16 KB (PDF page dictionaries can be near start or end) */
    size_t cap = 65536;
    char *buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return true;
    }
    size_t n = fread(buf, 1, cap, f);
    buf[n] = '\0';

    /* Replace NUL bytes inside binary PDF stream with spaces so strstr works across first chunk */
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0')
            buf[i] = ' ';
    }

    double w_pt = 0.0, h_pt = 0.0;
    bool found = false;

    const char *mb = strstr(buf, "/MediaBox");
    if (!mb) {
        /* Also check tail of file if PDF catalog is at the end */
        if (fseek(f, -16384, SEEK_END) == 0) {
            size_t nt = fread(buf, 1, 16384, f);
            buf[nt] = '\0';
            for (size_t i = 0; i < nt; ++i) {
                if (buf[i] == '\0')
                    buf[i] = ' ';
            }
            mb = strstr(buf, "/MediaBox");
        }
    }

    if (mb) {
        const char *br = strchr(mb, '[');
        if (br && (br - mb) < 64) {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (sscanf(br + 1, " %lf %lf %lf %lf", &x0, &y0, &x1, &y1) == 4) {
                w_pt = fabs(x1 - x0);
                h_pt = fabs(y1 - y0);
                found = (w_pt > 0.1 && h_pt > 0.1);
            }
        }
    } else {
        const char *ps_sz = strstr(buf, "/PageSize");
        if (ps_sz) {
            const char *br = strchr(ps_sz, '[');
            if (br && (br - ps_sz) < 48) {
                if (sscanf(br + 1, " %lf %lf", &w_pt, &h_pt) == 2) {
                    found = (w_pt > 0.1 && h_pt > 0.1);
                }
            }
        }
        if (!found) {
            const char *bb = strstr(buf, "%%BoundingBox:");
            if (bb) {
                double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (sscanf(bb + 14, " %lf %lf %lf %lf", &x0, &y0, &x1, &y1) == 4) {
                    w_pt = fabs(x1 - x0);
                    h_pt = fabs(y1 - y0);
                    found = (w_pt > 0.1 && h_pt > 0.1);
                }
            }
        }
    }

    free(buf);
    fclose(f);

    if (!found)
        return true;

    double w_mm = w_pt * 25.4 / 72.0;
    double h_mm = h_pt * 25.4 / 72.0;
    return validate_and_map_dimensions_mm(w_mm, h_mm, inout_media_size, media_sz_cap);
}

/*
 * Read next Netpbm P4 header from stream `fp`.
 * Returns true if a page header was read, false on EOF.
 */
static bool read_pbm_p4_header(FILE *fp, unsigned *out_width, unsigned *out_height) {
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (!isspace(c))
            break;
    }
    if (c == EOF)
        return false;
    int c2 = fgetc(fp);
    if (c != 'P' || c2 != '4')
        return false;

    unsigned vals[2] = {0, 0};
    int v_idx = 0;
    while (v_idx < 2) {
        c = fgetc(fp);
        if (c == EOF)
            return false;
        if (c == '#') {
            while ((c = fgetc(fp)) != EOF && c != '\n')
                ;
            continue;
        }
        if (isspace(c))
            continue;
        if (isdigit(c)) {
            unsigned v = (unsigned)(c - '0');
            while ((c = fgetc(fp)) != EOF && isdigit(c)) {
                if (v > 100000)
                    return false;
                v = v * 10 + (unsigned)(c - '0');
            }
            vals[v_idx++] = v;
            if (c == '#') {
                while ((c = fgetc(fp)) != EOF && c != '\n')
                    ;
            }
        } else {
            return false;
        }
    }
    *out_width = vals[0];
    *out_height = vals[1];
    return (*out_width > 0 && *out_height > 0);
}

/*
 * Compress one P4 PBM page from `fp` into a `struct rendered_page_s`.
 * Uses a single reusable 70-line band buffer (~41 KB) so host memory stays minimal.
 */
static struct rendered_page_s *compress_pbm_page(FILE *fp, unsigned pbm_w, unsigned pbm_h,
                                                 const struct page_dims_s *dims,
                                                 unsigned page_num, bool discard_only) {
    unsigned src_bpl = (pbm_w + 7u) / 8u;
    if (src_bpl == 0 || src_bpl > 32768)
        return NULL;

    uint8_t *linebuf = calloc(1, src_bpl);
    if (!linebuf)
        return NULL;

    if (discard_only) {
        for (unsigned y = 0; y < pbm_h; ++y) {
            if (fread(linebuf, 1, src_bpl, fp) != src_bpl)
                break;
        }
        free(linebuf);
        return NULL;
    }

    struct rendered_page_s *page = calloc(1, sizeof(*page));
    if (!page) {
        free(linebuf);
        return NULL;
    }
    page->dims = *dims;
    page->page_number = page_num;

    unsigned band_lines = dims->band_size;
    unsigned dst_bpl = dims->line_size;
    unsigned compsize = 2u * dst_bpl * band_lines + 4096u;

    uint8_t *bandbuf = calloc(dst_bpl, band_lines);
    uint8_t *compbuf = calloc(1, compsize);
    if (!bandbuf || !compbuf) {
        free(linebuf);
        free(bandbuf);
        free(compbuf);
        free(page);
        return NULL;
    }

    unsigned copy_bytes = (src_bpl < dst_bpl) ? src_bpl : dst_bpl;
    unsigned shift_dst  = (src_bpl < dst_bpl) ? (unsigned)center_offset_bytes(src_bpl, dst_bpl) : 0;
    unsigned shift_src  = (dst_bpl < src_bpl) ? (unsigned)center_offset_bytes(dst_bpl, src_bpl) : 0;

    struct capt_band_node *last_band = NULL;

    for (unsigned iband = 0; iband * band_lines < dims->num_lines; ++iband) {
        unsigned start_line = iband * band_lines;
        unsigned nlines = band_lines;
        if (start_line + nlines >= dims->num_lines)
            nlines = dims->num_lines - start_line;

        memset(bandbuf, 0, (size_t)dst_bpl * (size_t)band_lines);

        for (unsigned iline = 0; iline < nlines; ++iline) {
            if (start_line + iline < pbm_h) {
                if (fread(linebuf, 1, src_bpl, fp) != src_bpl) {
                    memset(linebuf, 0, src_bpl);
                }
            } else {
                memset(linebuf, 0, src_bpl);
            }
            memcpy(bandbuf + iline * dst_bpl + shift_dst, linebuf + shift_src, copy_bytes);
        }

        enum hiscoa_eob_type eob = (start_line + nlines >= dims->num_lines) ? HISCOA_EOB_PAGE : HISCOA_EOB_BAND;
        size_t csz = hiscoa_compress_band(compbuf, compsize, bandbuf, dst_bpl, nlines, eob, &hiscoa_default_params);

        struct capt_band_node *node = calloc(1, sizeof(*node) + csz);
        if (!node)
            break;
        node->size = csz;
        memcpy(node->data, compbuf, csz);

        if (!page->bands)
            page->bands = node;
        if (last_band)
            last_band->next = node;
        last_band = node;
        page->band_count++;
        page->total_compressed_bytes += csz;
    }

    /* Consume any trailing scanlines if pbm_h > dims->num_lines */
    for (unsigned y = dims->num_lines; y < pbm_h; ++y) {
        if (fread(linebuf, 1, src_bpl, fp) != src_bpl)
            break;
    }

    free(linebuf);
    free(bandbuf);
    free(compbuf);
    return page;
}

/*
 * Render a CUPS Raster stream directly using libcupsimage
 */
static int render_cups_raster_file(const char *filepath,
                                   const struct lbp2900_prefs *prefs,
                                   page_ready_callback_fn cb,
                                   void *user_ctx) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0)
        return RENDER_ERR_IO;
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        close(fd);
        return RENDER_ERR_IO;
    }

    struct cups_page_header2_s header;
    int page_idx = 0;
    int emitted = 0;

    while (cupsRasterReadHeader2(ras, &header)) {
        page_idx++;
        char media_buf[64];
        snprintf(media_buf, sizeof(media_buf), "%s",
                 header.MediaType[0] ? header.MediaType : (prefs ? prefs->media_size : "A4"));

        if (header.PageSize[0] > 0 && header.PageSize[1] > 0) {
            double w_mm = (double)header.PageSize[0] * 25.4 / 72.0;
            double h_mm = (double)header.PageSize[1] * 25.4 / 72.0;
            if (!validate_and_map_dimensions_mm(w_mm, h_mm, media_buf, sizeof(media_buf))) {
                cupsRasterClose(ras);
                close(fd);
                return RENDER_ERR_INVALID_DIMENSIONS;
            }
        }

        struct page_dims_s dims;
        fill_page_dims_for_size(&dims, media_buf, prefs);

        unsigned src_bpl = header.cupsBytesPerLine;
        if (src_bpl == 0 || src_bpl > 32768)
            break;
        uint8_t *linebuf = calloc(1, src_bpl);
        if (!linebuf)
            break;

        if (!page_in_range(page_idx, prefs ? prefs->page_range : NULL)) {
            for (unsigned y = 0; y < header.cupsHeight; ++y)
                cupsRasterReadPixels(ras, linebuf, src_bpl);
            free(linebuf);
            continue;
        }

        struct rendered_page_s *page = calloc(1, sizeof(*page));
        page->dims = dims;
        page->page_number = (unsigned)(emitted + 1);

        unsigned band_lines = dims.band_size;
        unsigned dst_bpl = dims.line_size;
        unsigned compsize = 2u * dst_bpl * band_lines + 4096u;
        uint8_t *bandbuf = calloc(dst_bpl, band_lines);
        uint8_t *compbuf = calloc(1, compsize);

        unsigned copy_bytes = (src_bpl < dst_bpl) ? src_bpl : dst_bpl;
        unsigned shift_dst  = (src_bpl < dst_bpl) ? (unsigned)center_offset_bytes(src_bpl, dst_bpl) : 0;
        unsigned shift_src  = (dst_bpl < src_bpl) ? (unsigned)center_offset_bytes(dst_bpl, src_bpl) : 0;
        struct capt_band_node *last_band = NULL;

        for (unsigned iband = 0; iband * band_lines < dims.num_lines; ++iband) {
            unsigned start_line = iband * band_lines;
            unsigned nlines = band_lines;
            if (start_line + nlines >= dims.num_lines)
                nlines = dims.num_lines - start_line;
            memset(bandbuf, 0, (size_t)dst_bpl * (size_t)band_lines);

            for (unsigned iline = 0; iline < nlines; ++iline) {
                if (start_line + iline < header.cupsHeight) {
                    if (cupsRasterReadPixels(ras, linebuf, src_bpl) != src_bpl)
                        memset(linebuf, 0, src_bpl);
                } else {
                    memset(linebuf, 0, src_bpl);
                }
                memcpy(bandbuf + iline * dst_bpl + shift_dst, linebuf + shift_src, copy_bytes);
            }

            enum hiscoa_eob_type eob = (start_line + nlines >= dims.num_lines) ? HISCOA_EOB_PAGE : HISCOA_EOB_BAND;
            size_t csz = hiscoa_compress_band(compbuf, compsize, bandbuf, dst_bpl, nlines, eob, &hiscoa_default_params);
            struct capt_band_node *node = calloc(1, sizeof(*node) + csz);
            node->size = csz;
            memcpy(node->data, compbuf, csz);
            if (!page->bands) page->bands = node;
            if (last_band) last_band->next = node;
            last_band = node;
            page->band_count++;
            page->total_compressed_bytes += csz;
        }
        for (unsigned y = dims.num_lines; y < header.cupsHeight; ++y)
            cupsRasterReadPixels(ras, linebuf, src_bpl);

        free(linebuf);
        free(bandbuf);
        free(compbuf);

        emitted++;
        bool keep_going = cb(page, emitted, user_ctx);
        free_rendered_page(page);
        if (!keep_going)
            break;
    }

    cupsRasterClose(ras);
    close(fd);
    return emitted;
}

/*
 * Convert a Plain Text file into a clean paginated PostScript file
 */
static int convert_text_to_ps(const char *txt_path, const char *ps_path) {
    FILE *in = fopen(txt_path, "r");
    if (!in)
        return -1;
    FILE *out = fopen(ps_path, "w");
    if (!out) {
        fclose(in);
        return -1;
    }

    fprintf(out,
        "%%!PS-Adobe-3.0\n"
        "%%%%Creator: Canon LBP2900 Standalone CAPT Engine\n"
        "/Courier findfont 10.5 scalefont setfont\n"
        "/lh 13 def\n"
        "/top 800 def\n"
        "/bot 42 def\n"
        "/left 36 def\n"
        "/y top def\n"
        "/nl { /y y lh sub def y bot lt { showpage /Courier findfont 10.5 scalefont setfont /y top def } if } def\n");

    char line[1024];
    bool any_content = false;
    while (fgets(line, sizeof(line), in)) {
        any_content = true;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Wrap at 82 columns */
        size_t pos = 0;
        do {
            size_t chunk = (len - pos > 82) ? 82 : (len - pos);
            fprintf(out, "left y moveto (");
            for (size_t i = 0; i < chunk; ++i) {
                char c = line[pos + i];
                if (c == '(' || c == ')' || c == '\\')
                    fputc('\\', out);
                if ((unsigned char)c >= 32 && (unsigned char)c < 127)
                    fputc(c, out);
                else
                    fputc(' ', out);
            }
            fprintf(out, ") show nl\n");
            pos += chunk;
        } while (pos < len);
    }

    if (!any_content) {
        fprintf(out, "left y moveto () show\n");
    }
    fprintf(out, "showpage\n%%%%EOF\n");
    fclose(in);
    fclose(out);
    return 0;
}

int render_document_streaming(const char *filepath,
                              const struct lbp2900_prefs *prefs,
                              page_ready_callback_fn cb,
                              void *user_ctx) {
    FILE *probe = fopen(filepath, "rb");
    if (!probe)
        return RENDER_ERR_IO;
    uint8_t magic[8] = {0};
    size_t nread = fread(magic, 1, sizeof(magic), probe);
    fclose(probe);
    if (nread == 0)
        return RENDER_ERR_IO;

    /* 1. Check for CUPS Raster magic ("RaSt", "tSaR", "RaS2", "2SaR", "RaS3", "3SaR") */
    if (nread >= 4 &&
        (memcmp(magic, "RaSt", 4) == 0 || memcmp(magic, "tSaR", 4) == 0 ||
         memcmp(magic, "RaS2", 4) == 0 || memcmp(magic, "2SaR", 4) == 0 ||
         memcmp(magic, "RaS3", 4) == 0 || memcmp(magic, "3SaR", 4) == 0)) {
        return render_cups_raster_file(filepath, prefs, cb, user_ctx);
    }

    char effective_media[32];
    snprintf(effective_media, sizeof(effective_media), "%s",
             (prefs && prefs->media_size[0]) ? prefs->media_size : "A4");

    /* 2. Check for raw Netpbm P4 file */
    if (nread >= 2 && magic[0] == 'P' && magic[1] == '4') {
        FILE *fp = fopen(filepath, "rb");
        if (!fp)
            return RENDER_ERR_IO;
        unsigned w = 0, h = 0;
        int page_idx = 0, emitted = 0;
        while (read_pbm_p4_header(fp, &w, &h)) {
            double w_mm = (double)w * 25.4 / 600.0;
            double h_mm = (double)h * 25.4 / 600.0;
            if (!validate_and_map_dimensions_mm(w_mm, h_mm, effective_media, sizeof(effective_media))) {
                fclose(fp);
                return RENDER_ERR_INVALID_DIMENSIONS;
            }
            struct page_dims_s dims;
            fill_page_dims_for_size(&dims, effective_media, prefs);

            page_idx++;
            bool in_rng = page_in_range(page_idx, prefs ? prefs->page_range : NULL);
            struct rendered_page_s *page = compress_pbm_page(fp, w, h, &dims, (unsigned)(emitted + 1), !in_rng);
            if (in_rng && page) {
                emitted++;
                bool cont = cb(page, emitted, user_ctx);
                free_rendered_page(page);
                if (!cont)
                    break;
            }
        }
        fclose(fp);
        return emitted;
    }

    /* 3. If PDF (%PDF) or PostScript (%!PS), validate physical paper dimensions first */
    bool is_pdf_or_ps = (nread >= 4 && (memcmp(magic, "%PDF", 4) == 0 || memcmp(magic, "%!PS", 4) == 0));
    if (is_pdf_or_ps) {
        if (!inspect_pdf_or_ps_dimensions(filepath, effective_media, sizeof(effective_media))) {
            return RENDER_ERR_INVALID_DIMENSIONS;
        }
    }

    struct page_dims_s dims;
    fill_page_dims_for_size(&dims, effective_media, prefs);

    char temp_ps[256] = {0};
    const char *input_for_gs = filepath;
    if (!is_pdf_or_ps) {
        unsigned seq = __sync_add_and_fetch(&g_tmp_seq, 1);
        snprintf(temp_ps, sizeof(temp_ps), "/tmp/lbp2900-txt-%d-%u.ps", (int)getpid(), seq);
        if (convert_text_to_ps(filepath, temp_ps) == 0) {
            input_for_gs = temp_ps;
        }
    }

    /*
     * 4. Stream via Ghostscript into 600x600 DPI 1-bit monochrome P4 frames
     * Uses fork() + execvp() with 8MB clist memory limits and instant SIGKILL on cancel!
     */
    unsigned width_px = (unsigned)dims.line_size * 8u;
    unsigned height_px = (unsigned)dims.num_lines;

    int copies = (prefs && prefs->copies >= 1 && prefs->copies <= 99) ? prefs->copies : 1;
    int total_emitted = 0;

    char geom_arg[64];
    snprintf(geom_arg, sizeof(geom_arg), "-g%ux%u", width_px, height_px);

    for (int copy_iter = 0; copy_iter < copies; ++copy_iter) {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            if (temp_ps[0]) unlink(temp_ps);
            return RENDER_ERR_IO;
        }

        pid_t gs_pid = fork();
        if (gs_pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            if (temp_ps[0]) unlink(temp_ps);
            return RENDER_ERR_IO;
        }

        if (gs_pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            for (int cfd = 3; cfd < 1024; ++cfd)
                close(cfd);
            const char *gs_argv[] = {
                "gs",
                "-q",
                "-dSAFER",
                "-dBATCH",
                "-dNOPAUSE",
                "-dBandBufferSpace=4000000",
                "-dMaxBitmap=8000000",
                "-dBufferSpace=8000000",
                "-dNumRenderingThreads=1",
                "-sDEVICE=pbmraw",
                "-r600x600",
                geom_arg,
                "-dPDFFitPage",
                "-sOutputFile=-",
                input_for_gs,
                NULL
            };
            execvp("gs", (char * const *)gs_argv);
            _exit(127);
        }

        close(pipefd[1]);
        FILE *gs_pipe = fdopen(pipefd[0], "rb");
        if (!gs_pipe) {
            close(pipefd[0]);
            kill(gs_pid, SIGKILL);
            waitpid(gs_pid, NULL, 0);
            if (temp_ps[0]) unlink(temp_ps);
            return RENDER_ERR_IO;
        }

        unsigned w = 0, h = 0;
        int src_page_idx = 0;
        bool aborted = false;

        while (read_pbm_p4_header(gs_pipe, &w, &h)) {
            src_page_idx++;
            bool in_rng = page_in_range(src_page_idx, prefs ? prefs->page_range : NULL);
            struct rendered_page_s *page = compress_pbm_page(gs_pipe, w, h, &dims,
                                                             (unsigned)(total_emitted + 1), !in_rng);
            if (in_rng && page) {
                total_emitted++;
                bool keep_going = cb(page, total_emitted, user_ctx);
                free_rendered_page(page);
                if (!keep_going) {
                    aborted = true;
                    break;
                }
            }
        }

        if (aborted) {
            kill(gs_pid, SIGKILL);
        }
        fclose(gs_pipe);
        waitpid(gs_pid, NULL, 0);

        if (aborted)
            break;
    }

    if (temp_ps[0])
        unlink(temp_ps);

    return total_emitted;
}

int generate_test_document_ps(const char *out_ps_path, int num_pages, const struct lbp2900_prefs *prefs) {
    if (num_pages < 1)
        num_pages = 1;
    FILE *f = fopen(out_ps_path, "w");
    if (!f)
        return -1;

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(f, "%%!PS-Adobe-3.0\n");
    fprintf(f, "%%%%Title: Canon LASER SHOT LBP-2900 Test Page\n");
    fprintf(f, "%%%%Pages: %d\n", num_pages);
    fprintf(f, "%%%%EndComments\n");

    for (int p = 1; p <= num_pages; ++p) {
        fprintf(f, "%%%%Page: %d %d\n", p, p);
        /* Outer border frame */
        fprintf(f, "1.5 setlinewidth 28 28 539 786 rectstroke\n");
        fprintf(f, "0.5 setlinewidth 32 32 531 778 rectstroke\n");

        /* Header Banner */
        fprintf(f, "0.12 setgray 42 735 511 58 rectfill\n");
        fprintf(f, "1 setgray /Helvetica-Bold findfont 20 scalefont setfont\n");
        fprintf(f, "56 766 moveto (Canon LASER SHOT LBP-2900) show\n");
        fprintf(f, "/Helvetica findfont 11 scalefont setfont\n");
        fprintf(f, "56 746 moveto (CAPT v1.0 Standalone Host-Rendered Driver  |  CUPS-Free Direct USB Engine) show\n");

        /* Page Badge */
        fprintf(f, "0 setgray /Helvetica-Bold findfont 14 scalefont setfont\n");
        fprintf(f, "42 705 moveto (Printer Verification & Diagnostic Test  -  Page %d of %d) show\n", p, num_pages);

        /* Divider line */
        fprintf(f, "1 setlinewidth 42 695 moveto 553 695 lineto stroke\n");

        /* Technical Specifications Table */
        fprintf(f, "/Helvetica-Bold findfont 11 scalefont setfont\n");
        fprintf(f, "42 672 moveto (1. Driver & Hardware Configuration) show\n");
        fprintf(f, "/Courier findfont 10 scalefont setfont\n");
        fprintf(f, "52 652 moveto (Printer Model       : Canon LASER SHOT LBP-2900 (USB 04a9:2676)) show\n");
        fprintf(f, "52 637 moveto (Protocol            : Canon Advanced Printing Technology (CAPT v1.0 / Hi-SCoA)) show\n");
        fprintf(f, "52 622 moveto (Resolution          : 600 x 600 DPI (1-bit Monochrome Host-Side Rendering)) show\n");
        fprintf(f, "52 607 moveto (Memory Optimization : 70-line Band Streaming + 15-Slot BufLevel Flow Control) show\n");
        fprintf(f, "52 592 moveto (Paper Size / Media  : %s  |  Density: %d/15  |  Toner Save: %s) show\n",
                (prefs && prefs->media_size[0]) ? prefs->media_size : "A4",
                prefs ? prefs->density : 7,
                (prefs && prefs->toner_save) ? "ON" : "OFF");
        fprintf(f, "52 577 moveto (Timestamp           : %s  |  Page Sequence #%d) show\n", time_str, p);

        /* Halftone / Grayscale Ramp (Tests Hi-SCoA compression & Toner Density) */
        fprintf(f, "/Helvetica-Bold findfont 11 scalefont setfont\n");
        fprintf(f, "42 545 moveto (2. Halftone Grayscale Ramp & Hi-SCoA Band Compression Check) show\n");
        for (int i = 0; i <= 10; ++i) {
            double g = 1.0 - (i / 10.0);
            int x = 42 + i * 46;
            fprintf(f, "%.2f setgray %d 490 42 40 rectfill\n", g, x);
            fprintf(f, "0 setgray 0.5 setlinewidth %d 490 42 40 rectstroke\n", x);
            fprintf(f, "/Helvetica findfont 8 scalefont setfont %d 478 moveto (%d%%) show\n", x + 10, i * 10);
        }

        /* Radial Star Resolution Target & Fine Line Check */
        fprintf(f, "0 setgray /Helvetica-Bold findfont 11 scalefont setfont\n");
        fprintf(f, "42 445 moveto (3. 600 DPI Laser Precision Target & Typography Scale) show\n");
        fprintf(f, "gsave 130 345 translate 0.35 setlinewidth\n");
        fprintf(f, "0 5 355 { gsave rotate 0 0 moveto 75 0 lineto stroke grestore } for\n");
        fprintf(f, "1 setgray 0 0 12 0 360 arc fill 0 setgray 0 0 75 0 360 arc stroke grestore\n");

        /* Typography Scale */
        int ty = 415;
        const int sizes[] = { 6, 8, 10, 12, 14, 18 };
        for (size_t s = 0; s < ARRAY_SIZE(sizes); ++s) {
            fprintf(f, "/Times-Roman findfont %d scalefont setfont\n", sizes[s]);
            fprintf(f, "235 %d moveto (%d pt: The quick brown fox jumps over the lazy dog 0123456789) show\n",
                    ty, sizes[s]);
            ty -= (sizes[s] + 8);
        }

        /* Multi-page / Out-of-Paper verification box */
        fprintf(f, "0.94 setgray 42 120 511 115 rectfill\n");
        fprintf(f, "0 setgray 0.8 setlinewidth 42 120 511 115 rectstroke\n");
        fprintf(f, "/Helvetica-Bold findfont 12 scalefont setfont\n");
        fprintf(f, "56 212 moveto (Multi-Page Pipeline & Out-of-Paper Recovery Verification) show\n");
        fprintf(f, "/Helvetica findfont 10 scalefont setfont\n");
        fprintf(f, "56 192 moveto (- Host computer renders 600 DPI bands in < 3 MB RAM to protect the printer's 2 MB buffer.) show\n");
        fprintf(f, "56 176 moveto (- Persistent USB session prevents the job-5 ReserveUnit (0x8c) engine lockup.) show\n");
        fprintf(f, "56 160 moveto (- Out-of-Paper state pauses printing, alerts the user, and waits for the physical) show\n");
        fprintf(f, "56 144 moveto (  Paper/Resume button on the Canon LBP-2900 before reprinting the affected page.) show\n");

        /* Big Page Number Watermark in Bottom Right */
        fprintf(f, "/Helvetica-Bold findfont 36 scalefont setfont\n");
        fprintf(f, "380 56 moveto (PAGE %d / %d) show\n", p, num_pages);
        fprintf(f, "/Helvetica findfont 9 scalefont setfont\n");
        fprintf(f, "42 56 moveto (Canon LBP-2900 CAPT Standalone Suite on Debian GNU/Linux) show\n");

        fprintf(f, "showpage\n");
    }

    fprintf(f, "%%%%EOF\n");
    fclose(f);
    return 0;
}
