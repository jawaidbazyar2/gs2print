#include "page.hpp"

#include <algorithm>
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

}  // namespace

Page::Page() : bits(rawBytes(), 0) {}

void Page::fill(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || bits.empty()) return;
    int x0 = x / kScale;
    int y0 = y / kScale;
    int x1 = (x + w) / kScale;
    int y1 = (y + h) / kScale;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kWidth) x1 = kWidth;
    if (y1 > kHeight) y1 = kHeight;
    if (x0 >= x1 || y0 >= y1) return;

    for (int yy = y0; yy < y1; ++yy) {
        uint8_t *row = bits.data() + static_cast<size_t>(yy) * kStride;
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
    if (bits.size() != rawBytes()) return;
    std::vector<uint8_t> filtered;
    encodeUp(bits.data(), filtered);
    uLongf bound = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<uint8_t> out(bound);
    uLongf outLen = bound;
    int rc = compress2(out.data(), &outLen, filtered.data(),
                       static_cast<uLong>(filtered.size()), Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return;
    out.resize(static_cast<size_t>(outLen));
    packed = std::move(out);
    std::vector<uint8_t>().swap(bits);
}

bool Page::inflateInto(std::vector<uint8_t> &out) const {
    if (packed.empty()) return false;
    std::vector<uint8_t> filtered(filteredBytes());
    uLongf got = static_cast<uLongf>(filtered.size());
    int rc = uncompress(filtered.data(), &got, packed.data(), static_cast<uLong>(packed.size()));
    if (rc != Z_OK || got != filtered.size()) return false;
    out.assign(rawBytes(), 0);
    return decodeUp(filtered.data(), filtered.size(), out);
}
