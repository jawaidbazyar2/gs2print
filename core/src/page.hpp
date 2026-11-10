#ifndef GS2_PAGE_HPP
#define GS2_PAGE_HPP

#include <cstdint>
#include <vector>

// One 8.5 by 11 inch page at 1440 dpi, packed 1-bit, bit 7 leftmost.
struct Page {
    static constexpr int kDpi = 1440;
    static constexpr int kWidth = kDpi * 85 / 10;   // 8.5 in
    static constexpr int kHeight = kDpi * 11;       // 11 in
    static constexpr int kPlaten = kDpi * 8;        // 8 in printable
    static constexpr int kOriginX = (kWidth - kPlaten) / 2;
    static constexpr int kStride = kWidth / 8;

    std::vector<uint8_t> bits;

    Page();
    Page(Page &&) noexcept = default;
    Page &operator=(Page &&) noexcept = default;
    Page(const Page &) = delete;
    Page &operator=(const Page &) = delete;

    void fill(int x, int y, int w, int h);
};

#endif
