#include "gs2print.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failed = 0;

void expect(bool ok, const char *name) {
    if (ok) {
        std::printf("ok  %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++g_failed;
    }
}

struct Box {
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool any = false;
};

Box bounds(gs2_job *job, int page) {
    Box box;
    int w = 0, h = 0, stride = 0;
    const uint8_t *bits = gs2_job_page_bits(job, page, &w, &h, &stride);
    if (!bits) return box;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = bits + static_cast<size_t>(y) * stride;
        for (int byte = 0; byte < stride; ++byte) {
            uint8_t value = row[byte];
            if (!value) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if (!(value & static_cast<uint8_t>(0x80 >> bit))) continue;
                int x = byte * 8 + bit;
                if (x >= w) continue;
                if (!box.any) {
                    box.minX = box.maxX = x;
                    box.minY = box.maxY = y;
                    box.any = true;
                } else {
                    if (x < box.minX) box.minX = x;
                    if (x > box.maxX) box.maxX = x;
                    if (y < box.minY) box.minY = y;
                    if (y > box.maxY) box.maxY = y;
                }
            }
        }
    }
    return box;
}

bool inkAt(gs2_job *job, int page, int x, int y) {
    int w = 0, h = 0, stride = 0;
    const uint8_t *bits = gs2_job_page_bits(job, page, &w, &h, &stride);
    if (!bits || x < 0 || y < 0 || x >= w || y >= h) return false;
    const uint8_t *row = bits + static_cast<size_t>(y) * stride;
    return (row[x >> 3] & static_cast<uint8_t>(0x80 >> (x & 7))) != 0;
}

void writeStr(gs2_job *job, const char *s) {
    gs2_job_write(job, reinterpret_cast<const uint8_t *>(s), std::strlen(s));
}

void testTextAndCrLf() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "A\r\nB");
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    // The head starts at the top of the form. Draft A's apex is wire 1.
    expect(gs2_job_page_count(job) == 1, "text page count");
    expect(box.any && box.minY == 0, "draft A sits on the first baseline");
    expect(box.minX == 180, "printable area is centered on the page");
    // CR+LF feeds 1/6 inch once. At 720 dpi that is 120 pixels, where B starts.
    expect(inkAt(job, 0, box.minX, 120), "CR+LF feeds once");
    expect(box.maxY < 225, "CR+LF does not feed a second line");
    gs2_job_free(job);
}

void testEscT() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "\x1bT16\nA");
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    // 16/144 inch is 80 pixels at 720 dpi. A's apex is the top wire.
    expect(box.any && box.minY == 80, "ESC T 16 feeds 16/144 inch");
    gs2_job_free(job);
}

void testGraphics() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "\x1bG0008");
    const uint8_t cols[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    gs2_job_write(job, cols, 8);
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    // Pica graphics is 80 dpi: 18 grid units per column, stored as 9 pixels.
    expect(box.any, "graphics leaves ink");
    expect(box.minY == 0, "graphics bit 0 is the top wire");
    expect(box.maxX - box.minX + 1 == 8 * 9, "graphics uses the pitch density, not text cells");
    expect(box.maxY - box.minY + 1 == 8 * 10, "graphics uses eight pins at 1/72 inch");
    gs2_job_free(job);
}

void testReverse() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "\n\nA\x1br\nB");
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    // Two forward feeds put A at 240 pixels (ink at 270). Reverse LF then B at 120.
    expect(box.any && box.minY == 120, "reverse feed draws above the previous line");
    expect(inkAt(job, 0, 180, 270), "the earlier line stays on the page");
    gs2_job_free(job);
}

void testFormFeedAndPdf() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "A\fB");
    expect(gs2_job_page_count(job) == 2, "form feed keeps the open page");
    expect(bounds(job, 0).any, "committed page keeps its ink");
    expect(bounds(job, 1).any, "open page keeps its ink");
    expect(bounds(job, 0).any, "committed page reads back again");
    gs2_job_finish(job);
    expect(gs2_job_page_count(job) == 2, "form feed yields two pages");
    expect(bounds(job, 0).any && bounds(job, 1).any, "each page has ink");

    const char *path = "/tmp/gs2print-test.pdf";
    std::remove(path);
    expect(gs2_job_save_pdf(job, path) == 0, "pdf saves");
    FILE *f = std::fopen(path, "rb");
    char mag[5] = {};
    bool pdf = f && std::fread(mag, 1, 4, f) == 4 && std::memcmp(mag, "%PDF", 4) == 0;
    if (f) std::fclose(f);
    expect(pdf, "pdf header");
    FILE *whole = std::fopen(path, "rb");
    std::string body;
    if (whole) {
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, whole)) > 0) body.append(buf, n);
        std::fclose(whole);
    }
    expect(body.find("/FlateDecode") != std::string::npos && body.find("/Predictor 12") != std::string::npos
               && body.find("/CCITTFaxDecode") == std::string::npos,
           "pdf image is a 1-bit Flate picture with predictor 12");
    gs2_job_free(job);
}

void testHeader() {
    const char *saved = "GS2PRINT 1\nprinter imagewriter-ii\n\nA";
    size_t n = std::strlen(saved);
    size_t off = gs2_dump_payload_offset(reinterpret_cast<const uint8_t *>(saved), n);
    expect(off == n - 1 && saved[off] == 'A', "saved dump header is stripped");

    const char *raw = "GS2PRINT 1 is not a header\nA";
    size_t rawOff = gs2_dump_payload_offset(reinterpret_cast<const uint8_t *>(raw), std::strlen(raw));
    expect(rawOff == 0, "raw file is not stripped");

    gs2_job *job = gs2_job_new();
    auto *bytes = reinterpret_cast<const uint8_t *>(saved);
    gs2_job_write(job, bytes + off, n - off);
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    expect(box.any && box.minY == 0 && box.maxY < 100, "header payload prints as one character");
    gs2_job_free(job);
}

void testSplitCommands() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "\x1bT1");
    Box early = bounds(job, 0);
    expect(!early.any, "ESC T 1 returns before the command completes");
    writeStr(job, "6\nA");
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    expect(box.any && box.minY == 80, "the rest of ESC T 16 applies on the next write");
    gs2_job_free(job);

    gs2_job *gfx = gs2_job_new();
    writeStr(gfx, "\x1bG000");
    expect(!bounds(gfx, 0).any, "ESC G count returns before its data");
    writeStr(gfx, "2");
    const uint8_t cols[2] = {0xFF, 0xFF};
    gs2_job_write(gfx, cols, 2);
    gs2_job_finish(gfx);
    Box gbox = bounds(gfx, 0);
    expect(gbox.any && gbox.maxX - gbox.minX + 1 == 2 * 9, "split ESC G prints the columns it was given");
    gs2_job_free(gfx);

    gs2_job *drop = gs2_job_new();
    writeStr(drop, "A\x1bT1");
    gs2_job_finish(drop);
    Box dbox = bounds(drop, 0);
    expect(gs2_job_page_count(drop) == 1 && dbox.any && dbox.minY == 0 && dbox.maxY < 120,
           "finish abandons a partial command and still commits the page");
    gs2_job_free(drop);
}

}  // namespace

int main() {
    testTextAndCrLf();
    testEscT();
    testGraphics();
    testReverse();
    testFormFeedAndPdf();
    testHeader();
    testSplitCommands();
    if (g_failed) {
        std::printf("%d failed\n", g_failed);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
