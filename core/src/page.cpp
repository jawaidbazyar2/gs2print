#include "page.hpp"

#include <zlib.h>

namespace {

constexpr size_t rawBytes() {
    return static_cast<size_t>(Page::kStride) * Page::kHeight;
}

constexpr size_t filteredBytes() {
    return static_cast<size_t>(Page::kHeight) * (Page::kStride + 1);
}

bool decodeUp(const uint8_t *filtered, size_t len, std::vector<uint8_t> &bits) {
    const size_t rowBytes = static_cast<size_t>(Page::kStride);
    if (len != filteredBytes() || bits.size() != rawBytes()) return false;
    for (int y = 0; y < Page::kHeight; ++y) {
        const uint8_t *src = filtered + static_cast<size_t>(y) * (rowBytes + 1);
        if (src[0] != 2) return false;
        uint8_t *row = bits.data() + static_cast<size_t>(y) * rowBytes;
        const uint8_t *prev = y ? row - rowBytes : nullptr;
        for (size_t i = 0; i < rowBytes; ++i)
            row[i] = static_cast<uint8_t>(src[1 + i] + (prev ? prev[i] : 0));
    }
    return true;
}

void fillRect(uint8_t *bits, int x0, int y0, int x1, int y1) {
    for (int yy = y0; yy < y1; ++yy) {
        uint8_t *row = bits + static_cast<size_t>(yy) * Page::kStride;
        int xx = x0;
        while (xx < x1 && (xx & 7)) {
            row[xx >> 3] |= static_cast<uint8_t>(0x80 >> (xx & 7));
            ++xx;
        }
        while (xx + 8 <= x1) {
            row[xx >> 3] = 0xFF;
            xx += 8;
        }
        while (xx < x1) {
            row[xx >> 3] |= static_cast<uint8_t>(0x80 >> (xx & 7));
            ++xx;
        }
    }
}

}  // namespace

Page::Page() {
    for (int p = 0; p < kPlanes; ++p) bits[p].assign(rawBytes(), 0);
}

void Page::fill(int x, int y, int w, int h, unsigned mask) {
    if (w <= 0 || h <= 0 || !mask) return;
    int x0 = x / kScale;
    int y0 = y / kScale;
    int x1 = (x + w) / kScale;
    int y1 = (y + h) / kScale;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kWidth) x1 = kWidth;
    if (y1 > kHeight) y1 = kHeight;
    if (x0 >= x1 || y0 >= y1) return;

    for (int p = 0; p < kPlanes; ++p) {
        if (!(mask & (1u << p))) continue;
        if (bits[p].empty()) continue;
        fillRect(bits[p].data(), x0, y0, x1, y1);
    }
}

void Page::encodeUp(const uint8_t *bits, std::vector<uint8_t> &out) {
    const size_t rowBytes = static_cast<size_t>(kStride);
    out.resize(filteredBytes());
    const uint8_t *prev = nullptr;
    for (int y = 0; y < kHeight; ++y) {
        uint8_t *dst = out.data() + static_cast<size_t>(y) * (rowBytes + 1);
        const uint8_t *row = bits + static_cast<size_t>(y) * rowBytes;
        dst[0] = 2;
        for (size_t i = 0; i < rowBytes; ++i)
            dst[1 + i] = static_cast<uint8_t>(row[i] - (prev ? prev[i] : 0));
        prev = row;
    }
}

void Page::compress() {
    for (int p = 0; p < kPlanes; ++p) {
        if (bits[p].size() != rawBytes()) continue;
        std::vector<uint8_t> filtered;
        encodeUp(bits[p].data(), filtered);
        uLongf bound = compressBound(static_cast<uLong>(filtered.size()));
        std::vector<uint8_t> out(bound);
        uLongf outLen = bound;
        int rc = compress2(out.data(), &outLen, filtered.data(),
                           static_cast<uLong>(filtered.size()), Z_DEFAULT_COMPRESSION);
        if (rc != Z_OK) continue;
        out.resize(static_cast<size_t>(outLen));
        packed[p] = std::move(out);
        std::vector<uint8_t>().swap(bits[p]);
    }
}

bool Page::inflateInto(int plane, std::vector<uint8_t> &out) const {
    if (plane < 0 || plane >= kPlanes) return false;
    if (packed[plane].empty()) return false;
    std::vector<uint8_t> filtered(filteredBytes());
    uLongf got = static_cast<uLongf>(filtered.size());
    int rc = uncompress(filtered.data(), &got, packed[plane].data(),
                        static_cast<uLong>(packed[plane].size()));
    if (rc != Z_OK || got != filtered.size()) return false;
    out.assign(rawBytes(), 0);
    return decodeUp(filtered.data(), filtered.size(), out);
}
