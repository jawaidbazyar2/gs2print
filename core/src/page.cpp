#include "page.hpp"

#include <algorithm>

Page::Page() : bits(static_cast<size_t>(kStride) * kHeight, 0) {}

void Page::fill(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(kWidth, x + w);
    int y1 = std::min(kHeight, y + h);
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
