# gs2print

macOS app that turns an ImageWriter II byte stream into a page preview and a PDF. Open a raw dump (`GS2.SSC*`, `GS2.PARL*`, or any headerless printer capture) or a saved `GS2PRINT 1` dump. The PDF is written next to that dump, with the same basename. A live ticket names a local socket; the PDF for that job is saved in Documents.

The decoder and page bitmap live in `core/`. The SwiftUI window in `app/` only displays that bitmap and calls the C API in `core/include/gs2print.h`. PDF pages are written with the vendored copy of [libHaru](https://github.com/libharu/libharu).

## Build

### MacOS

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

### Windows

On Windows, install Build Tools for Visual Studio 2022 with the C++ workload, CMake, and Ninja. The Visual Studio IDE is not required. From the repo root:

```powershell
.\build.ps1
```

That builds `build\gs2print.exe` and runs `build\gs2print_tests.exe`. See [WindowsBuild.md](WindowsBuild.md).

## Using with Emulators

### GSSquared

GSSquared works by creating a "Ticket" file named "something.gs2print", then asking the operating system to open that file. As long as you have run gs2print once before, it will be registered with your operating system. Then gs2print will open a connection back to GSSquared for the print session.

### KEGS

KEGS lets you specify a network IP address and port for outbound serial port connections to go to. This seems to work fine by specifying either 127.0.0.1:9100 or IP.AD.DR.ESS:9100. The port is arbitrary and can be set in gs2print Settings.

I did notice however that if KEGS can't connect to the port when it tried, that it does not seem to retry automatically and you have to go back to F4 Serial Port setup and modify it to get it to retry.

## Credits

The ImageWriter II glyph tables in `core/fonts/glyphs.cpp` come from [mikedaley/web-a2e](https://github.com/mikedaley/web-a2e), under `src/js/printer/` (`imagewriter-ii-rom-draft.js`, `imagewriter-ii-rom-nlq-fixed.js`, `imagewriter-ii-rom-nlq-prop.js`, and the standard-quality faces beside them). Those columns were transcribed from Appendix C of the ImageWriter II Technical Reference Manual through that project’s font editor. They are not a dumped printer ROM. web-a2e is MIT licensed.

libHaru is Copyright (C) 1999-2006 Takeshi Kanno and Copyright (C) 2007-2009 Antony Dovgal. See `third_party/libharu/LICENSE`.

The Windows build vendors zlib 1.3.1. See `third_party/zlib/LICENSE`.
