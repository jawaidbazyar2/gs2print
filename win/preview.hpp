#pragma once

#include "ui.hpp"

#include <cstdint>
#include <vector>

enum class ScaleKind { FitWidth, FitHeight, Fixed };

struct PagePreview {
    ScaleKind kind = ScaleKind::FitHeight;
    double fixedScale = 1;
    int scrollX = 0;
    int scrollY = 0;
    int pageIndex = -1;

    void attach(HWND hwnd);
    void shutdown();
    void clear();
    bool hasImage() const { return dib != nullptr; }
    int width() const { return imgW; }
    int height() const { return imgH; }

    void submit(std::vector<uint8_t> bits, int width, int height, int stride, uint64_t generation);
    void complete();
    void notePage(int page);

    double resolvedScale(int viewW, int viewH, int dpi) const;
    bool canZoom(double factor, int viewW, int viewH, int dpi) const;
    void zoom(double factor, int viewW, int viewH, int dpi);
    void fitWidth() { kind = ScaleKind::FitWidth; }
    void fitHeight() { kind = ScaleKind::FitHeight; }

    void paint(HDC hdc, const RECT &client, int dpi);
    void scroll(int bar, WPARAM how, int clientSpan, int contentSpan);

private:
    HWND hwnd = nullptr;
    HBITMAP dib = nullptr;
    int imgW = 0;
    int imgH = 0;
    uint64_t shownGeneration = 0;

    struct Worker;
    Worker *worker = nullptr;

    double clampScale(double scale) const;
    bool updateBars(int viewW, int viewH, int dpi, int *destW, int *destH);
};

struct PreviewBinding {
    PagePreview *preview = nullptr;
};

LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void registerPreviewClass(HINSTANCE inst);

constexpr double kPreviewZoom = 1.25;
