# Windows Build

Spec for the Windows app. The macOS app in `app/` is the other build. Printer behavior, the ticket format, and the C API are in [PrinterEmulation.md](PrinterEmulation.md) and `core/include/gs2print.h`. This document is the Win32 window, the sockets, and the build that produces one executable.

The core stays a C++ library with a C API. The Windows program links that library and does not reimplement the decoder. It owns the window, the menus, the TCP listener, the ticket connection, the registry settings, and the `.gs2print` association.

## Toolchain

Windows 10 version 1903 or later, 64-bit. Version 1803 is enough for `AF_UNIX`. 1903 is the floor because the process manifest sets the active code page to UTF-8, and `gs2_job_save_pdf` passes that path to libHaru's `fopen`.

Build Tools for Visual Studio 2022, with the Desktop development with C++ workload: the MSVC toolset, the Windows 10 SDK, CMake, and Ninja. The Visual Studio IDE is not installed and not used. CMake 3.24 or later drives the build. `build.ps1` loads that toolset and configures Ninja. There is no Visual Studio solution.

Nothing is downloaded at build time. No vcpkg, FetchContent, NuGet, MSYS2, or MinGW. A MinGW or MSYS2 binary would translate `/tmp` paths on one side of the ticket socket and not the other. The executable is a normal Win32 process, so the path in the ticket is the path it connects to.

The Xcode project remains the macOS build. CMake does not produce the Swift app.

The only architecture is x64.

## Layout

```
CMakeLists.txt
build.ps1
third_party/zlib/          zlib 1.3.1, committed
win/main.cpp               entry, single instance, command line
win/app.cpp                jobs, file classification, which window is idle
win/session.cpp            one job: core handle, preview, PDF path
win/window.cpp             the job window and its controls
win/preview.cpp            1-bit page to an 8-bit DIB, then StretchBlt
win/server.cpp             TCP listener
win/ticket.cpp             connect to the ticket socket and read
win/settings.cpp           registry
win/associate.cpp          HKCU ProgID for .gs2print
win/app.manifest
win/gs2print.rc
```

`core/`, `tests/main.cpp`, and `third_party/libharu/` stay as they are. `build/` is already gitignored.

## Targets

Three CMake targets.

`gs2print_core` is a static library. It compiles `core/src/*.cpp`, `core/fonts/glyphs.cpp`, every `.c` file under `third_party/libharu/src`, and the zlib sources (`adler32.c`, `compress.c`, `crc32.c`, `deflate.c`, `inffast.c`, `inflate.c`, `inftrees.c`, `trees.c`, `uncompr.c`, `zutil.c`). Include `core/include` publicly and `core/src`, `core/fonts`, `third_party/libharu/include`, and `third_party/zlib` privately. Define `LIBHPDF_HAVE_ZLIB=1`. Leave `LIBHPDF_HAVE_LIBPNG` unset. The checked-in `third_party/libharu/include/hpdf_config.h` already matches that. C++20 for the core (`/std:c++20`). libHaru and zlib compile as C. `/utf-8` on every file. `/W4` on `core/` and `win/`. Warnings in `third_party/` are not errors, and those files define `_CRT_SECURE_NO_WARNINGS`.

`gs2print_tests` is a console program. It compiles `tests/main.cpp` and links `gs2print_core`. It is the same driver the Xcode scheme `gs2print_tests` builds. Exit 0 prints `all passed`. Exit 1 prints the failure count.

`gs2print` is a Windows-subsystem executable (`WIN32`, no console). It compiles `win/*.cpp` and `win/gs2print.rc`, and links `gs2print_core` plus `ws2_32`, `gdi32`, `user32`, `comctl32`, `shell32`, `ole32`, and `uuid`. Compile with `UNICODE` and `_UNICODE`. The C API stays narrow UTF-8; the UI converts at the boundary with `WideCharToMultiByte(CP_UTF8)` and `MultiByteToWideChar(CP_UTF8)`.

Release links the static CRT (`/MT`) so the zip is one file. Debug links the debug DLL CRT and is for local runs. Do not ship a Debug build.

`build.ps1` finds Build Tools, loads the x64 environment, and builds Release with Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\gs2print_tests.exe
```

The program is `build\gs2print.exe`. Those cmake lines are what the script runs after `vcvars64.bat`. Run the script from a normal PowerShell window. The compiler is `cl.exe`. No IDE is opened.

## Manifest

`win/app.manifest` is embedded through the resource file. It requests `asInvoker`, common-controls version 6, Per-Monitor V2 DPI awareness, and the UTF-8 active code page:

```xml
<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
<dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
```

`win/gs2print.rc` also carries a version block. `ProductName` is `gs2print`. `FileDescription` is `GS2 printer`.

`gs2_job_save_pdf` is unchanged. Its `const char *` path is UTF-8 on every platform. The manifest is what makes libHaru's `fopen` open a non-ASCII Documents path.

## One process

Windows starts a new process for each double-clicked `.gs2print`. This program collapses that to one process, because one process owns the TCP port and the job windows share it. macOS already delivers a second file to the running app. The result is the same: one connection is one job, and a second ticket opens another window.

The first process creates a mutex named `Local\net.gssquared.gs2print` and a message-only window of class `gs2print.ipc`. It does both before showing a job window. A later process waits on the mutex. `WAIT_ABANDONED` means the previous process died, and this process becomes the owner. `WAIT_TIMEOUT` is not used; if the mutex is held, this process looks up `gs2print.ipc` for up to two seconds. It then sends a `WM_COPYDATA` whose payload is the UTF-16 command line arguments, calls `AllowSetForegroundWindow` on the first process, and exits 0. The first process treats that payload the same way it treats its own command line.

Launched with no path while a copy is already running, the second process sends an empty payload. The first process brings the primary window forward and does not open a dialog.

`SetCurrentProcessExplicitAppUserModelID(L"net.gssquared.gs2print")` runs at startup so the job windows share one taskbar group.

## Command line

`CommandLineToArgvW`. Skip `argv[0]`. Each remaining argument is a file. No argument opens the primary window with the idle status `Open a printer dump.`

The working directory is irrelevant. A dump's PDF is written beside the dump. A live job's PDF is written in the user's Documents folder (`FOLDERID_Documents`).

## File association

On every launch the program writes its HKCU registration, so a moved exe updates the command. No administrator rights, and no MSI. A zip of `gs2print.exe` is the install. The association appears after the first run, which is the rule in PrinterEmulation.md.

```
HKCU\Software\Classes\.gs2print
  (Default) = net.gssquared.gs2print.ticket

HKCU\Software\Classes\net.gssquared.gs2print.ticket
  (Default) = GS2 printer ticket
  shell\open\command
    (Default) = "<full path of this exe>" "%1"
```

The ProgID matches the macOS UTI `net.gssquared.gs2print.ticket`. The friendly name matches `CFBundleTypeName` in `app/Info.plist`.

## Settings

Registry values live at `HKCU\Software\net.gssquared.gs2print`. Missing values use the defaults. The names match the macOS `UserDefaults` keys.

| Value | Type | Default | Meaning |
| --- | --- | --- | --- |
| `imagewriter.autoLineFeed` | DWORD | 0 | Power-on SW1-8. 0 returns the head on CR. 1 also feeds a line. |
| `serverListen` | DWORD | 0 | Open the TCP port at startup. |
| `serverPort` | DWORD | 9100 | TCP port, 1 through 65535. |
| `serverLAN` | DWORD | 0 | 0 binds localhost. 1 binds every interface. |

A job reads `imagewriter.autoLineFeed` once, in `gs2_job_set_auto_lf`, before the first `gs2_job_write`. Changing the switch later affects the next job. `ESC D` and `ESC Z` still override it for the job that is already running.

Settings… is a menu item on every job window. The dialog has one section, ImageWriter, and one checkbox: `SW1-8  Line feed after carriage return`.

## Windows

The primary window is created at startup. Its title is `gs2print`. Further jobs get their own windows, titled the same, when the primary window is not idle. A window is idle when it has no ticket, no TCP client, no open network job, no pages, no PDF, and no error.

Closing a window finishes that job and drops its core object. Closing the last window exits the process. Closing the primary window while another job is open leaves the process and the TCP listener running.

The minimum client size is 720 by 640 device-independent pixels. `WM_DPICHANGED` resizes to the suggested rect.

Each window shows the same controls. The server bar is shared state, so a change on one window updates the others.

Top to bottom, when a page is showing:

- Status line. `Printing` while bytes are still arriving, with a marquee progress control beside it for that same span. A finished job shows the PDF file name alone. An idle window shows `Open a printer dump.`
- The page, white on a light gray viewport, centered when it is smaller than the viewport. Scrollbars appear only when the page overflows.
- Previous, `Page N of M`, Next, then `−`, `+`, Fit Width, Fit Height.
- `Wrote` and the PDF file name as a link, after a save succeeds.
- The error line, in red, when there is one.
- A separator, then the server bar: Listen, a port field, Other computers, the address, the bind error, and Close Job.

Previous and Next are disabled at the ends. Close Job is enabled when this window holds a ticket, or holds the TCP client and that client is still connected.

File → Open… (`Ctrl+O`) uses the common item dialog. The filter offers `.gs2print` and all files. The dialog is owned by the window that issued it, and the dump opens in that window.

The strings the macOS app already shows are the strings here:

- `Open a printer dump.`
- `Connecting`
- `Printing`
- `Could not start a print job.`
- `Could not write <name>.`
- `Could not open <name>.`
- `Socket path is too long.`
- `Unsupported printer.`
- `Port must be between 1 and 65535.`
- `Port <n> is already in use.`
- `Could not listen on port <n>.`
- `Could not connect to <path>: <system message>.`

## Opening a file

Finder and File → Open both go through one function. The same path delivered twice inside half a second is ignored. Entries older than two seconds are dropped. A ticket whose socket already has a live client is ignored.

Read the whole file, then classify it with `gs2_ticket_field`. Do not parse the header in the UI.

- `socket` does not fit in 4096 bytes: `Socket path is too long.`
- `socket` is absent or empty: the file is a dump.
- `printer` is not exactly `imagewriter-ii`: `Unsupported printer.`
- otherwise it is a live ticket. Connect to that path.

A rejected ticket or a live ticket claims an idle window (the primary, else an idle extra, else a new window) and raises it. A dump opens in the focused window, or the primary when none is focused.

A dump feeds the core on the UI thread. `gs2_dump_payload_offset` skips a `GS2PRINT 1` header; a raw capture returns 0 and the whole file is the payload. Then `gs2_job_set_auto_lf`, `gs2_job_write`, `gs2_job_finish`. The PDF is written beside the dump. Take the last path component. If it has a final dot and the suffix after that dot is 1 to 8 characters, replace the suffix with `pdf`. Otherwise append `.pdf`. `GS2.PARL1.20241101120000` becomes `GS2.PARL1.20241101120000.pdf`. `dump.gs2print` becomes `dump.pdf`.

## Preview

`gs2_job_page_bits` returns a packed 1-bit bitmap, 720 dpi, 6120 by 7920, stride 765, bit 7 leftmost. The pointer is valid only until the next `gs2_job_page_bits`, `gs2_job_write`, or `gs2_job_finish`. The UI thread copies those bytes out before it returns to the queue. Expanding to the DIB happens on a worker, one page at a time. A newer request waits until that worker finishes, and a generation counter drops a result that belongs to a job that has since been replaced.

The DIB is 8-bit gray, one byte per printer pixel, 255 for paper and 0 for ink. That is the full page, about 48 MB. The view scales it. There is no baked-down preview bitmap.

Paint with `StretchBlt`. When the scale times the window's DPI scale is at least 1, use `COLORONCOLOR` so printer pixels stay crisp. Otherwise use `HALFTONE`, and call `SetBrushOrgEx` after setting that mode. The page background is white.

Scale is display only. The bitmap and the PDF stay at 720 dpi.

The default mode is fit height. Fit width uses the viewport width. Fit height uses the viewport height. Either fit is recomputed on `WM_SIZE`. `+` and `−` multiply the current scale by 1.25 or by 1/1.25, leave fit, and hold that scale across resizes. Fixed zoom keeps the page's long side between 80 and 4000 device-independent pixels. A button that would not move the scale is disabled.

The viewport scrolls horizontally and vertically. Changing the page index resets the scroll origin to the top left. Zooming keeps the origin.

While a network job is open, preview updates are coalesced to 10 Hz. The window follows the last page until the user steps away. Opening a dump shows page 1 and does not follow.

## Live ticket

`ticket.cpp` connects and reads. It does not bind, listen, or delete the socket. GS2 owns the name.

The window shows `Connecting` and comes forward as soon as the file is accepted, before any printer bytes. On connect it shows `Printing`.

A path that begins with `\\.\pipe\` or `\\?\pipe\` is opened with `CreateFileW` for generic read, existing only. Any other path is an `AF_UNIX` `SOCK_STREAM` socket. Call `WSAStartup` once at process start. Fill `sockaddr_un` with `sun_family = AF_UNIX` and copy the UTF-8 path, including the NUL, into `sun_path`. `sun_path` holds 108 bytes. A path that does not fit fails with `Socket path is too long.` The length passed to `connect` is the offset of `sun_path` plus the path bytes plus the NUL. Windows `sockaddr_un` has no `sun_len`.

The read is a blocking `recv` on a worker thread, 16 KB at a time. A readiness wait can miss the peer's close, and the job would then never write its PDF. `shutdown(SD_BOTH)` from the UI thread unblocks that `recv`. A pipe uses `ReadFile` and `CancelIoEx` the same way.

Ten seconds with no bytes shuts the connection down. That is the same idle close as a capture file. The worker then reports the close, and the UI finishes the job. GS2 closing its end, or the user pressing Close Job, finishes it the same way. Zero bytes received produces no PDF.

On the first byte, replace any previous core object, call `gs2_job_new` and `gs2_job_set_auto_lf`, and start the preview. Further bytes call `gs2_job_write` on the UI thread only.

## TCP server

`server.cpp` is the raw printer port from `app/PrintServer.swift`. One accepted client at a time. A second connection is accepted and closed. Ten seconds of silence finishes the job in the window and leaves the socket open, so the next byte starts another job on that same connection. Closing the client, or Close Job, finishes the job and drops the client. The listener stays up until Listen is turned off.

Listen opens an IPv4 socket and an IPv6 socket. Both are nonblocking, `SO_REUSEADDR`, listen backlog 8. The IPv6 socket sets `IPV6_V6ONLY`. Localhost uses `127.0.0.1` and `in6addr_loopback`. Other computers uses `INADDR_ANY` and `in6addr_any`. If the IPv6 bind fails because the port is taken, close IPv4 and report the port in use. If IPv6 fails for any other reason, keep IPv4. One thread polls both listeners and the client with `WSAPoll`. The idle deadline is the poll timeout, reset on each received buffer.

The address line is `127.0.0.1:<port>` when Other computers is off. When it is on, the line is the up, non-loopback IPv4 addresses, sorted, one per line, each with the port. If none are found, the line is `0.0.0.0:<port>`.

The port field accepts 1 through 65535. Any other text sets `Port must be between 1 and 65535.` and does not change the stored port. Pressing Enter on an unchanged port retries the bind. That is how a user reclaims a port that was busy. Turning Listen on commits the field first. Turning Listen off clears the address and the bind error.

A ticket still connects out while the server is listening. The core sees one byte stream either way.

## PDF

`gs2_job_finish`, then `gs2_job_save_pdf`. A live job, ticket or TCP, writes `GS2-Print-YYYYMMDD-HHMMSS.pdf` in Documents, using local time. If that name exists, append `-2`, then `-3`, and so on. The stamp uses the digits `yyyyMMdd-HHmmss`.

The link at the bottom of the window is the file name. Clicking it calls `ShellExecuteW` with the verb `open`. Failure sets `Could not open <name>.` The window stays on the finished page. The next bytes of a still-open TCP connection start a new job in that window. The next ticket is a new connection.

## What stays put

`core/include/gs2print.h` gains no Windows types and no wide-character path. The ticket text stays `GS2PRINT 1`, `printer imagewriter-ii`, and a `socket` line. The Xcode schemes, `build.sh`, and `app/` stay the macOS program.
