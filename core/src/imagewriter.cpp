#include "imagewriter.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kDpi = 1440;

struct Pitch {
    double cpi;
    int gfxDpi;
};

constexpr Pitch kPitches[] = {
    {9.0, 72},
    {10.0, 80},
    {12.0, 96},
    {13.4, 107},
    {15.0, 120},
    {17.0, 136},
    {10.0, 144},
    {12.0, 160},
};

constexpr int kPlaten = kDpi * 8;

int divRound(int num, int den) {
    return static_cast<int>(std::lround(static_cast<double>(num) / den));
}

int digit(uint8_t ch) {
    if (ch == 0x20) return 0;
    if (ch >= '0' && ch <= '9') return ch - '0';
    return -1;
}

}  // namespace

ImageWriter::ImageWriter(Paper *paper)
    : paper_(paper),
      formH_(kDpi * 11),
      lineHeight_(kDpi / 6) {}

void ImageWriter::write(const uint8_t *bytes, size_t len) {
    if (!bytes || len == 0) return;
    for (size_t i = 0; i < len; ++i) byteIn(bytes[i]);
}

void ImageWriter::finish() {
    state_ = State::Ground;
    numLeft_ = 0;
    gfxLeft_ = 0;
    imgLeft_ = 0;
    customLeft_ = 0;
    sync();
    if (pendingCR_) {
        pendingCR_ = false;
        feed(lineHeight_);
    }
}

void ImageWriter::sync() { flushLine(); }

void ImageWriter::byteIn(uint8_t raw) {
    uint8_t ch = includeEighth_ ? raw : static_cast<uint8_t>(raw & 0x7F);
    if (deselected_) {
        if (ch == 0x11) deselected_ = false;
        return;
    }

    switch (state_) {
    case State::Ground:
        ground(ch);
        break;
    case State::Escape:
        state_ = State::Ground;
        handleEsc(ch);
        break;
    case State::Param:
        paramByte(ch);
        state_ = State::Ground;
        break;
    case State::Number:
        numberByte(ch);
        break;
    case State::ImgCount:
        imgCountByte(ch);
        break;
    case State::ImgData:
        imgDataByte(raw);
        break;
    case State::RepeatChar:
        for (int i = 0; i < numAcc_; ++i) emitChar(ch);
        state_ = State::Ground;
        break;
    case State::RepeatCol:
        for (int i = 0; i < numAcc_; ++i) graphicsColumn(raw, gfxDotW());
        state_ = State::Ground;
        break;
    case State::TabSet:
        tabByte(ch, true);
        break;
    case State::TabClear:
        tabByte(ch, false);
        break;
    case State::Switch:
        switchByte(raw);
        break;
    case State::CustomKey:
        if (ch == 0x04) state_ = State::Ground;
        else {
            customKey_ = ch;
            state_ = State::CustomWidth;
        }
        break;
    case State::CustomWidth:
        customWidth(ch);
        break;
    case State::CustomData:
        customData(raw);
        break;
    }
}

void ImageWriter::ground(uint8_t ch) {
    if (ch == 0x1B) {
        state_ = State::Escape;
        return;
    }
    if (ch == 0x0E) {
        doubleWidth_ = true;
        return;
    }
    if (ch == 0x0F) {
        doubleWidth_ = false;
        return;
    }
    if (ch == 0x08) {
        releasePendingFeed();
        int step = lastAdvance_ ? lastAdvance_ : charAdvance();
        x_ = std::max(left_, x_ - step);
        return;
    }
    if (ch == 0x09) {
        releasePendingFeed();
        if (tabs_.empty()) return;
        int cell = std::max(1, colDots());
        int cur = static_cast<int>(std::lround(static_cast<double>(x_ - left_) / cell));
        for (int stop : tabs_) {
            if (stop > cur) {
                x_ = left_ + static_cast<int>(std::lround(stop * static_cast<double>(cell)));
                break;
            }
        }
        return;
    }
    if (ch == 0x0C) {
        formFeed();
        return;
    }
    if (ch == 0x0D) {
        carriageReturn();
        return;
    }
    if (ch == 0x0A) {
        // A CR arms one feed. The LF that closes the line performs it, so
        // CR+LF and CR, ESC T nn, LF each move the paper once, at the line
        // height in effect when the LF arrives.
        lineFeed();
        return;
    }
    if (ch == 0x18) {
        cancelLine();
        return;
    }
    if (ch == 0x13) {
        if (selectEnabled_) deselected_ = true;
        return;
    }
    if (ch == 0x1F) {
        paramCmd_ = 0x1F;
        state_ = State::Param;
        return;
    }
    if ((ch >= 0x20 && ch < 0x7F) || (includeEighth_ && ch >= 0xA0)) {
        unsigned code = (mouseText_ && ch >= 0x40 && ch <= 0x5F) ? ch + 0x80 : ch;
        emitChar(code);
    }
}

void ImageWriter::handleEsc(uint8_t ch) {
    switch (ch) {
    case 'n': pitch_ = 0; proportional_ = false; break;
    case 'N': pitch_ = 1; proportional_ = false; break;
    case 'E': pitch_ = 2; proportional_ = false; break;
    case 'e': pitch_ = 3; proportional_ = false; break;
    case 'q': pitch_ = 4; proportional_ = false; break;
    case 'Q': pitch_ = 5; proportional_ = false; break;
    case 'p': pitch_ = 6; proportional_ = true; break;
    case 'P': pitch_ = 7; proportional_ = true; break;

    case 'm': quality_ = Quality::Corr; break;
    case 'M': quality_ = Quality::Nlq; break;

    case '!': bold_ = true; break;
    case '"': bold_ = false; break;
    case 'X': underline_ = true; break;
    case 'Y': underline_ = false; break;
    case 'w': halfHeight_ = true; break;
    case 'W': halfHeight_ = false; break;
    case 'x': script_ = Script::Super; break;
    case 'y': script_ = Script::Sub; break;
    case 'z': script_ = Script::None; break;

    case '>': break;
    case '<': break;

    case '&': mouseText_ = true; break;
    case '$':
        mouseText_ = false;
        customFont_ = CustomFont::None;
        break;
    case '\'': customFont_ = CustomFont::Low; break;
    case '*': customFont_ = CustomFont::High; break;

    case 'A': lineHeight_ = kDpi / 6; break;
    case 'B': lineHeight_ = kDpi / 8; break;
    case 'T': beginNumber('T', 2); break;
    case 'f': feedDir_ = 1; break;
    case 'r': feedDir_ = -1; break;
    case 'O': break;
    case 'o': break;
    case 'H': beginNumber('H', 4); break;
    case 'L': beginNumber('L', 3); break;
    case '(':
        tabList_.clear();
        tabAcc_ = 0;
        tabDig_ = false;
        state_ = State::TabSet;
        break;
    case ')':
        tabList_.clear();
        tabAcc_ = 0;
        tabDig_ = false;
        state_ = State::TabClear;
        break;
    case '0': tabs_.clear(); break;
    case '1': case '2': case '3': case '4': case '5': case '6':
        pendingDots_ = ch - '0';
        break;
    case 'u': beginNumber('u', 3); break;
    case 'F': beginNumber('F', 4); break;
    case 'R': beginNumber('R', 3); break;
    case 'V': beginNumber('V', 4); break;
    case 'D':
        swClose_ = true;
        swPos_ = 0;
        state_ = State::Switch;
        break;
    case 'Z':
        swClose_ = false;
        swPos_ = 0;
        state_ = State::Switch;
        break;
    case 'v':
        y_ = 0;
        break;
    case 'a':
    case 'K':
    case 'l':
    case 's':
        paramCmd_ = ch;
        state_ = State::Param;
        break;
    case 'G':
    case 'S':
        gfxLeft_ = 4;
        gfxAcc_ = 0;
        gfxMul_ = 1;
        imgDotW_ = gfxDotW();
        state_ = State::ImgCount;
        break;
    case 'g':
        gfxLeft_ = 3;
        gfxAcc_ = 0;
        gfxMul_ = 8;
        imgDotW_ = gfxDotW();
        state_ = State::ImgCount;
        break;
    case '-':
        customMax_ = 8;
        custom_.clear();
        break;
    case '+':
        customMax_ = 16;
        custom_.clear();
        break;
    case 'I':
        state_ = State::CustomKey;
        break;
    case 'c':
        pitch_ = 1;
        proportional_ = false;
        bold_ = underline_ = halfHeight_ = doubleWidth_ = false;
        script_ = Script::None;
        mouseText_ = false;
        customFont_ = CustomFont::None;
        propSpacing_ = pendingDots_ = 0;
        crBeforeLF_ = true;
        feedDir_ = 1;
        quality_ = Quality::Draft;
        left_ = 0;
        lineHeight_ = kDpi / 6;
        x_ = 0;
        y_ = 0;
        lastAdvance_ = 0;
        pendingCR_ = false;
        deselected_ = false;
        tabs_.clear();
        state_ = State::Ground;
        break;
    default:
        break;
    }
}

void ImageWriter::paramByte(uint8_t ch) {
    switch (paramCmd_) {
    case 'K':
        break;
    case 'a': {
        int sel = ch & 0x0F;
        if (sel == 0) quality_ = Quality::Corr;
        else if (sel == 1) quality_ = Quality::Draft;
        else if (sel == 2) quality_ = Quality::Nlq;
        break;
    }
    case 's': {
        int n = (ch >= '0' && ch <= '9') ? ch - '0' : (ch & 0x0F);
        propSpacing_ = std::max(0, std::min(9, n));
        break;
    }
    case 'l':
        crBeforeLF_ = (ch == '0');
        break;
    case 0x1F:
        if (ch >= 0x31 && ch <= 0x3F) {
            int lines = ch - 0x30;
            if (crBeforeLF_) x_ = left_;
            feed(feedDir_ * lines * lineHeight_);
        }
        break;
    default:
        break;
    }
}

void ImageWriter::beginNumber(uint8_t cmd, int digits) {
    numCmd_ = cmd;
    numLeft_ = digits;
    numAcc_ = 0;
    state_ = State::Number;
}

void ImageWriter::numberByte(uint8_t ch) {
    int d = digit(ch);
    if (d >= 0) numAcc_ = numAcc_ * 10 + d;
    if (--numLeft_ <= 0) dispatchNumber();
}

void ImageWriter::dispatchNumber() {
    int n = numAcc_;
    switch (numCmd_) {
    case 'T':
        lineHeight_ = n * (kDpi / 144);
        state_ = State::Ground;
        break;
    case 'H': {
        int dots = n * (kDpi / 144);
        if (dots < kDpi) dots = kDpi;
        if (dots > kDpi * 11) dots = kDpi * 11;
        formH_ = dots;
        state_ = State::Ground;
        break;
    }
    case 'L':
        left_ = static_cast<int>(std::lround(n * static_cast<double>(colDots())));
        if (x_ < left_) x_ = left_;
        state_ = State::Ground;
        break;
    case 'F':
        x_ = left_ + static_cast<int>(std::lround(n * static_cast<double>(gfxDotW())));
        state_ = State::Ground;
        break;
    case 'u':
        if (n > 0) {
            tabs_.push_back(n);
            std::sort(tabs_.begin(), tabs_.end());
            tabs_.erase(std::unique(tabs_.begin(), tabs_.end()), tabs_.end());
        }
        state_ = State::Ground;
        break;
    case 'R':
        state_ = State::RepeatChar;
        break;
    case 'V':
        state_ = State::RepeatCol;
        break;
    default:
        state_ = State::Ground;
        break;
    }
}

void ImageWriter::imgCountByte(uint8_t ch) {
    int d = digit(ch);
    if (d >= 0) gfxAcc_ = gfxAcc_ * 10 + d;
    if (--gfxLeft_ <= 0) {
        imgLeft_ = gfxAcc_ * gfxMul_;
        state_ = imgLeft_ > 0 ? State::ImgData : State::Ground;
    }
}

void ImageWriter::imgDataByte(uint8_t raw) {
    graphicsColumn(raw, imgDotW_);
    if (--imgLeft_ <= 0) state_ = State::Ground;
}

void ImageWriter::tabByte(uint8_t ch, bool set) {
    if (ch >= '0' && ch <= '9') {
        tabAcc_ = tabAcc_ * 10 + (ch - '0');
        tabDig_ = true;
        return;
    }
    if (ch == '.') {
        if (tabDig_) tabList_.push_back(tabAcc_);
        if (set) {
            for (int c : tabList_) if (c > 0) tabs_.push_back(c);
            std::sort(tabs_.begin(), tabs_.end());
            tabs_.erase(std::unique(tabs_.begin(), tabs_.end()), tabs_.end());
        } else {
            tabs_.erase(std::remove_if(tabs_.begin(), tabs_.end(), [&](int c) {
                return std::find(tabList_.begin(), tabList_.end(), c) != tabList_.end();
            }), tabs_.end());
        }
        state_ = State::Ground;
        return;
    }
    if (tabDig_) tabList_.push_back(tabAcc_);
    tabAcc_ = 0;
    tabDig_ = false;
}

void ImageWriter::switchByte(uint8_t raw) {
    if (swPos_ == 0) {
        swFirst_ = raw;
        swPos_ = 1;
        return;
    }
    if (swClose_) {
        swA_ |= swFirst_;
        swB_ |= raw;
    } else {
        swA_ = static_cast<uint8_t>(swA_ & ~swFirst_);
        swB_ = static_cast<uint8_t>(swB_ & ~raw);
    }
    applySwitches();
    state_ = State::Ground;
}

void ImageWriter::applySwitches() {
    autoLF_ = (swA_ & 0x80) != 0;
    crBeforeLF_ = (swA_ & 0x40) != 0;
    selectEnabled_ = (swA_ & 0x10) == 0;
    includeEighth_ = (swB_ & 0x20) == 0;
    slashedZero_ = (swB_ & 0x01) != 0;
    if (!selectEnabled_) deselected_ = false;
}

void ImageWriter::customWidth(uint8_t ch) {
    int cols = 0;
    if (ch >= 'A' && ch <= 'P') {
        cols = ch - '@';
        customTop_ = true;
    } else if (ch >= 'a' && ch <= 'p') {
        cols = ch - '`';
        customTop_ = false;
    } else {
        state_ = State::Ground;
        return;
    }
    customLeft_ = std::min(cols, customMax_);
    customBuf_.clear();
    if (customLeft_ <= 0) {
        state_ = State::CustomKey;
        return;
    }
    state_ = State::CustomData;
}

void ImageWriter::customData(uint8_t raw) {
    customBuf_.push_back(raw);
    if (--customLeft_ > 0) return;
    CustomChar cc;
    cc.wireTop = customTop_;
    cc.data = customBuf_;
    custom_[customKey_] = std::move(cc);
    state_ = State::CustomKey;
}

ImageWriter::Quality ImageWriter::effective() const {
    if (script_ != Script::None && (quality_ == Quality::Draft || quality_ == Quality::Nlq))
        return Quality::Corr;
    if (quality_ == Quality::Draft && (bold_ || halfHeight_ || doubleWidth_ || proportional_))
        return Quality::Corr;
    return quality_;
}

IwLocale ImageWriter::locale() const {
    switch (swA_ & 0x07) {
    case 1: return IwLocale::IT;
    case 2: return IwLocale::DK;
    case 3: return IwLocale::UK;
    case 4: return IwLocale::DE;
    case 5: return IwLocale::SE;
    case 6: return IwLocale::FR;
    case 7: return IwLocale::ES;
    default: return IwLocale::US;
    }
}

int ImageWriter::gfxDotW() const {
    return std::max(1, divRound(kDpi, kPitches[pitch_].gfxDpi));
}

int ImageWriter::colDots() const {
    return std::max(1, static_cast<int>(std::lround(kDpi / kPitches[pitch_].cpi)));
}

int ImageWriter::charAdvance() const {
    return colDots() * (doubleWidth_ ? 2 : 1);
}

void ImageWriter::emitChar(unsigned code) {
    releasePendingFeed();

    if (pendingDots_) {
        x_ += pendingDots_ * gfxDotW();
        pendingDots_ = 0;
    }

    Quality face = effective();
    bool nlq = customFont_ == CustomFont::None && face == Quality::Nlq;
    IwGlyph prop;
    prop.cols = nullptr;
    prop.n = 0;
    bool propNlq = false;
    if (proportional_ && customFont_ == CustomFont::None) {
        if (nlq) {
            prop = iw_nlq_prop(code, locale());
            if (prop.n) propNlq = true;
            else prop = iw_corr_prop(code, locale());
        } else {
            prop = iw_corr_prop(code, locale());
        }
    }

    const CustomChar *custom = nullptr;
    if (customFont_ != CustomFont::None) {
        auto it = custom_.find(static_cast<uint8_t>(code));
        if (it != custom_.end()) custom = &it->second;
    }

    int xs = doubleWidth_ ? 2 : 1;
    IwGlyph glyph;
    glyph.cols = nullptr;
    glyph.n = 0;
    int hDpi = 120;
    int vDpi = 72;
    int rows = 9;
    int adv = charAdvance();
    bool half = halfHeight_ || script_ != Script::None;

    if (prop.n) {
        glyph = prop;
        hDpi = propNlq ? 160 : kPitches[pitch_].gfxDpi;
        vDpi = propNlq ? 144 : 72;
        rows = propNlq ? 18 : 9;
        int dotW = std::max(1, divRound(kDpi, hDpi));
        double scale = propNlq ? (120.0 / 160.0) : 1.0;
        adv = static_cast<int>(std::lround((glyph.n * xs + propSpacing_) * dotW * scale));
    } else if (custom) {
        hDpi = kPitches[pitch_].gfxDpi;
        rows = 9;
        adv = custom->data.size() * xs * gfxDotW();
    } else {
        if (face == Quality::Draft) glyph = iw_draft(code, locale());
        else if (face == Quality::Nlq) {
            glyph = iw_nlq_fixed(code, locale());
            if (!glyph.n) glyph = iw_corr_fixed(code, locale());
        } else {
            glyph = iw_corr_fixed(code, locale());
        }
        if (face == Quality::Nlq && iw_nlq_fixed(code, locale()).n) {
            hDpi = 160;
            vDpi = 144;
            rows = (code >= 0xC0 && code <= 0xDF) ? 16 : 18;
        }
        adv = charAdvance();
    }

    if (x_ + adv > kPlaten) {
        x_ = left_;
        feed(lineHeight_);
    }

    if (custom) {
        std::vector<uint32_t> cols(custom->data.size());
        int shift = custom->wireTop ? 0 : 1;
        for (size_t i = 0; i < custom->data.size(); ++i)
            cols[i] = static_cast<uint32_t>(custom->data[i]) << shift;
        strokeGlyph(cols.data(), static_cast<int>(cols.size()), rows, hDpi, vDpi,
                    x_, y_, xs, half);
    } else if (glyph.n) {
        if (slashedZero_ && code == 0x30) {
            std::vector<uint32_t> cols(glyph.cols, glyph.cols + glyph.n);
            int denom = std::max(1, glyph.n - 1);
            for (int c = 0; c < glyph.n; ++c) {
                int bit = (rows - 1) * c / denom;
                cols[c] |= 1u << bit;
            }
            strokeGlyph(cols.data(), glyph.n, rows, hDpi, vDpi, x_, y_, xs, half);
        } else {
            strokeGlyph(glyph.cols, glyph.n, rows, hDpi, vDpi, x_, y_, xs, half);
        }
    }

    if (underline_ && adv > 0) {
        int row = divRound(kDpi, vDpi);
        int uy = y_ + 8 * row;
        line_.push_back({x_, uy, adv, std::max(1, row / 4)});
    }

    x_ += adv;
    lastAdvance_ = adv;
}

void ImageWriter::strokeGlyph(const uint32_t *cols, int n, int rows, int hDpi, int vDpi,
                              int x, int y, int xScale, bool half) {
    int colW = std::max(1, divRound(kDpi, hDpi));
    int rowH = std::max(1, divRound(kDpi, vDpi));
    int fullRow = rowH;
    if (half) rowH = std::max(1, rowH / 2);
    int y0 = y;
    if (script_ == Script::Sub) y0 += (rows / 2) * fullRow;
    else if (script_ == Script::Super) y0 -= (rows / 4) * fullRow;

    int drawnW = colW * xScale;
    for (int c = 0; c < n; ++c) {
        uint32_t bits = cols[c];
        int cx = x + c * drawnW;
        for (int r = 0; r < rows; ++r) {
            if ((bits >> r) & 1u) {
                line_.push_back({cx, y0 + r * rowH, drawnW, rowH});
            }
        }
        if (bold_) {
            int bx = cx + std::max(1, colW / 2);
            for (int r = 0; r < rows; ++r) {
                if ((bits >> r) & 1u)
                    line_.push_back({bx, y0 + r * rowH, drawnW, rowH});
            }
        }
    }
}

void ImageWriter::graphicsColumn(uint8_t bits, int dotW) {
    releasePendingFeed();
    int rowH = divRound(kDpi, 72);
    for (int r = 0; r < 8; ++r) {
        if ((bits >> r) & 1u)
            line_.push_back({x_, y_ + r * rowH, dotW, rowH});
    }
    x_ += dotW;
    lastAdvance_ = dotW;
}

void ImageWriter::feed(int dots) {
    flushLine();
    if (dots >= 0) {
        y_ += dots;
        while (y_ >= formH_) {
            paper_->formFeed();
            y_ -= formH_;
        }
    } else {
        y_ += dots;
        if (y_ < 0) y_ = 0;
    }
}

void ImageWriter::releasePendingFeed() {
    if (!pendingCR_) return;
    pendingCR_ = false;
    feed(feedDir_ * lineHeight_);
}

void ImageWriter::carriageReturn() {
    flushLine();
    x_ = left_;
    if (autoLF_) pendingCR_ = true;
    else pendingCR_ = false;
}

void ImageWriter::lineFeed() {
    flushLine();
    if (pendingCR_) {
        pendingCR_ = false;
        feed(feedDir_ * lineHeight_);
        return;
    }
    if (crBeforeLF_) x_ = left_;
    feed(feedDir_ * lineHeight_);
}

void ImageWriter::formFeed() {
    flushLine();
    pendingCR_ = false;
    paper_->formFeed();
    y_ = 0;
    x_ = left_;
}

void ImageWriter::flushLine() {
    for (const Rect &r : line_) paper_->ink(r.x, r.y, r.w, r.h);
    line_.clear();
}

void ImageWriter::cancelLine() {
    line_.clear();
    x_ = left_;
    pendingDots_ = 0;
}
