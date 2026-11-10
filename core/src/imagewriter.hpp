#ifndef GS2_IMAGEWRITER_HPP
#define GS2_IMAGEWRITER_HPP

#include "glyphs.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// The page the head is printing on. Coordinates are platen dots at 1440 dpi,
// origin at the left of the 8 inch printable width, y growing down.
class Paper {
public:
    virtual ~Paper() = default;
    virtual void ink(int x, int y, int w, int h) = 0;
    virtual void formFeed() = 0;
};

// ImageWriter II decoder. One byte changes state. write() never waits for
// a byte it has not been given.
class ImageWriter {
public:
    explicit ImageWriter(Paper *paper);

    void write(const uint8_t *bytes, size_t len);
    // Drop a half-received command and strike any buffered line.
    void finish();
    void sync();

    int lineHeight() const { return lineHeight_; }
    bool pendingFeed() const { return pendingCR_; }

private:
    enum class State {
        Ground,
        Escape,
        Param,
        Number,
        ImgCount,
        ImgData,
        RepeatChar,
        RepeatCol,
        TabSet,
        TabClear,
        Switch,
        CustomKey,
        CustomWidth,
        CustomData
    };

    enum class Quality { Draft, Corr, Nlq };
    enum class Script { None, Super, Sub };
    enum class CustomFont { None, Low, High };

    struct CustomChar {
        bool wireTop = true;
        std::vector<uint8_t> data;
    };

    struct Rect {
        int x, y, w, h;
    };

    void byteIn(uint8_t raw);
    void ground(uint8_t ch);
    void handleEsc(uint8_t ch);
    void paramByte(uint8_t ch);
    void numberByte(uint8_t ch);
    void dispatchNumber();
    void beginNumber(uint8_t cmd, int digits);
    void imgCountByte(uint8_t ch);
    void imgDataByte(uint8_t raw);
    void tabByte(uint8_t ch, bool set);
    void switchByte(uint8_t raw);
    void applySwitches();
    void customWidth(uint8_t ch);
    void customData(uint8_t raw);

    void emitChar(unsigned code);
    void graphicsColumn(uint8_t bits, int dotW);
    void strokeGlyph(const uint32_t *cols, int n, int rows, int hDpi, int vDpi,
                     int x, int y, int xScale, bool half);
    void feed(int dots);
    void carriageReturn();
    void lineFeed();
    void formFeed();
    void releasePendingFeed();
    void flushLine();
    void cancelLine();

    int charAdvance() const;
    int gfxDotW() const;
    int colDots() const;
    Quality effective() const;
    IwLocale locale() const;

    Paper *paper_;
    State state_ = State::Ground;

    int x_ = 0;
    int y_ = 0;
    int left_ = 0;
    int formH_;
    int lineHeight_;
    int feedDir_ = 1;
    int lastAdvance_ = 0;
    bool pendingCR_ = false;

    Quality quality_ = Quality::Draft;
    bool proportional_ = false;
    bool bold_ = false;
    bool underline_ = false;
    bool halfHeight_ = false;
    bool doubleWidth_ = false;
    Script script_ = Script::None;
    bool mouseText_ = false;
    bool autoLF_ = true;
    bool crBeforeLF_ = true;
    bool includeEighth_ = false;
    bool slashedZero_ = false;
    bool selectEnabled_ = false;
    bool deselected_ = false;
    int propSpacing_ = 0;
    int pendingDots_ = 0;
    int customMax_ = 8;
    CustomFont customFont_ = CustomFont::None;

    // Pitch index 0..7: extended, pica, elite, semicondensed, condensed,
    // ultracondensed, proportional pica, proportional elite.
    int pitch_ = 1;

    uint8_t swA_ = 0xD0;
    uint8_t swB_ = 0x20;

    uint8_t paramCmd_ = 0;
    uint8_t numCmd_ = 0;
    int numLeft_ = 0;
    int numAcc_ = 0;
    int gfxLeft_ = 0;
    int gfxAcc_ = 0;
    int gfxMul_ = 1;
    int imgLeft_ = 0;
    int imgDotW_ = 18;

    int tabAcc_ = 0;
    bool tabDig_ = false;
    std::vector<int> tabList_;
    std::vector<int> tabs_;

    int swPos_ = 0;
    uint8_t swFirst_ = 0;
    bool swClose_ = false;

    uint8_t customKey_ = 0;
    bool customTop_ = true;
    int customLeft_ = 0;
    std::vector<uint8_t> customBuf_;
    std::unordered_map<uint8_t, CustomChar> custom_;

    std::vector<Rect> line_;
};

#endif
