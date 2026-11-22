// MuPDF reports errors with setjmp/longjmp (fz_try/fz_catch), which skips
// C++ destructors. So this file keeps to plain C types: nothing with a
// destructor may live in a scope that fz_try can jump out of.

#include "redact.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <cstdio>

namespace {
void quiet(void *, const char *)
{
}
} // namespace

bool redactText(const char *in, const char *password, int page, const double *rects, int count, const char *out, char *error,
                std::size_t errorSize)
{
    fz_context *ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        std::snprintf(error, errorSize, "Cannot start MuPDF.");
        return false;
    }
    fz_set_warning_callback(ctx, quiet, nullptr);
    fz_set_error_callback(ctx, quiet, nullptr);

    pdf_document *doc = nullptr;
    pdf_page *pg = nullptr;
    bool ok = false;
    fz_var(doc);
    fz_var(pg);
    fz_var(ok);
    fz_try(ctx)
    {
        doc = pdf_open_document(ctx, in);
        if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : "")) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "wrong password");
        }
        if (page < 0 || page >= pdf_count_pages(ctx, doc)) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "no page %d", page + 1);
        }
        pg = pdf_load_page(ctx, doc, page);
        const fz_rect bounds = fz_bound_page(ctx, reinterpret_cast<fz_page *>(pg));
        const float w = bounds.x1 - bounds.x0, h = bounds.y1 - bounds.y0;

        pdf_annot *annot = pdf_create_annot(ctx, pg, PDF_ANNOT_REDACT);
        fz_rect all = fz_empty_rect;
        for (int i = 0; i < count; ++i) {
            const double *r = rects + 4 * i;
            const fz_rect q = fz_make_rect(float(bounds.x0 + r[0] * w), float(bounds.y0 + r[1] * h), float(bounds.x0 + (r[0] + r[2]) * w),
                                           float(bounds.y0 + (r[1] + r[3]) * h));
            pdf_add_annot_quad_point(ctx, annot, fz_quad_from_rect(q));
            all = fz_union_rect(all, q);
        }
        pdf_set_annot_rect(ctx, annot, all);

        pdf_redact_options opts{};
        opts.black_boxes = 0;
        opts.image_method = PDF_REDACT_IMAGE_NONE;
        opts.line_art = PDF_REDACT_LINE_ART_NONE;
        opts.text = PDF_REDACT_TEXT_REMOVE;
        pdf_apply_redaction(ctx, annot, &opts);

        pdf_write_options wopts = pdf_default_write_options;
        pdf_save_document(ctx, doc, out, &wopts);
        ok = true;
    }
    fz_always(ctx)
    {
        pdf_drop_page(ctx, pg);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        std::snprintf(error, errorSize, "%s", fz_caught_message(ctx));
    }
    fz_drop_context(ctx);
    return ok;
}
