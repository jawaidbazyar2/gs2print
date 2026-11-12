#include "settings.hpp"

#include "ui.hpp"

namespace {

constexpr wchar_t kKey[] = L"Software\\net.gssquared.gs2print";

DWORD readDword(const wchar_t *name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

void writeDword(const wchar_t *name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &value, sizeof value);
}

struct SettingsDialog {
    HWND owner = nullptr;
    HWND window = nullptr;
    HWND check = nullptr;
    HFONT font = nullptr;
};

SettingsDialog *dialogFrom(HWND hwnd) {
    return reinterpret_cast<SettingsDialog *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void layoutDialog(SettingsDialog *dlg) {
    RECT client{};
    GetClientRect(dlg->window, &client);
    int pad = dip(dlg->window, 16);
    int width = client.right - pad * 2;
    int y = pad;
    MoveWindow(dlg->check, pad, y, width, dip(dlg->window, 24), TRUE);
    y += dip(dlg->window, 40);
    int buttonW = dip(dlg->window, 80);
    int buttonH = dip(dlg->window, 26);
    HWND cancel = GetDlgItem(dlg->window, IDC_SETTING_CANCEL);
    HWND ok = GetDlgItem(dlg->window, IDC_SETTING_OK);
    MoveWindow(ok, client.right - pad - buttonW * 2 - dip(dlg->window, 8), y, buttonW, buttonH, TRUE);
    MoveWindow(cancel, client.right - pad - buttonW, y, buttonW, buttonH, TRUE);
}

LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialog *dlg = dialogFrom(hwnd);
    switch (msg) {
    case WM_CREATE: {
        auto *created = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        dlg = static_cast<SettingsDialog *>(created->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dlg));
        dlg->window = hwnd;
        dlg->font = CreateFontW(-MulDiv(9, GetDpiForWindow(hwnd), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        dlg->check = CreateWindowExW(0, L"BUTTON", L"SW1-8  Line feed after carriage return",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTING_LF)),
                                      created->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTING_OK)),
                        created->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTING_CANCEL)),
                        created->hInstance, nullptr);
        SendMessageW(dlg->check, WM_SETFONT, reinterpret_cast<WPARAM>(dlg->font), TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_SETTING_OK), WM_SETFONT, reinterpret_cast<WPARAM>(dlg->font), TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_SETTING_CANCEL), WM_SETFONT, reinterpret_cast<WPARAM>(dlg->font), TRUE);
        SendMessageW(dlg->check, BM_SETCHECK, autoLineFeed() ? BST_CHECKED : BST_UNCHECKED, 0);
        layoutDialog(dlg);
        return 0;
    }
    case WM_SIZE:
        if (dlg) layoutDialog(dlg);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SETTING_OK:
            setAutoLineFeed(SendMessageW(dlg->check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            DestroyWindow(hwnd);
            return 0;
        case IDC_SETTING_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (dlg && dlg->font) {
            DeleteObject(dlg->font);
            dlg->font = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool autoLineFeed() { return readDword(L"imagewriter.autoLineFeed", 0) != 0; }
void setAutoLineFeed(bool on) { writeDword(L"imagewriter.autoLineFeed", on ? 1 : 0); }

bool storedListen() { return readDword(L"serverListen", 0) != 0; }
void storeListen(bool on) { writeDword(L"serverListen", on ? 1 : 0); }

int storedPort() {
    DWORD value = readDword(L"serverPort", 9100);
    if (value < 1 || value > 65535) return 9100;
    return static_cast<int>(value);
}

void storePort(int port) { writeDword(L"serverPort", static_cast<DWORD>(port)); }

bool storedLan() { return readDword(L"serverLAN", 0) != 0; }
void storeLan(bool on) { writeDword(L"serverLAN", on ? 1 : 0); }

void showSettings(HWND owner) {
    static bool registered = false;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSW cls{};
        cls.lpfnWndProc = settingsProc;
        cls.hInstance = inst;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = L"gs2print.settings";
        RegisterClassW(&cls);
        registered = true;
    }
    SettingsDialog dlg;
    dlg.owner = owner;
    int dpi = static_cast<int>(GetDpiForWindow(owner));
    RECT frame{0, 0, MulDiv(440, dpi, 96), MulDiv(120, dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_DLGMODALFRAME, static_cast<UINT>(dpi));
    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    EnableWindow(owner, FALSE);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"gs2print.settings", L"Settings",
                                   style, x, y, width, height, owner, nullptr, inst, &dlg);
    ShowWindow(window, SW_SHOW);
    MSG msg;
    while (IsWindow(window) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}
