#include "preview.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr double kMinLongSide = 80;
constexpr double kMaxLongSide = 4000;

struct Job {
    std::vector<uint8_t> bits;
    int width = 0;
    int height = 0;
    int stride = 0;
    uint64_t generation = 0;
};

HBITMAP expandGray(const uint8_t *bits, int width, int height, int stride) {
    if (width <= 0 || height <= 0 || stride <= 0) return nullptr;
    struct Info {
        BITMAPINFOHEADER header;
        RGBQUAD colors[256];
    } info{};
    info.header.biSize = sizeof(BITMAPINFOHEADER);
    info.header.biWidth = width;
    info.header.biHeight = -height;
    info.header.biPlanes = 1;
    info.header.biBitCount = 8;
    info.header.biCompression = BI_RGB;
    for (int i = 0; i < 256; ++i) {
        info.colors[i].rgbRed = static_cast<BYTE>(i);
        info.colors[i].rgbGreen = static_cast<BYTE>(i);
        info.colors[i].rgbBlue = static_cast<BYTE>(i);
    }
    void *pixels = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO *>(&info), DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib || !pixels) return nullptr;
    int rowBytes = (width + 3) & ~3;
    auto *out = static_cast<uint8_t *>(pixels);
    std::memset(out, 255, static_cast<size_t>(rowBytes) * static_cast<size_t>(height));
    int packed = (std::min)(stride, (width + 7) / 8);
    for (int y = 0; y < height; ++y) {
        const uint8_t *src = bits + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint8_t *dest = out + static_cast<size_t>(y) * static_cast<size_t>(rowBytes);
        for (int byte = 0; byte < packed; ++byte) {
            uint8_t value = src[byte];
            if (!value) continue;
            int x0 = byte * 8;
            int n = (std::min)(8, width - x0);
            uint8_t mask = 0x80;
            for (int bit = 0; bit < n; ++bit) {
                if (value & mask) dest[x0 + bit] = 0;
                mask = static_cast<uint8_t>(mask >> 1);
            }
        }
    }
    return dib;
}

}  // namespace

struct PagePreview::Worker {
    std::mutex mu;
    std::condition_variable cv;
    std::thread thread;
    bool quit = false;
    bool hasWork = false;
    Job job;
    HBITMAP ready = nullptr;
    uint64_t readyGeneration = 0;
    int readyW = 0;
    int readyH = 0;
    HWND hwnd = nullptr;

    explicit Worker(HWND window) : hwnd(window) {
        thread = std::thread([this] { loop(); });
    }

    ~Worker() { stop(); }

    void stop() {
        {
            std::lock_guard lock(mu);
            quit = true;
        }
        cv.notify_all();
        if (thread.joinable()) thread.join();
        std::lock_guard lock(mu);
        if (ready) {
            DeleteObject(ready);
            ready = nullptr;
        }
    }

    void submit(Job next) {
        {
            std::lock_guard lock(mu);
            job = std::move(next);
            hasWork = true;
        }
        cv.notify_one();
    }

    bool take(HBITMAP *bmp, int *w, int *h, uint64_t *generation) {
        std::lock_guard lock(mu);
        if (!ready) return false;
        *bmp = ready;
        *w = readyW;
        *h = readyH;
        *generation = readyGeneration;
        ready = nullptr;
        return true;
    }

    void loop() {
        for (;;) {
            Job current;
            {
                std::unique_lock lock(mu);
                cv.wait(lock, [&] { return quit || hasWork; });
                if (quit && !hasWork) return;
                current = std::move(job);
                hasWork = false;
            }
            HBITMAP bmp = expandGray(current.bits.data(), current.width, current.height, current.stride);
            HWND target = nullptr;
            {
                std::lock_guard lock(mu);
                if (quit) {
                    if (bmp) DeleteObject(bmp);
                    return;
                }
                if (ready) DeleteObject(ready);
                ready = bmp;
                readyW = current.width;
                readyH = current.height;
                readyGeneration = current.generation;
                target = hwnd;
            }
            if (target) PostMessageW(target, WM_PREVIEW, 0, 0);
        }
    }
};

void PagePreview::attach(HWND window) {
    hwnd = window;
    if (!worker) worker = new Worker(window);
}

void PagePreview::shutdown() {
    if (!worker) return;
    worker->stop();
    delete worker;
    worker = nullptr;
    if (dib) {
        DeleteObject(dib);
        dib = nullptr;
    }
}

void PagePreview::clear() {
    if (dib) {
        DeleteObject(dib);
        dib = nullptr;
    }
    imgW = 0;
    imgH = 0;
    scrollX = 0;
    scrollY = 0;
    pageIndex = -1;
    ++shownGeneration;
}

void PagePreview::submit(std::vector<uint8_t> bits, int width, int height, int stride, uint64_t generation) {
    if (!worker) return;
    shownGeneration = generation;
    Job job;
    job.bits = std::move(bits);
    job.width = width;
    job.height = height;
    job.stride = stride;
    job.generation = generation;
    worker->submit(std::move(job));
}

void PagePreview::complete() {
    if (!worker) return;
    HBITMAP bmp = nullptr;
    int w = 0;
    int h = 0;
    uint64_t generation = 0;
    if (!worker->take(&bmp, &w, &h, &generation)) return;
    if (generation != shownGeneration) {
        if (bmp) DeleteObject(bmp);
        return;
    }
    if (dib) DeleteObject(dib);
    dib = bmp;
    imgW = w;
    imgH = h;
}

void PagePreview::notePage(int page) {
    if (page == pageIndex) return;
    pageIndex = page;
    scrollX = 0;
    scrollY = 0;
}

double PagePreview::clampScale(double scale) const {
    double longSide = static_cast<double>((std::max)(imgW, imgH));
    if (longSide <= 0) return 1;
    double lo = kMinLongSide / longSide;
    double hi = kMaxLongSide / longSide;
    return (std::min)((std::max)(scale, lo), hi);
}

double PagePreview::resolvedScale(int viewW, int viewH, int dpi) const {
    if (imgW <= 0 || imgH <= 0) return 0;
    double dipsW = viewW * 96.0 / dpi;
    double dipsH = viewH * 96.0 / dpi;
    switch (kind) {
    case ScaleKind::FitWidth:
        return dipsW > 1 ? dipsW / imgW : 0;
    case ScaleKind::FitHeight:
        return dipsH > 1 ? dipsH / imgH : 0;
    case ScaleKind::Fixed:
        return clampScale(fixedScale);
    }
    return 0;
}

bool PagePreview::canZoom(double factor, int viewW, int viewH, int dpi) const {
    double current = resolvedScale(viewW, viewH, dpi);
    if (current <= 0) return false;
    double next = clampScale(current * factor);
    if (factor > 1) return next > current * 1.001;
    return next < current / 1.001;
}

void PagePreview::zoom(double factor, int viewW, int viewH, int dpi) {
    double current = resolvedScale(viewW, viewH, dpi);
    if (current <= 0) return;
    fixedScale = clampScale(current * factor);
    kind = ScaleKind::Fixed;
}

bool PagePreview::updateBars(int viewW, int viewH, int dpi, int *destW, int *destH) {
    double scale = resolvedScale(viewW, viewH, dpi);
    if (!dib || scale <= 0) {
        *destW = 0;
        *destH = 0;
    } else {
        *destW = (std::max)(1, static_cast<int>(std::llround(imgW * scale * dpi / 96.0)));
        *destH = (std::max)(1, static_cast<int>(std::llround(imgH * scale * dpi / 96.0)));
    }
    int maxX = (std::max)(0, *destW - viewW);
    int maxY = (std::max)(0, *destH - viewH);
    scrollX = (std::min)((std::max)(scrollX, 0), maxX);
    scrollY = (std::min)((std::max)(scrollY, 0), maxY);
    bool hadH = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_HSCROLL) != 0;
    bool hadV = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VSCROLL) != 0;
    auto bar = [&](int which, int pos, int page, int maxPos) {
        SCROLLINFO info{};
        info.cbSize = sizeof info;
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = maxPos > 0 ? maxPos + page - 1 : 0;
        info.nPage = maxPos > 0 ? static_cast<UINT>(page) : 0;
        info.nPos = maxPos > 0 ? pos : 0;
        SetScrollInfo(hwnd, which, &info, TRUE);
    };
    bar(SB_HORZ, scrollX, viewW, maxX);
    bar(SB_VERT, scrollY, viewH, maxY);
    bool hasH = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_HSCROLL) != 0;
    bool hasV = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VSCROLL) != 0;
    return hadH != hasH || hadV != hasV;
}

void PagePreview::paint(HDC hdc, const RECT &, int dpi) {
    RECT client{};
    GetClientRect(hwnd, &client);
    int destW = 0;
    int destH = 0;
    if (dpi > 0 && updateBars(client.right, client.bottom, dpi, &destW, &destH)) {
        GetClientRect(hwnd, &client);
        updateBars(client.right, client.bottom, dpi, &destW, &destH);
    }
    int viewW = client.right;
    int viewH = client.bottom;
    if (viewW <= 0 || viewH <= 0) return;
    HDC back = CreateCompatibleDC(hdc);
    HBITMAP frame = CreateCompatibleBitmap(hdc, viewW, viewH);
    HGDIOBJ oldFrame = SelectObject(back, frame);
    HBRUSH surround = CreateSolidBrush(RGB(224, 224, 224));
    FillRect(back, &client, surround);
    DeleteObject(surround);
    if (dib && destW > 0 && destH > 0) {
        int x = destW < viewW ? (viewW - destW) / 2 : -scrollX;
        int y = destH < viewH ? (viewH - destH) / 2 : -scrollY;
        double device = static_cast<double>(destW) / imgW;
        SetStretchBltMode(back, device >= 1 ? COLORONCOLOR : HALFTONE);
        if (device < 1) SetBrushOrgEx(back, 0, 0, nullptr);
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, dib);
        StretchBlt(back, x, y, destW, destH, mem, 0, 0, imgW, imgH, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    BitBlt(hdc, 0, 0, viewW, viewH, back, 0, 0, SRCCOPY);
    SelectObject(back, oldFrame);
    DeleteObject(frame);
    DeleteDC(back);
}

void PagePreview::scroll(int bar, WPARAM how, int clientSpan, int contentSpan) {
    int *pos = bar == SB_HORZ ? &scrollX : &scrollY;
    int maxPos = (std::max)(0, contentSpan - clientSpan);
    int next = *pos;
    switch (LOWORD(how)) {
    case SB_LINELEFT:
        next -= 48;
        break;
    case SB_LINERIGHT:
        next += 48;
        break;
    case SB_PAGELEFT:
        next -= clientSpan;
        break;
    case SB_PAGERIGHT:
        next += clientSpan;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO info{};
        info.cbSize = sizeof info;
        info.fMask = SIF_TRACKPOS;
        GetScrollInfo(hwnd, bar, &info);
        next = info.nTrackPos;
        break;
    }
    default:
        return;
    }
    next = (std::min)((std::max)(next, 0), maxPos);
    if (next == *pos) return;
    *pos = next;
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *state = reinterpret_cast<PreviewBinding *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *created = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        return 0;
    }
    case WM_PREVIEW:
        if (state && state->preview) {
            state->preview->complete();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (HWND parent = GetParent(hwnd)) SendMessageW(parent, WM_PREVIEW, 0, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (state && state->preview) {
            state->preview->paint(hdc, client, static_cast<int>(GetDpiForWindow(hwnd)));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_HSCROLL:
    case WM_VSCROLL:
        if (state && state->preview && state->preview->hasImage()) {
            RECT client{};
            GetClientRect(hwnd, &client);
            int dpi = static_cast<int>(GetDpiForWindow(hwnd));
            int viewW = client.right;
            int viewH = client.bottom;
            double scale = state->preview->resolvedScale(viewW, viewH, dpi);
            int destW = static_cast<int>(std::llround(state->preview->width() * scale * dpi / 96.0));
            int destH = static_cast<int>(std::llround(state->preview->height() * scale * dpi / 96.0));
            if (msg == WM_HSCROLL) state->preview->scroll(SB_HORZ, wParam, viewW, destW);
            else state->preview->scroll(SB_VERT, wParam, viewH, destH);
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerPreviewClass(HINSTANCE inst) {
    WNDCLASSW cls{};
    cls.lpfnWndProc = previewProc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"gs2print.view";
    RegisterClassW(&cls);
}
