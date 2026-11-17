# Printer Emulation

Spec for ImageWriter II printing. Not implemented. Byte capture is what ships today. Tracked as [#51](https://github.com/jawaidbazyar2/gssquared/issues/51).

Research notes that led here: [Imagewriter.md](Imagewriter.md), [EpsonMX80.md](EpsonMX80.md), [Parallel.md](Parallel.md). The user-facing capture card is [Cards_Parallel.md](Cards_Parallel.md).

## Summary

GSSquared keeps capturing printer bytes the way it does now. It does not rasterize them. A separate printer app, which the user downloads on their own, turns a byte stream into a page on screen and a PDF.

A port uses it when its `[[connections]]` device is `gs2print`. GS2 starts the app only when a print job actually begins. It writes a small `.gs2print` ticket and asks the operating system to open it. The ticket names a local socket that GS2 is already listening on. The printer app connects, its window appears immediately, and GS2 streams the guest's bytes as they arrive. Replies the printer owes the guest (`ESC ?` and anything later) travel the same socket back to GS2 and into serial RX. Opened on its own, File → Open reads a raw printer dump and writes a PDF, with no emulator running.

The socket GS2 opens is a parent-chosen local name, not a fixed TCP port. macOS, Linux, and Windows all support that. The listen and connect calls live in a small platform library.

The same app can also listen, as a server the user starts. Another emulator, or a real Apple II with a serial-to-network adapter, then connects and sends the same raw bytes. That port is the user's choice and is shown in the window.

## What exists today

The parallel card (`card = "parallel"`, slots 1–7) is output-only. Guest writes to `$C0n0` enqueue `MESSAGE_DATA` on the same queue the Super Serial Card and the IIgs SCC ports use. A `FileDevice` thread drains the queue.

- Capture files are named `GS2.PARLn.YYYYMMDDHHMMSS`. Clipboard is the other attachment.
- The file opens on the first byte, closes after about 10 seconds of idle or on Ctrl-Reset (`MESSAGE_CLOSE`), and reopens on the next write.
- Closing shows an OSD toast with the filename.
- The main thread never blocks on file I/O.
- Config is `[[connections]]` with `device = "file"` or `device = "clipboard"`. Parallel allows `none`, `file`, and `clipboard`.

### Super Serial Card printer (interim)

`device = "printer"` is offered only on a Super Serial Card. It uses the same `FileDevice` as `device = "file"`: the dump is `GS2.SSCn.YYYYMMDDHHMMSS`, the file opens on the first byte, and it closes after about 10 seconds of idle or on Ctrl-Reset. While Printer is attached, DIPSW1 mode bits are SIC P8 (SW1-5 ON, SW1-6 OFF). With the 9600 default that value is `$ED`. Stock SSC firmware then does not send ETX (`$83`) or wait for ACK (`$86`). Other attachments leave DIPSW1 at `$EF`, which is SIC P8A, so a File capture of `PR#` output can be only `$83`. Firmware latches the P8 auto-CR flag at `PR#` / card init, so reset after selecting Printer.

This is the attachment that later opens the gs2print ticket below and still writes that disk dump. The ticket, socket, and sidecar are not implemented. SCC and parallel ports do not offer `printer`.

```toml
[[cards]]
slot = 1
card = "super_serial"

[[connections]]
slot = 1
device = "printer"
```

There is no ImageWriter, Epson, or PDF emulation in the emulator. The IIgs printer port (SCC channel A) uses the same device interface, so `gs2print` below covers it as well as the parallel card.

## Printer

Emulate the ImageWriter II. It was Apple's printer for this machine, and the Technical Reference Manual prints the character matrices. The Epson MX-80 / RX-80 ROM investigation ([EpsonMX80.md](EpsonMX80.md), `utils/mx80-rom-print.c`) is set aside. That ROM is an RX-80 dump, the glyphs are stored sideways with inverted bits and page-boundary padding, and the layout past the ASCII set was never settled.

Draw every page as a bitmap. Embedded PDF fonts would make smaller, selectable text, and ImageWriter-style fonts exist, but the printer has a user-defined character set, so a text font cannot represent every job. The PDF is a picture of the page.

The print head has 9 pins at 1/72 inch. Paper moves in 1/144 inch steps, which is how double-density output works. Graphics mode uses the top 8 pins, one byte per column. Buffer a page or more, because the paper can move up and down, not only forward.

Commands are ESC sequences: graphics, tabs, character set, colour (`ESC K n`), the two software-switch banks (`ESC Z a b` opens switches, `ESC D a b` closes them), and identification (`ESC ?`). An ESC followed by an unknown byte is ignored along with that byte.

### Identification

The GS/OS ImageWriter II driver (`NEW.IW/imager.aii`) opens with `ESC ?` (`$1B $3F`) and `_PrDevRead`s up to 8 bytes. A match is ASCII `IW10`, optionally preceded by one junk byte, then optional feature letters, then CR (`$0D`).

| Letter | Meaning |
| --- | --- |
| `C` | colour ribbon |
| `F` | sheet feeder |

No match is not a failure. If the read returns zero bytes, the driver assumes ImageWriter I and continues. ImageWriter I never answered `ESC ?`. The timeout is the port driver's short DevRead, about 2 seconds on a v4+ driver. That is why a capture-only attachment neither hangs nor fails: the probe times out and printing proceeds as an ImageWriter I. Colour and sheet-feeder features then stay off.

This emulation answers. The reply is `IW10C\r` (`$49 $57 $31 $30 $43 $0D`). `C` is always present because the colour ribbon is always installed. `F` is omitted; there is no sheet feeder. No leading junk byte. Six bytes fit the 8-byte read.

`ESC ?` does not move the head, change switches, or ink. File → Open and saved dumps still consume it with no visible effect; there is no host to reply to.

ImageWriter LQ is out of scope. Its driver sends `ESC Z $00 $02` then `ESC ?` and requires `LQ1`. Answering `IW10` does not satisfy that driver.

After ID, the same IW II driver resets and sets page length. Those are ordinary commands, already in this spec, not a second protocol:

```
ESC c             hardware reset (head recal)
ESC Z $07 $00     language / default switches
ESC H nnnn        page length in 1/144" (1584 is 11 inches)
```

They are not page content. Dots start after them.

Do not answer `ESC ?` inside GS2. The printer emulation is the printer. A loopback on the serial device would reply before the sidecar was up, and would then double-answer once it was.

### Fonts

Use the column tables from [mikedaley/web-a2e](https://github.com/mikedaley/web-a2e) `src/js/printer/`. They were transcribed from the ImageWriter II Technical Reference Manual through that project's font editor. They are not a dumped ROM. The repo is MIT.

`imagewriter-ii.js` loads three of them:

| File | Export | Face |
| --- | --- | --- |
| `imagewriter-ii-rom-draft.js` | `IW2_DRAFT_ROM` | Draft. 12 columns, last column always blank. Appendix C. |
| `imagewriter-ii-rom-nlq-fixed.js` | `IW2_NLQ_FIXED` | Near-letter-quality, fixed pitch. 16 columns, up to 18 bits. |
| `imagewriter-ii-rom-nlq-prop.js` | `IW2_NLQ_PROP_ROM` | NLQ proportional. Variable width; the trailing blank column is the spacer. |

Each glyph is an array of column values, left to right. Bit 0 is the top wire, bit 7 is wire 8, and bit 8 (values above `0xFF`) is wire 9, the descender. Codes cover `$20`–`$7E` and MouseText `$C0`–`$DF`. Locale tables (UK, IT, DE, SE, FR, ES, DK) replace `$23`, `$40`, `$5B`–`$5D`, `$60`, and `$7B`–`$7E`.

Draft is the face to implement first. `imagewriter-ii-rom-standard-fixed.js` and `imagewriter-ii-rom-standard-prop.js` are in the same directory and are not loaded by that class.

## Core

One library turns printer bytes into pages. The platform frontends only display the current page and own the window, the socket, and the menus. The library does not open a window or a socket.

Write it in C++. The printer is a stateful object: head position, switch banks, tabs, the custom character set, and the page under the head. That is the same language as GSSquared, and libHaru's C API calls from C++ directly.

Export a small C API for the frontends. Swift and AppKit on macOS, a Win32 window on Windows, and GTK on Linux can all call C. They should not depend on the C++ ABI. The C functions are: feed bytes, take any printer-to-host reply those bytes produced, ask whether the job painted ink, read the current page planes (pointer, width, height, stride, one plane at a time), and finish the job.

`gs2_job_write` never waits. A command split across calls stays in the decoder until a later write completes it. `ESC ?` split across two writes emits the reply on the write that supplies `?`. After each write on a live or TCP session, the frontend copies pending reply bytes with `gs2_job_take_reply(job, buf, cap)` and writes them on the same connection, toward the guest. The call copies at most `cap` bytes, returns how many, and leaves the rest. File → Open does not take replies. `gs2_job_did_print` is 1 after any ink, else 0. A live session writes a PDF only when that is set.

The head is placed at 1440 dpi. That grid divides the wire pitch (1/72 inch), the paper step (1/144 inch), and the graphics densities 72, 80, 96, 120, 144, and 160. The stored page is 720 dpi, four packed 1-bit planes, one per ribbon band: each dot rectangle is rounded onto that grid when it is drawn, two grid units per pixel. The page under the head is uncompressed, so dots can be painted as bytes arrive. A form feed, or the end of the job, commits that page. The core deflates each plane with a PNG Up row predictor and starts a fresh uncompressed page. Reading a committed page inflates it. The frontend redraws from the planes as bytes arrive, so the preview is the page in progress.

### PDF

libHaru writes the PDF when the job ends, at `GS2-Print-YYYYMMDD-HHMMSS.pdf`. Each page is four 1-bit images of the 720 dpi planes, Flate-compressed with PNG predictor 12, the same Up filter as a committed page. The images are drawn at 8.5 by 11 inches as PDF layers. There is no embedded font. The on-screen preview composites the four planes; the frontend does not re-read the PDF to draw it.

## Connection

The attachment is a `[[connections]]` device named `gs2print`, next to `file` and `clipboard`. The user picks it in the config editor and in the Control Panel port button. File and clipboard capture stay as they are. Until that ships, the SSC test attachment is `device = "printer"` (above). It captures to disk and does not open a ticket. That same `printer` device is what later opens the ticket and still writes the dump.

```toml
[[cards]]
slot = 1
card = "parallel"

[[connections]]
slot = 1
device = "gs2print"
```

Allowed on a parallel card and on a serial port used as a printer (IIgs SCC channel A, slot 1, and a Super Serial Card). It is not a modem and it is not offered in the web build. The platform default stays `file`; `gs2print` is selected, not assumed.

With `device = "gs2print"`, the first byte of a job runs the ticket and socket flow in this document. With `file` or `clipboard`, nothing about the sidecar runs.

## Sidecar

The printer app is a separate download. GSSquared does not ship it, spawn a copy beside its own binary, or link it in. The web build has no sidecar; it keeps file and clipboard capture.

The app has three jobs:

- A live GS2 job: connect to GS2, show the page as bytes arrive, write a PDF when the job ends.
- File → Open: read a raw printer dump and write a PDF, with no emulator running.
- A server: listen for a raw TCP connection from anything else that can open one.

The window is a small native UI on each platform: the composited page from the core, and a button that closes the job. After a PDF is written, its file name is shown at the bottom of the window. That name is a link. Clicking it asks the operating system to open the PDF (`open` on macOS, `ShellExecute` on Windows, `xdg-open` on Linux).

Settings… opens a window with one section per printer. ImageWriter’s first switch is SW1-8, line feed after carriage return. It ships open, so a carriage return returns the head and does not feed. The saved position is the power-on DIP for the next job. A job already printing keeps the position it started with. The guest can still close or open that switch with `ESC D` / `ESC Z`.

The preview shows one page at a time. Its scale buttons are +, −, fit width, and fit height. + draws the page larger and − draws it smaller. Fit width scales the page so its width matches the view; fit height scales it so its height matches the view. While either fit is selected, resizing the window changes the scale to keep that fit. + or − leaves fit and holds the scale the user picked. The view of each page scrolls horizontally and vertically, so the user can move to any part of a page drawn larger than the window. Scaling and scrolling are display only. The planes and the PDF stay at 720 dpi.

The sidecar registers itself as the handler for `.gs2print` when the user installs or first runs it. That is a UTI in its macOS bundle, an HKCU ProgID on Windows (same idea as [ProtocolHandlers.md](ProtocolHandlers.md) for `.gs2`), and a `.desktop` MIME type on Linux. A zip or AppImage only gains the association after that registration.

### When a job starts

Nothing is launched at emulator startup. The first byte after the port has been idle starts the job, which is the same moment `FileDevice` opens a capture file. For the ImageWriter II driver that byte is often `ESC ?`, during "Establishing communication", not page data. The ticket still starts then: the sidecar has to be on the socket to answer before DevRead expires. The window may appear on a blank page. That is the handshake, not a defect.

1. GS2 picks a fresh local socket name, binds, and listens.
2. GS2 writes a ticket (below) in the temp directory.
3. GS2 asks the operating system to open that file (`open` on macOS, `ShellExecute` on Windows, `xdg-open` on Linux).
4. The association starts the printer app, or delivers the file to one that is already running. The app reads the ticket and connects.
5. GS2 accepts, writes any guest bytes it held, then writes further bytes as they arrive. The window is up before any dots are drawn. Replies the sidecar writes are read by GS2 and placed in the serial port's RX path.

GS2 listens, and the sidecar connects. The operating system starts the app, so GS2 has no pipe to it and cannot wait on its stdout. Binding before the open means the sidecar finds a listener when it starts. Guest TX that arrives before accept is held in order and flushed on accept, so a cold start that connects inside the DevRead window still replies. If nothing connects in time, the driver falls through to ImageWriter I, the same as a missing reply on hardware. A second job against an already-running sidecar is fast.

Each job gets its own socket name, so two emulators, or two jobs in a row, do not share an endpoint. A second `.gs2print` opened while a window is already up is another job: macOS delivers it to the running app, and Windows and Linux normally start another process. Either way, one connection is one job.

### Socket

The name is a filesystem path GS2 invents, such as a file under the temp directory that includes the process id and a counter. There is no well-known TCP port.

The same idea works on macOS, Linux, and Windows. A platform library exposes listen, connect, read, write, and close, and hides the rest:

- macOS and Linux: an `AF_UNIX` `SOCK_STREAM` socket at that path.
- Windows: the same `AF_UNIX` socket (Winsock, Windows 10 version 1803 and later — the floor [gs2debug.md](gs2debug.md) already documents), or a `\\.\pipe\...` pipe with a generated name.

The path must fit in `sun_path`. Both ends must see the same path. Under MSYS2 that is a Windows path, because a `/tmp/...` path is translated by only one of the two processes.

The socket is full duplex. Guest printer bytes travel GS2 to the sidecar. Printer-to-host bytes travel sidecar to GS2. The ticket already carries the printer model, so the socket has no header. Closing the connection ends the job.

GS2 copies sidecar-to-GS2 bytes into the serial receive path the guest already uses (`_PrDevRead`, Super Serial Card RX, IIgs SCC channel A RX). It does not wait for a full reply; each `read` that returns data is injected as it arrives. A parallel card is output-only. Replies are still produced on that attachment and are discarded; they never reach the guest.

The sidecar writes a reply as soon as `gs2_job_write` completes the command that produced it, without waiting for more guest bytes. `ESC ?` is two bytes and the reply is six. That has to land in the guest before the port driver's DevRead expires.

### `.gs2print` file

One extension, two contents. The first line is the magic `GS2PRINT 1`. The header is `key value` lines. A blank line ends the header.

A live ticket:

```
GS2PRINT 1
printer imagewriter-ii
socket /tmp/gs2-print-12345-1.sock

```

A saved dump uses the same header and then the raw byte stream:

```
GS2PRINT 1
printer imagewriter-ii

<raw printer bytes>
```

`printer` is `imagewriter-ii` for this spec.

### File → Open

File → Open takes a raw printer dump, the headerless byte stream a `file` capture writes (`GS2.PARLn.*` or any other file of printer bytes). The frontend feeds the whole file to the core, shows the pages, and the core writes the PDF beside the dump, with the same basename and a `.pdf` extension, only if the dump painted ink. A probe-only dump (`ESC ?`, or that plus reset and page length) opens with no PDF. A `.gs2print` saved dump (header, then bytes) opens the same way. No socket and no emulator are involved.

### When the job ends

The job ends when any of these happen:

- About 60 seconds pass with no bytes. Guest software can pause that long in the middle of one job.
- Ctrl-Reset sends `MESSAGE_CLOSE`.
- The user presses the sidecar's close-job button, which closes its end of the socket.
- GS2 closes its end.

If the job painted ink, the sidecar finishes the page and the core saves the libHaru document to `GS2-Print-YYYYMMDD-HHMMSS.pdf` somewhere the user can find it (Documents is the default). The window stays open on the finished page. The saved file name is the link at the bottom of the window. The next guest byte starts a new ticket and a new connection.

A handshake that never inked (`ESC ?`, or that plus `ESC c` / `ESC Z` / `ESC H`) is not a print job. The sidecar still answers `ESC ?`. It does not write a PDF. The window returns to the idle prompt.

### Server mode

Server mode is for clients that were not launched by a `.gs2print` ticket and will never write one. The user starts the printer app and turns listening on. The window shows the address. Another emulator that can open a TCP connection, or a real Apple II whose serial adapter opens one, is pointed at that address.

The bytes on the connection are the printer bytes, full duplex, with no header. Guest TX is what the client writes. Printer replies are what this app writes back on the same connection. One connection is one job. Closing it ends the job. About 60 seconds with no bytes also ends it, because a serial-to-network adapter often holds its TCP connection open across what the Apple II thinks are separate jobs. The next bytes start another page.

A client that only sends and never reads still works. The replies sit in the TCP window until the client reads or closes. Software that sends `ESC ?` and reads the serial data path gets `IW10C\r`.

The port is stored in the app's settings and shown in the window. Port 9100 is the usual starting choice, because that is the ordinary "send raw bytes, then close" printer port. If it is already taken, the window says so and the user picks another. Nothing in GS2 or the sidecar assumes a compiled-in port for the ticket path above.

Listening is on localhost until the user opts in to the LAN. A real machine on the network is the reason to opt in. The adapter copies UART bytes onto the TCP connection and handles baud rate and the printer's busy line on the Apple II side. The adapter must also copy bytes the TCP server writes back onto the Apple II's serial RX. An adapter that is TX-only makes `ESC ?` time out, and the IW II driver then continues as ImageWriter I.

A GS2 ticket still works while the server is running. The app connects out to GS2's local socket for that job, and accepts inbound TCP for the others. The renderer sees one byte stream either way.

### When the sidecar is not installed

Opening the ticket fails, or nothing connects within a few seconds. GS2 shows an OSD toast saying the printer app is not installed, and keeps writing the raw capture file it already writes. The user can open that file in the sidecar later. Guest software is not stalled on the missing app. `ESC ?` times out, and the IW II driver continues as ImageWriter I.

## Colour

The ImageWriter II colour ribbon has four bands: yellow, magenta, cyan, and black. `ESC K n` selects a colour. `n` is an ASCII digit, `$30`–`$36`.

| `n` | Colour | Planes |
| --- | --- | --- |
| `0` | Black | K |
| `1` | Yellow | Y |
| `2` | Magenta | M |
| `3` | Cyan | C |
| `4` | Orange | Y and M |
| `5` | Green | Y and C |
| `6` | Purple | M and C |

The owner's manual names magenta "red" and cyan "blue". Those are the same two bands. Power-on colour is black. Combined colours print yellow first, then the other band, matching the Technical Reference Manual.

A real printer activates these commands only when a colour ribbon is installed. The emulation always has one. Software can still overprint: a carriage return with no line feed, then another `ESC K`, paints the same dots in a second band.

Each plane is a 720 dpi packed 1-bit bitmap, bit 7 leftmost, two grid units per pixel. Painting ORs into the selected plane or planes. A later hit on the same pixel keeps the earlier ink. The four bits are independent, so orange is Y and M set, not a fifth colour value.

The page under the head keeps all four planes uncompressed. A form feed, or the end of the job, deflates each plane with a PNG Up row predictor. An unused plane compresses to almost nothing, so a black-only job stays cheap. The C API returns one plane at a time. The frontend composites the four planes for the preview.

Each PDF page is four 1-bit images, one per plane, Flate-compressed with PNG predictor 12. They are PDF optional-content layers named Yellow, Magenta, Cyan, and Black, drawn in that order at 8.5 by 11 inches. Each image is ink-as-1, painted in that band's colour, and multiply-composited onto white paper so overlapping inks mix subtractively. The on-screen preview composites the same four planes; the frontend does not re-read the PDF to draw it.
