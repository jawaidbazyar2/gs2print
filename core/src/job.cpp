#include "gs2print.h"
#include "imagewriter.hpp"
#include "page.hpp"

#include "hpdf.h"

#include <algorithm>
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

    void ink(int x, int y, int w, int h, unsigned planes) override {
        open.fill(x + Page::kOriginX, y, w, h, planes);
        if (w > 0 && h > 0 && planes) dirty = true;
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

void gs2_job_set_auto_lf(gs2_job *job, int on) {
    if (!job) return;
    job->iw.setAutoLineFeed(on != 0);
}

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

const uint8_t *gs2_job_page_bits(gs2_job *job, int page, int plane,
                                 int *width, int *height, int *stride) {
    if (!job || page < 0 || plane < 0 || plane >= Page::kPlanes) return nullptr;
    job->iw.sync();
    const Page *src = nullptr;
    if (page < static_cast<int>(job->pages.size())) src = &job->pages[page];
    else if (job->countOpen && page == static_cast<int>(job->pages.size())) src = &job->open;
    if (!src) return nullptr;
    const uint8_t *bits = src->bits[plane].data();
    if (src->bits[plane].empty()) {
        if (!src->inflateInto(plane, job->scratch)) return nullptr;
        bits = job->scratch.data();
    }
    if (width) *width = Page::kWidth;
    if (height) *height = Page::kHeight;
    if (stride) *stride = Page::kStride;
    return bits;
}

void gs2_planes_composite(const uint8_t *const planes[4],
                          int width, int height, int stride,
                          uint8_t *rgb) {
    if (!planes || !rgb || width <= 0 || height <= 0 || stride <= 0) return;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::memset(rgb, 255, pixels * 3);
    static const uint8_t kInk[Page::kPlanes][3] = {
        {255, 255, 0},
        {255, 0, 255},
        {0, 255, 255},
        {0, 0, 0},
    };
    const int packed = std::min(stride, (width + 7) / 8);
    for (int p = 0; p < Page::kPlanes; ++p) {
        const uint8_t *src = planes[p];
        if (!src) continue;
        const uint8_t ir = kInk[p][0];
        const uint8_t ig = kInk[p][1];
        const uint8_t ib = kInk[p][2];
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = src + static_cast<size_t>(y) * static_cast<size_t>(stride);
            uint8_t *dest = rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
            for (int byte = 0; byte < packed; ++byte) {
                uint8_t value = row[byte];
                if (!value) continue;
                int x0 = byte * 8;
                int n = std::min(8, width - x0);
                uint8_t mask = 0x80;
                for (int bit = 0; bit < n; ++bit) {
                    if (value & mask) {
                        uint8_t *px = dest + (x0 + bit) * 3;
                        px[0] = static_cast<uint8_t>(static_cast<unsigned>(px[0]) * ir / 255u);
                        px[1] = static_cast<uint8_t>(static_cast<unsigned>(px[1]) * ig / 255u);
                        px[2] = static_cast<uint8_t>(static_cast<unsigned>(px[2]) * ib / 255u);
                    }
                    mask = static_cast<uint8_t>(mask >> 1);
                }
            }
        }
    }
}

namespace {

const char *kPlaneNames[Page::kPlanes] = {"Yellow", "Magenta", "Cyan", "Black"};
const float kPlaneRgb[Page::kPlanes][3] = {
    {1.0f, 1.0f, 0.0f},
    {1.0f, 0.0f, 1.0f},
    {0.0f, 1.0f, 1.0f},
    {0.0f, 0.0f, 0.0f},
};

HPDF_Image loadFlateMask(HPDF_Doc pdf, const uint8_t *bits, int width, int height,
                         HPDF_Dict ocg) {
    std::vector<uint8_t> filtered;
    Page::encodeUp(bits, filtered);
    if (filtered.size() > static_cast<size_t>(UINT32_MAX)) return nullptr;

    HPDF_Dict image = HPDF_DictStream_New(pdf->mmgr, pdf->xref);
    if (!image) return nullptr;
    image->header.obj_class |= HPDF_OSUBCLASS_XOBJECT;

    HPDF_STATUS ret = HPDF_OK;
    ret += HPDF_Dict_AddName(image, "Type", "XObject");
    ret += HPDF_Dict_AddName(image, "Subtype", "Image");
    if (ret != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image, "Width", width) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image, "Height", height) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddNumber(image, "BitsPerComponent", 1) != HPDF_OK) return nullptr;
    if (HPDF_Dict_AddBoolean(image, "ImageMask", HPDF_TRUE) != HPDF_OK) return nullptr;

    // Ink bits are 1. ImageMask paints 0, so swap the samples.
    HPDF_Array decode = HPDF_Array_New(pdf->mmgr);
    if (!decode) return nullptr;
    if (HPDF_Array_AddNumber(decode, 1) != HPDF_OK) return nullptr;
    if (HPDF_Array_AddNumber(decode, 0) != HPDF_OK) return nullptr;
    if (HPDF_Dict_Add(image, "Decode", decode) != HPDF_OK) return nullptr;
    if (ocg && HPDF_Dict_Add(image, "OC", ocg) != HPDF_OK) return nullptr;

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

bool addOcgs(HPDF_Doc pdf, HPDF_Dict ocgs[Page::kPlanes]) {
    HPDF_Array list = HPDF_Array_New(pdf->mmgr);
    HPDF_Array order = HPDF_Array_New(pdf->mmgr);
    HPDF_Array on = HPDF_Array_New(pdf->mmgr);
    if (!list || !order || !on) return false;
    for (int p = 0; p < Page::kPlanes; ++p) {
        HPDF_Dict ocg = HPDF_Dict_New(pdf->mmgr);
        if (!ocg) return false;
        if (HPDF_Xref_Add(pdf->xref, ocg) != HPDF_OK) return false;
        if (HPDF_Dict_AddName(ocg, "Type", "OCG") != HPDF_OK) return false;
        HPDF_String name = HPDF_String_New(pdf->mmgr, kPlaneNames[p], nullptr);
        if (!name) return false;
        if (HPDF_Dict_Add(ocg, "Name", name) != HPDF_OK) return false;
        if (HPDF_Array_Add(list, ocg) != HPDF_OK) return false;
        if (HPDF_Array_Add(order, ocg) != HPDF_OK) return false;
        if (HPDF_Array_Add(on, ocg) != HPDF_OK) return false;
        ocgs[p] = ocg;
    }
    HPDF_Dict d = HPDF_Dict_New(pdf->mmgr);
    if (!d) return false;
    if (HPDF_Dict_Add(d, "Order", order) != HPDF_OK) return false;
    if (HPDF_Dict_Add(d, "ON", on) != HPDF_OK) return false;
    HPDF_Dict props = HPDF_Dict_New(pdf->mmgr);
    if (!props) return false;
    if (HPDF_Dict_Add(props, "OCGs", list) != HPDF_OK) return false;
    if (HPDF_Dict_Add(props, "D", d) != HPDF_OK) return false;
    return HPDF_Dict_Add(pdf->catalog, "OCProperties", props) == HPDF_OK;
}

}  // namespace

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
    if (jump.doc->pdf_version < HPDF_VER_15) jump.doc->pdf_version = HPDF_VER_15;

    HPDF_Dict ocgs[Page::kPlanes] = {};
    if (!addOcgs(jump.doc, ocgs)) {
        HPDF_Free(jump.doc);
        return -1;
    }

    HPDF_ExtGState multiply = HPDF_CreateExtGState(jump.doc);
    if (!multiply) {
        HPDF_Free(jump.doc);
        return -1;
    }
    if (HPDF_ExtGState_SetBlendMode(multiply, HPDF_BM_MULTIPLY) != HPDF_OK) {
        HPDF_Free(jump.doc);
        return -1;
    }

    const float pageW = 8.5f * 72.0f;
    const float pageH = 11.0f * 72.0f;
    for (int i = 0; i < count; ++i) {
        HPDF_Page page = HPDF_AddPage(jump.doc);
        HPDF_Page_SetWidth(page, pageW);
        HPDF_Page_SetHeight(page, pageH);
        if (HPDF_Page_SetExtGState(page, multiply) != HPDF_OK) {
            HPDF_Free(jump.doc);
            return -1;
        }
        for (int p = 0; p < Page::kPlanes; ++p) {
            int w = 0, h = 0, stride = 0;
            const uint8_t *bits = gs2_job_page_bits(job, i, p, &w, &h, &stride);
            if (!bits) {
                HPDF_Free(jump.doc);
                return -1;
            }
            // Predictor rows go in uncompressed. libHaru deflates them on save.
            HPDF_Image image = loadFlateMask(jump.doc, bits, w, h, ocgs[p]);
            if (!image) {
                HPDF_Free(jump.doc);
                return -1;
            }
            if (HPDF_Page_SetRGBFill(page, kPlaneRgb[p][0], kPlaneRgb[p][1],
                                     kPlaneRgb[p][2]) != HPDF_OK) {
                HPDF_Free(jump.doc);
                return -1;
            }
            HPDF_Page_DrawImage(page, image, 0, 0, pageW, pageH);
        }
    }

    HPDF_STATUS saved = HPDF_SaveToFile(jump.doc, path);
    HPDF_Free(jump.doc);
    return saved == HPDF_OK ? 0 : -1;
}

namespace {

struct HeaderScan {
    size_t payload = 0;
    int field = 0;
};

bool headerMagic(const uint8_t *bytes, size_t len, size_t *index) {
    static const char magic[] = "GS2PRINT 1";
    const size_t mlen = sizeof(magic) - 1;
    if (!bytes || len < mlen + 1) return false;
    if (std::memcmp(bytes, magic, mlen) != 0) return false;
    size_t i = mlen;
    if (bytes[i] == '\r') ++i;
    if (i >= len || bytes[i] != '\n') return false;
    *index = i + 1;
    return true;
}

// A header line ends at CR or LF. A blank line ends the header.
// False means the line never reached a newline.
bool headerLine(const uint8_t *bytes, size_t len, size_t *index,
                const uint8_t **line, size_t *lineLen, bool *blank) {
    size_t i = *index;
    if (i >= len) return false;
    if (bytes[i] == '\n') {
        *blank = true;
        *line = bytes + i;
        *lineLen = 0;
        *index = i + 1;
        return true;
    }
    if (bytes[i] == '\r') {
        *blank = true;
        *line = bytes + i;
        *lineLen = 0;
        ++i;
        if (i < len && bytes[i] == '\n') ++i;
        *index = i;
        return true;
    }
    size_t start = i;
    while (i < len && bytes[i] != '\n' && bytes[i] != '\r') ++i;
    if (i >= len) return false;
    *blank = false;
    *line = bytes + start;
    *lineLen = i - start;
    if (bytes[i] == '\r') ++i;
    if (i < len && bytes[i] == '\n') ++i;
    *index = i;
    return true;
}

HeaderScan scanHeader(const uint8_t *bytes, size_t len, const char *key, char *buf, size_t cap) {
    HeaderScan scan;
    size_t i = 0;
    if (!headerMagic(bytes, len, &i)) return scan;
    const size_t keyLen = key ? std::strlen(key) : 0;
    while (i < len) {
        const uint8_t *line = nullptr;
        size_t lineLen = 0;
        bool blank = false;
        if (!headerLine(bytes, len, &i, &line, &lineLen, &blank)) return HeaderScan{};
        if (blank) {
            scan.payload = i;
            return scan;
        }
        if (!key || keyLen == 0 || scan.field != 0) continue;
        if (lineLen <= keyLen || std::memcmp(line, key, keyLen) != 0 || line[keyLen] != ' ') continue;
        size_t vlen = lineLen - keyLen - 1;
        if (!buf || vlen >= cap) {
            scan.field = -1;
            continue;
        }
        std::memcpy(buf, line + keyLen + 1, vlen);
        buf[vlen] = 0;
        scan.field = 1;
    }
    return HeaderScan{};
}

}  // namespace

size_t gs2_dump_payload_offset(const uint8_t *bytes, size_t len) {
    return scanHeader(bytes, len, nullptr, nullptr, 0).payload;
}

int gs2_ticket_field(const uint8_t *bytes, size_t len, const char *key, char *buf, size_t cap) {
    if (!key || !buf || cap == 0) return 0;
    buf[0] = 0;
    HeaderScan scan = scanHeader(bytes, len, key, buf, cap);
    if (scan.payload == 0) return 0;
    return scan.field;
}
