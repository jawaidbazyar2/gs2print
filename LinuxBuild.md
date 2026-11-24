# Linux Build

The Linux target is a GTK 4 desktop application using the existing C++ print engine. It opens raw or saved printer dumps, displays their pages, and writes a PDF beside the input file when the dump contains printed output. The Linux frontend currently handles local dump files; the macOS and Windows live TCP server and ticket-socket workflows are not part of this target.

## Requirements

- 64-bit x86 Linux
- CMake 3.24 or later
- Ninja
- A C++20 compiler
- GTK 4 development files and `pkg-config`

No dependencies are downloaded by CMake. The decoder, PDF writer, and zlib are built from the sources in the repository.

## Build and run

From the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/gs2print
```

The executable is `build/gs2print`. Open a dump from the app's Open button or pass its path when launching:

```sh
./build/gs2print path/to/printer-dump.gs2print
```

The app saves printed output as a PDF beside the input dump. When the input has a short file extension, that extension is replaced with `.pdf`; otherwise `.pdf` is appended.
