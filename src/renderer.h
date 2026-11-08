/*
 * Computer-Assisted Host Renderer for Canon LASER SHOT LBP-2900
 * Offloads rasterization & Hi-SCoA band compression to the computer CPU
 * so the printer's 2MB onboard RAM is never overwhelmed.
 */

#ifndef CAPT_RENDERER_H
#define CAPT_RENDERER_H

#include "capt_defs.h"

typedef bool (*page_ready_callback_fn)(struct rendered_page_s *page, int page_index, void *user_ctx);

/*
 * Streams a document file (PDF, PostScript, CUPS Raster, PNG, JPEG, PBM, or Text)
 * page-by-page at 600x600 DPI 1-bit monochrome, compresses each page into 70-line
 * Hi-SCoA bands, and invokes `cb(page, page_idx, user_ctx)` for each page.
 *
 * The callback takes ownership of `page` (or `render_document_streaming` frees it
 * after `cb` returns). Returns total pages rendered, or -1 on error.
 */
int render_document_streaming(const char *filepath,
                              const struct lbp2900_prefs *prefs,
                              page_ready_callback_fn cb,
                              void *user_ctx);

/*
 * Generates a PostScript file for a Windows-style Canon LBP-2900 Printer Test Page
 * (if num_pages == 1) or a Multi-Page Continuous Pipeline Test Document (if num_pages > 1).
 */
int generate_test_document_ps(const char *out_ps_path, int num_pages, const struct lbp2900_prefs *prefs);

#endif /* CAPT_RENDERER_H */
