#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

inline int dip(HWND hwnd, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

constexpr UINT WM_TICKET = WM_APP + 1;
constexpr UINT WM_SERVER = WM_APP + 2;
constexpr UINT WM_PREVIEW = WM_APP + 3;

enum ControlId : int {
    IDC_STATUS = 100,
    IDC_PROGRESS,
    IDC_VIEW,
    IDC_PREV,
    IDC_PAGE,
    IDC_NEXT,
    IDC_ZOOM_OUT,
    IDC_ZOOM_IN,
    IDC_FIT_W,
    IDC_FIT_H,
    IDC_WROTE,
    IDC_PDF,
    IDC_ERROR,
    IDC_SEP,
    IDC_LISTEN,
    IDC_PORT_LABEL,
    IDC_PORT,
    IDC_LAN,
    IDC_CLOSE,
    IDC_ADDRESS,
    IDC_BIND,
    IDM_OPEN = 400,
    IDM_EXIT,
    IDM_SETTINGS,
    IDC_SETTING_LF = 500,
    IDC_SETTING_OK,
    IDC_SETTING_CANCEL
};

constexpr UINT_PTR kPreviewTimer = 1;
