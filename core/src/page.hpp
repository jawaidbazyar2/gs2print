#ifndef GS2_PAGE_HPP
#define GS2_PAGE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// One 8.5 by 11 inch page. The head is placed on a 1440 dpi grid. The stored
// bitmap is 720 dpi, four packed 1-bit planes, bit 7 leftmost. Two grid units
// make one pixel. Plane 0 is yellow, 1 magenta, 2 cyan, 3 black.
struct Page {
    static constexpr int kGrid = 1440;
    static constexpr int kDpi = 720;
    static constexpr int kScale = kGrid / kDpi;
    static constexpr int kWidth = kDpi * 85 / 10;   // 8.5 in
    static constexpr int kHeight = kDpi * 11;       // 11 in
    static constexpr int kStride = kWidth / 8;
    static constexpr int kPlanes = 4;
    // Left margin of the 8 inch platen, in grid units.
    static constexpr int kOriginX = ((kGrid * 85 / 10) - (kGrid * 8)) / 2;

    std::vector<uint8_t> bits[kPlanes];
    std::vector<uint8_t> packed[kPlanes];

    Page();
    Page(Page &&) noexcept = default;
    Page &operator=(Page &&) noexcept = default;
    Page(const Page &) = delete;
    Page &operator=(const Page &) = delete;

    // x, y, w, h are 1440 dpi grid units. Edges map with integer division by kScale.
    // mask bit 0 is yellow, bit 1 magenta, bit 2 cyan, bit 3 black.
    void fill(int x, int y, int w, int h, unsigned mask);

    // PNG Up rows: a filter byte of 2, then each byte minus the byte above it.
    static void encodeUp(const uint8_t *bits, std::vector<uint8_t> &out);

    // Deflate the Up rows of each plane and drop the raw bitmaps. A failure
    // leaves that plane's raw bitmap.
    void compress();

    // Inflate packed[plane] into out. False when that plane is not compressed.
    bool inflateInto(int plane, std::vector<uint8_t> &out) const;
};

#endif
