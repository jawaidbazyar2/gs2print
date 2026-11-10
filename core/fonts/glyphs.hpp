#ifndef GS2_GLYPHS_HPP
#define GS2_GLYPHS_HPP

#include <stdint.h>

// Column bitmaps from the ImageWriter II Technical Reference, transcribed
// by mikedaley/web-a2e (MIT). Bit 0 is the top wire.

enum class IwLocale {
    US = 0,
    IT,
    DK,
    UK,
    DE,
    SE,
    FR,
    ES
};

struct IwGlyph {
    const uint32_t *cols;
    int n;
};

IwGlyph iw_draft(unsigned code, IwLocale loc);
IwGlyph iw_nlq_fixed(unsigned code, IwLocale loc);
IwGlyph iw_nlq_prop(unsigned code, IwLocale loc);
IwGlyph iw_corr_fixed(unsigned code, IwLocale loc);
IwGlyph iw_corr_prop(unsigned code, IwLocale loc);

#endif
