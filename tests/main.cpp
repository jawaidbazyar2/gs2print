#include "gs2print.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

void mergeBox(Box &into, const Box &b) {
    if (!b.any) return;
    if (!into.any) {
        into = b;
        return;
    }
    if (b.minX < into.minX) into.minX = b.minX;
    if (b.maxX > into.maxX) into.maxX = b.maxX;
    if (b.minY < into.minY) into.minY = b.minY;
    if (b.maxY > into.maxY) into.maxY = b.maxY;
}

Box boundsPlane(gs2_job *job, int page, int plane) {
    Box box;
    int w = 0, h = 0, stride = 0;
    const uint8_t *bits = gs2_job_page_bits(job, page, plane, &w, &h, &stride);
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

Box bounds(gs2_job *job, int page) {
    Box box;
    for (int plane = 0; plane < 4; ++plane) mergeBox(box, boundsPlane(job, page, plane));
    return box;
}

bool inkAtPlane(gs2_job *job, int page, int plane, int x, int y) {
    int w = 0, h = 0, stride = 0;
    const uint8_t *bits = gs2_job_page_bits(job, page, plane, &w, &h, &stride);
    if (!bits || x < 0 || y < 0 || x >= w || y >= h) return false;
    const uint8_t *row = bits + static_cast<size_t>(y) * stride;
    return (row[x >> 3] & static_cast<uint8_t>(0x80 >> (x & 7))) != 0;
}

bool inkAt(gs2_job *job, int page, int x, int y) {
    for (int plane = 0; plane < 4; ++plane) {
        if (inkAtPlane(job, page, plane, x, y)) return true;
    }
    return false;
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

void testCrOverstrike() {
    gs2_job *job = gs2_job_new();
    writeStr(job, "A\rB");
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    // Factory SW1-8 is open, so CR returns the head and the next glyph
    // reprints the same line.
    expect(box.any && box.minY == 0, "CR leaves the line at the top");
    expect(box.maxY < 110, "CR does not feed");
    expect(box.maxX - box.minX + 1 < 80, "CR returns the head before the next glyph");
    gs2_job_free(job);

    gs2_job *fed = gs2_job_new();
    const uint8_t on[] = {0x1B, 'D', 0x80, 0x00, 'A', '\r', 'B'};
    gs2_job_write(fed, on, sizeof on);
    gs2_job_finish(fed);
    Box fbox = bounds(fed, 0);
    expect(fbox.any && inkAt(fed, 0, fbox.minX, 120), "closing A-8 makes CR feed one line");
    gs2_job_free(fed);
}

void testSetAutoLf() {
    gs2_job *job = gs2_job_new();
    gs2_job_set_auto_lf(job, 1);
    writeStr(job, "A\rB");
    gs2_job_finish(job);
    Box box = bounds(job, 0);
    // Same spacing as CR+LF: 1/6 inch is 120 pixels at 720 dpi.
    expect(box.any && inkAt(job, 0, box.minX, 120), "power-on auto line feed makes CR feed one line");
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

std::string pdfTestPath() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::wstring wide = n > 0 && n < MAX_PATH ? std::wstring(buf, n) : L".\\";
    wide += L"gs2print-test.pdf";
    int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(need > 1 ? static_cast<size_t>(need - 1) : 0, '\0');
    if (need > 1) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), need, nullptr, nullptr);
    }
    return out;
#else
    return "/tmp/gs2print-test.pdf";
#endif
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

    const std::string pdfPath = pdfTestPath();
    const char *path = pdfPath.c_str();
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
    expect(body.find("/OCG") != std::string::npos, "pdf has optional content groups");
    expect(body.find("Yellow") != std::string::npos && body.find("Magenta") != std::string::npos
               && body.find("Cyan") != std::string::npos && body.find("(Black)") != std::string::npos,
           "pdf names the four ribbon layers");
    expect(body.find("/Multiply") != std::string::npos, "pdf multiply-composites the planes");
    int masks = 0;
    for (size_t pos = 0; (pos = body.find("/ImageMask", pos)) != std::string::npos; ++pos) ++masks;
    expect(masks == 8, "two pages times four image-mask planes");
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

    const char *live = "GS2PRINT 1\nprinter imagewriter-ii\nsocket /tmp/gs2-print-12345-1.sock\n\n";
    auto *liveBytes = reinterpret_cast<const uint8_t *>(live);
    size_t liveLen = std::strlen(live);
    char field[64];
    expect(gs2_dump_payload_offset(liveBytes, liveLen) == liveLen, "live ticket ends at the blank line");
    expect(gs2_ticket_field(liveBytes, liveLen, "socket", field, sizeof field) == 1
               && std::strcmp(field, "/tmp/gs2-print-12345-1.sock") == 0,
           "socket field");
    expect(gs2_ticket_field(liveBytes, liveLen, "printer", field, sizeof field) == 1
               && std::strcmp(field, "imagewriter-ii") == 0,
           "printer field");
    expect(gs2_ticket_field(liveBytes, liveLen, "missing", field, sizeof field) == 0, "absent field");
    expect(gs2_ticket_field(liveBytes, liveLen, "socket", field, 4) == -1, "short buffer does not copy");

    auto *savedBytes = reinterpret_cast<const uint8_t *>(saved);
    expect(gs2_ticket_field(savedBytes, n, "socket", field, sizeof field) == 0, "saved dump has no socket");
    expect(gs2_ticket_field(reinterpret_cast<const uint8_t *>(raw), std::strlen(raw), "socket", field, sizeof field) == 0,
           "raw file is not a header");

    const char *crlf = "GS2PRINT 1\r\nprinter imagewriter-ii\r\nsocket /tmp/gs2.sock\r\n\r\n";
    expect(gs2_ticket_field(reinterpret_cast<const uint8_t *>(crlf), std::strlen(crlf), "socket", field, sizeof field) == 1
               && std::strcmp(field, "/tmp/gs2.sock") == 0,
           "CRLF ticket socket");

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

void testColour() {
    uint8_t ybit[1] = {0x80};
    uint8_t mbit[1] = {0x80};
    uint8_t zbit[1] = {0};
    const uint8_t *yellow[4] = {ybit, zbit, zbit, zbit};
    uint8_t rgb[3] = {};
    gs2_planes_composite(yellow, 1, 1, 1, rgb);
    expect(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 0, "composite yellow 1x1");
    const uint8_t *orange[4] = {ybit, mbit, zbit, zbit};
    gs2_planes_composite(orange, 1, 1, 1, rgb);
    expect(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0, "composite orange 1x1");
    const uint8_t *black[4] = {zbit, zbit, zbit, ybit};
    gs2_planes_composite(black, 1, 1, 1, rgb);
    expect(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0, "composite black 1x1");

    gs2_job *job = gs2_job_new();
    writeStr(job, "A");
    gs2_job_finish(job);
    expect(boundsPlane(job, 0, 3).any, "default ink is black");
    expect(!boundsPlane(job, 0, 0).any && !boundsPlane(job, 0, 1).any && !boundsPlane(job, 0, 2).any,
           "default is not colour");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1bK1A");
    gs2_job_finish(job);
    expect(boundsPlane(job, 0, 0).any, "ESC K1 inks yellow");
    expect(!boundsPlane(job, 0, 1).any, "ESC K1 not magenta");
    expect(!boundsPlane(job, 0, 2).any, "ESC K1 not cyan");
    expect(!boundsPlane(job, 0, 3).any, "ESC K1 not black");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1bK4A");
    gs2_job_finish(job);
    expect(boundsPlane(job, 0, 0).any && boundsPlane(job, 0, 1).any, "ESC K4 inks yellow and magenta");
    expect(!boundsPlane(job, 0, 2).any && !boundsPlane(job, 0, 3).any, "ESC K4 not cyan or black");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1bK1A\r\x1bK2A");
    gs2_job_finish(job);
    Box y = boundsPlane(job, 0, 0);
    Box m = boundsPlane(job, 0, 1);
    expect(y.any && m.any && y.minX == m.minX && y.minY == m.minY,
           "CR overprint shares yellow and magenta dots");
    expect(!boundsPlane(job, 0, 3).any, "overprint is not black");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1bK1A\x1b" "cB");
    gs2_job_finish(job);
    expect(boundsPlane(job, 0, 0).any, "yellow A survives ESC c");
    expect(boundsPlane(job, 0, 3).any, "ESC c returns to black");
    gs2_job_free(job);
}

void testIdentify() {
    const uint8_t want[] = {'I', 'W', '1', '0', 'C', '\r'};
    uint8_t got[16];

    gs2_job *job = gs2_job_new();
    expect(gs2_job_did_print(job) == 0, "new job has not printed");
    writeStr(job, "\x1b?");
    size_t n = gs2_job_take_reply(job, got, sizeof got);
    expect(n == sizeof want && std::memcmp(got, want, sizeof want) == 0, "ESC ? replies IW10C CR");
    expect(gs2_job_take_reply(job, got, sizeof got) == 0, "reply drains");
    expect(!bounds(job, 0).any, "ESC ? does not ink");
    expect(gs2_job_did_print(job) == 0, "ESC ? has not printed");
    gs2_job_finish(job);
    expect(gs2_job_did_print(job) == 0, "finished probe has not printed");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1b");
    expect(gs2_job_take_reply(job, got, sizeof got) == 0, "split ESC has no reply yet");
    writeStr(job, "?");
    n = gs2_job_take_reply(job, got, sizeof got);
    expect(n == sizeof want && std::memcmp(got, want, sizeof want) == 0, "split ESC ? replies on ?");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1b?\x1b?");
    n = gs2_job_take_reply(job, got, 4);
    expect(n == 4 && std::memcmp(got, want, 4) == 0, "short cap copies a prefix");
    n = gs2_job_take_reply(job, got, sizeof got);
    expect(n == 8 && std::memcmp(got, want + 4, 2) == 0 && std::memcmp(got + 2, want, 6) == 0,
           "remainder then the second reply");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "A");
    expect(gs2_job_take_reply(job, got, sizeof got) == 0, "text yields no reply");
    gs2_job_finish(job);
    expect(gs2_job_did_print(job) == 1, "a printed character sets did_print");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x1b?");
    writeStr(job, "A");
    gs2_job_finish(job);
    expect(gs2_job_did_print(job) == 1, "ink after the probe sets did_print");
    gs2_job_free(job);

    job = gs2_job_new();
    const uint8_t setup[] = {0x1B, 'c', 0x1B, 'Z', 0x07, 0x00, 0x1B, 'H', '1', '5', '8', '4'};
    gs2_job_write(job, setup, sizeof setup);
    expect(gs2_job_did_print(job) == 0, "reset and page length do not print");
    expect(gs2_job_take_reply(job, got, sizeof got) == 0, "reset yields no reply");
    gs2_job_free(job);

    job = gs2_job_new();
    writeStr(job, "\x0c");
    expect(gs2_job_did_print(job) == 0, "form feed of a blank page does not print");
    gs2_job_free(job);
}

}  // namespace

int main() {
    testTextAndCrLf();
    testCrOverstrike();
    testSetAutoLf();
    testEscT();
    testGraphics();
    testReverse();
    testFormFeedAndPdf();
    testHeader();
    testSplitCommands();
    testColour();
    testIdentify();
    if (g_failed) {
        std::printf("%d failed\n", g_failed);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
