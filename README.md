# gs2print

macOS app that turns an ImageWriter II byte stream into a page preview and a PDF. Open a raw dump (`GS2.SSC*`, `GS2.PARL*`, or any headerless printer capture) or a `GS2PRINT 1` file. The PDF is written next to the source, with the same basename.

The decoder and page bitmap live in `core/`. The SwiftUI window in `app/` only displays that bitmap and calls the C API in `core/include/gs2print.h`. PDF pages are written with the vendored copy of [libHaru](https://github.com/libharu/libharu).

## Build

Requires Xcode with the macOS 26 SDK. zlib is the system library; nothing else is fetched at build time.

Open `gs2print.xcodeproj` and run the **gs2print** scheme.

From the command line:

```sh
xcodebuild -project gs2print.xcodeproj -scheme gs2print -configuration Debug -derivedDataPath build
open build/Build/Products/Debug/gs2print.app
```

The **gs2print_tests** scheme is a small command-line driver, not an XCTest bundle:

```sh
xcodebuild -project gs2print.xcodeproj -scheme gs2print_tests -configuration Debug -derivedDataPath build
./build/Build/Products/Debug/gs2print_tests
```

`build/` and Xcode’s `DerivedData/` are gitignored.

## Credits

The ImageWriter II glyph tables in `core/fonts/glyphs.cpp` come from [mikedaley/web-a2e](https://github.com/mikedaley/web-a2e), under `src/js/printer/` (`imagewriter-ii-rom-draft.js`, `imagewriter-ii-rom-nlq-fixed.js`, `imagewriter-ii-rom-nlq-prop.js`, and the standard-quality faces beside them). Those columns were transcribed from Appendix C of the ImageWriter II Technical Reference Manual through that project’s font editor. They are not a dumped printer ROM. web-a2e is MIT licensed.

libHaru is Copyright (C) 1999-2006 Takeshi Kanno and Copyright (C) 2007-2009 Antony Dovgal. See `third_party/libharu/LICENSE`.
