#include "gs2print.h"
#include "imagewriter.hpp"
#include "page.hpp"

#include "hpdf.h"

#include <cstdint>
#include <cstring>
#include <setjmp.h>
#include <vector>

struct PdfJump {
    jmp_buf env;
    HPDF_Doc doc;
};

extern "C" void gs2_pdf_error(HPDF_STATUS error_no, HPDF_STATUS detail_no, void *user) {
    (void)error_no;
    (void)detail_no;
    auto *jump = static_cast<PdfJump *>(user);
    longjmp(jump->env, 1);
}

struct gs2_job : Paper {
    Page open;
    std::vector<Page> pages;
    std::vector<uint8_t> scratch;
    bool dirty = false;
    bool countOpen = true;
    ImageWriter iw;

    gs2_job() : iw(this) {}

    void ink(int x, int y, int w, int h) override {
        open.fill(x + Page::kOriginX, y, w, h);
        if (w > 0 && h > 0) dirty = true;
    }

    void formFeed() override {
        pages.push_back(std::move(open));
        pages.back().compress();
        open = Page();
        dirty = false;
    }
};

gs2_job *gs2_job_new(void) { return new gs2_job(); }

void gs2_job_free(gs2_job *job) { delete job; }

void gs2_job_write(gs2_job *job, const uint8_t *bytes, size_t len) {
    if (!job) return;
    job->countOpen = true;
    job->iw.write(bytes, len);
}

void gs2_job_finish(gs2_job *job) {
    if (!job) return;
    job->iw.finish();
    if (job->dirty || job->pages.empty()) {
        job->pages.push_back(std::move(job->open));
        job->pages.back().compress();
        job->open = Page();
        job->dirty = false;
    }
    job->countOpen = false;
}

int gs2_job_page_count(gs2_job *job) {
    if (!job) return 0;
    return static_cast<int>(job->pages.size()) + (job->countOpen ? 1 : 0);
}

const uint8_t *gs2_job_page_bits(gs2_job *job, int page, int *width, int *height, int *stride) {
    if (!job || page < 0) return nullptr;
    job->iw.sync();
    const Page *src = nullptr;
    if (page < static_cast<int>(job->pages.size())) src = &job->pages[page];
    else if (job->countOpen && page == static_cast<int>(job->pages.size())) src = &job->open;
    if (!src) return nullptr;
    const uint8_t *bits = src->bits.data();
    if (src->bits.empty()) {
        if (!src->inflateInto(job->scratch)) return nullptr;
        bits = job->scratch.data();
    }
    if (width) *width = Page::kWidth;
    if (height) *height = Page::kHeight;
    if (stride) *stride = Page::kStride;
    return bits;
}

static HPDF_Image loadFlateImage(HPDF_Doc pdf, const uint8_t *bits, int width, int height) {
    std::vector<uint8_t> filtered;
    Page::encodeUp(bits, filtered);
    if (filtered.size() > static_cast<size_t>(UINT32_MAX)) return nullptr;

    HPDF_Dict image = HPDF_DictStream_New(pdf->mmgr, pdf->xref);
    if (!image) return nullptr;
    image->header.obj_class |= HPDF_OSUBCLASS_XOBJECT;

    HPDF_STATUS ret = HPDF_OK;
    ret += HPDF_Dict_AddName(image, "Type", "XObject");
    ret += HPDF_Dict_AddName(image, "Subtype", "Image");
    ret += HPDF_Dict_AddName(image, "ColorSpace", "DeviceGray");
    if (ret != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image, "Width", width) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image, "Height", height) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image, "BitsPerComponent", 1) != HPDF_OK) return nullptr;

    // Ink bits are 1. DeviceGray paints 1 as white, so swap the samples.
    HPDF_Array decode = HPDF_Array_New(pdf->mmgr);
    if (!decode) return nullptr;
    if (HPDF_Array_AddNumber(decode, 1) != HPDF_OK) return nullptr;
    if (HPDF_Array_AddNumber(decode, 0) != HPDF_OK) return nullptr;
    if (HPDF_Dict_Add(image, "Decode", decode) != HPDF_OK) return nullptr;

    if (HPDF_Stream_Write(image->stream, filtered.data(),
                          static_cast<HPDF_UINT>(filtered.size())) != HPDF_OK)
        return nullptr;

    image->filter = HPDF_STREAM_FILTER_FLATE_DECODE;
    image->filterParams = HPDF_Dict_New(pdf->mmgr);
    if (!image->filterParams) return nullptr;
    if (HPDF_Dict_AddNumber(image->filterParams, "Predictor", 12) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image->filterParams, "Columns", width) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image->filterParams, "Colors", 1) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image->filterParams, "BitsPerComponent", 1) != HPDF_OK) return nullptr;
    return image;
}

int gs2_job_save_pdf(gs2_job *job, const char *path) {
    if (!job || !path) return -1;
    int count = gs2_job_page_count(job);
    if (count <= 0) return -1;

    PdfJump jump;
    jump.doc = nullptr;
    if (setjmp(jump.env)) {
        if (jump.doc) HPDF_Free(jump.doc);
        return -1;
    }

    jump.doc = HPDF_New(gs2_pdf_error, &jump);
    if (!jump.doc) return -1;

    const float pageW = 8.5f * 72.0f;
    const float pageH = 11.0f * 72.0f;
    for (int i = 0; i < count; ++i) {
        int w = 0, h = 0, stride = 0;
        const uint8_t *bits = gs2_job_page_bits(job, i, &w, &h, &stride);
        if (!bits) {
            HPDF_Free(jump.doc);
            return -1;
        }
        HPDF_Page page = HPDF_AddPage(jump.doc);
        HPDF_Page_SetWidth(page, pageW);
        HPDF_Page_SetHeight(page, pageH);
        // Predictor rows go in uncompressed. libHaru deflates them on save.
        HPDF_Image image = loadFlateImage(jump.doc, bits, w, h);
        if (!image) {
            HPDF_Free(jump.doc);
            return -1;
        }
        HPDF_Page_DrawImage(page, image, 0, 0, pageW, pageH);
    }

    HPDF_STATUS saved = HPDF_SaveToFile(jump.doc, path);
    HPDF_Free(jump.doc);
    return saved == HPDF_OK ? 0 : -1;
}

size_t gs2_dump_payload_offset(const uint8_t *bytes, size_t len) {
    static const char magic[] = "GS2PRINT 1";
    const size_t mlen = sizeof(magic) - 1;
    if (!bytes || len < mlen + 1) return 0;
    if (std::memcmp(bytes, magic, mlen) != 0) return 0;
    size_t i = mlen;
    if (bytes[i] == '\r') ++i;
    if (i >= len || bytes[i] != '\n') return 0;
    ++i;
    while (i < len) {
        if (bytes[i] == '\n') return i + 1;
        if (bytes[i] == '\r') {
            ++i;
            if (i < len && bytes[i] == '\n') ++i;
            return i;
        }
        while (i < len && bytes[i] != '\n' && bytes[i] != '\r') ++i;
        if (i >= len) return 0;
        if (bytes[i] == '\r') ++i;
        if (i < len && bytes[i] == '\n') ++i;
    }
    return 0;
}
