#include <winsock2.h>
#include "window.hpp"

#include "app.hpp"
#include "preview.hpp"
#include "settings.hpp"
#include "text.hpp"
#include "ui.hpp"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>

namespace {

HINSTANCE g_inst = nullptr;
int g_cascade = 0;

HFONT makeFont(HWND hwnd, bool underline) {
    int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    return CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, underline ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                        L"Segoe UI");
}

void setFont(HWND parent, HFONT font, HFONT link) {
    int ids[] = {IDC_STATUS, IDC_PREV,     IDC_PAGE, IDC_NEXT,  IDC_ZOOM_OUT, IDC_ZOOM_IN, IDC_FIT_W, IDC_FIT_H,
                 IDC_WROTE,  IDC_ERROR,    IDC_LISTEN, IDC_PORT_LABEL, IDC_PORT, IDC_LAN, IDC_CLOSE, IDC_ADDRESS,
                 IDC_BIND};
    for (int id : ids) {
        if (HWND child = GetDlgItem(parent, id)) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (HWND pdf = GetDlgItem(parent, IDC_PDF)) SendMessageW(pdf, WM_SETFONT, reinterpret_cast<WPARAM>(link), TRUE);
}

void applyFonts(HWND hwnd) {
    HFONT old = static_cast<HFONT>(GetPropW(hwnd, L"font"));
    HFONT oldLink = static_cast<HFONT>(GetPropW(hwnd, L"linkfont"));
    HFONT font = makeFont(hwnd, false);
    HFONT link = makeFont(hwnd, true);
    SetPropW(hwnd, L"font", font);
    SetPropW(hwnd, L"linkfont", link);
    setFont(hwnd, font, link);
    if (old) DeleteObject(old);
    if (oldLink) DeleteObject(oldLink);
}

std::wstring controlText(HWND hwnd, int id) {
    HWND child = GetDlgItem(hwnd, id);
    int n = GetWindowTextLengthW(child);
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(child, text.data(), n + 1);
    return text;
}

bool g_moved = false;

void place(HWND hwnd, int id, int x, int y, int w, int h, bool show) {
    HWND child = GetDlgItem(hwnd, id);
    if (!child) return;
    bool visible = IsWindowVisible(child) != FALSE;
    if (!show) {
        if (visible) {
            ShowWindow(child, SW_HIDE);
            g_moved = true;
        }
        return;
    }
    RECT now{};
    GetWindowRect(child, &now);
    MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT *>(&now), 2);
    if (now.left != x || now.top != y || now.right - now.left != w || now.bottom - now.top != h) {
        MoveWindow(child, x, y, w, h, FALSE);
        g_moved = true;
    }
    if (!visible) {
        ShowWindow(child, SW_SHOW);
        g_moved = true;
    }
}

void layoutWindow(HWND hwnd, Session *session) {
    g_moved = false;
    RECT client{};
    GetClientRect(hwnd, &client);
    int pad = dip(hwnd, 12);
    int width = client.right - pad * 2;
    bool image = session && session->preview.hasImage();
    bool pdf = session && !session->pdfPath.empty();
    bool error = session && !session->error.empty();
    std::wstring address = app().server.address;
    std::wstring bind = app().server.bindError;
    int addressLines = 1;
    for (wchar_t ch : address) {
        if (ch == L'\n') ++addressLines;
    }
    int bottom = client.bottom - pad;
    if (!bind.empty()) {
        int h = dip(hwnd, 20);
        bottom -= h;
        place(hwnd, IDC_BIND, pad, bottom, width, h, true);
    } else {
        place(hwnd, IDC_BIND, 0, 0, 0, 0, false);
    }
    if (!address.empty()) {
        int h = dip(hwnd, 16) * addressLines;
        bottom -= h;
        place(hwnd, IDC_ADDRESS, pad, bottom, width, h, true);
        bottom -= dip(hwnd, 4);
    } else {
        place(hwnd, IDC_ADDRESS, 0, 0, 0, 0, false);
    }
    int row = dip(hwnd, 28);
    bottom -= row;
    int listenW = dip(hwnd, 70);
    int labelW = dip(hwnd, 36);
    int editW = dip(hwnd, 72);
    int lanW = dip(hwnd, 140);
    int closeW = dip(hwnd, 90);
    int x = pad;
    place(hwnd, IDC_LISTEN, x, bottom, listenW, row, true);
    x += listenW;
    place(hwnd, IDC_PORT_LABEL, x, bottom, labelW, row, true);
    x += labelW + dip(hwnd, 6);
    place(hwnd, IDC_PORT, x, bottom + dip(hwnd, 2), editW, dip(hwnd, 24), true);
    x += editW + dip(hwnd, 12);
    place(hwnd, IDC_LAN, x, bottom, lanW, row, true);
    place(hwnd, IDC_CLOSE, client.right - pad - closeW, bottom, closeW, row, true);
    bottom -= dip(hwnd, 10);
    place(hwnd, IDC_SEP, pad, bottom, width, dip(hwnd, 2), true);
    bottom -= dip(hwnd, 8);
    if (error) {
        int h = dip(hwnd, 18);
        bottom -= h;
        place(hwnd, IDC_ERROR, pad, bottom, width, h, true);
        bottom -= dip(hwnd, 4);
    } else {
        place(hwnd, IDC_ERROR, 0, 0, 0, 0, false);
    }
    if (pdf) {
        int h = dip(hwnd, 18);
        bottom -= h;
        place(hwnd, IDC_WROTE, pad, bottom, dip(hwnd, 40), h, true);
        place(hwnd, IDC_PDF, pad + dip(hwnd, 44), bottom, width - dip(hwnd, 44), h, true);
        bottom -= dip(hwnd, 4);
    } else {
        place(hwnd, IDC_WROTE, 0, 0, 0, 0, false);
        place(hwnd, IDC_PDF, 0, 0, 0, 0, false);
    }
    if (image) {
        int h = dip(hwnd, 28);
        bottom -= h;
        int bx = pad;
        place(hwnd, IDC_PREV, bx, bottom, dip(hwnd, 80), h, true);
        bx += dip(hwnd, 84);
        place(hwnd, IDC_PAGE, bx, bottom, dip(hwnd, 140), h, true);
        bx += dip(hwnd, 144);
        place(hwnd, IDC_NEXT, bx, bottom, dip(hwnd, 70), h, true);
        bx += dip(hwnd, 82);
        place(hwnd, IDC_ZOOM_OUT, bx, bottom, dip(hwnd, 28), h, true);
        bx += dip(hwnd, 32);
        place(hwnd, IDC_ZOOM_IN, bx, bottom, dip(hwnd, 28), h, true);
        bx += dip(hwnd, 36);
        place(hwnd, IDC_FIT_W, bx, bottom, dip(hwnd, 90), h, true);
        bx += dip(hwnd, 94);
        place(hwnd, IDC_FIT_H, bx, bottom, dip(hwnd, 90), h, true);
        bottom -= dip(hwnd, 8);
    } else {
        place(hwnd, IDC_PREV, 0, 0, 0, 0, false);
        place(hwnd, IDC_PAGE, 0, 0, 0, 0, false);
        place(hwnd, IDC_NEXT, 0, 0, 0, 0, false);
        place(hwnd, IDC_ZOOM_OUT, 0, 0, 0, 0, false);
        place(hwnd, IDC_ZOOM_IN, 0, 0, 0, 0, false);
        place(hwnd, IDC_FIT_W, 0, 0, 0, 0, false);
        place(hwnd, IDC_FIT_H, 0, 0, 0, 0, false);
    }
    int top = pad;
    if (image) {
        place(hwnd, IDC_STATUS, pad, top, width - dip(hwnd, 28), dip(hwnd, 22), true);
        place(hwnd, IDC_PROGRESS, client.right - pad - dip(hwnd, 22), top, dip(hwnd, 22), dip(hwnd, 22),
              session && session->printing);
        top += dip(hwnd, 28);
    }
    place(hwnd, IDC_VIEW, pad, top, width, (std::max)(0, bottom - top), image);
    if (!image && session) {
        HFONT font = static_cast<HFONT>(GetPropW(hwnd, L"font"));
        HDC dc = GetDC(hwnd);
        HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
        SIZE size{};
        GetTextExtentPoint32W(dc, session->message.c_str(), static_cast<int>(session->message.size()), &size);
        SelectObject(dc, previous);
        ReleaseDC(hwnd, dc);
        int statusW = size.cx + dip(hwnd, 8);
        int rowY = top + ((std::max)(0, bottom - top) - dip(hwnd, 22)) / 2;
        int statusX = pad + (width - statusW) / 2;
        if (session->printing) statusX -= dip(hwnd, 14);
        place(hwnd, IDC_STATUS, statusX, rowY, statusW, dip(hwnd, 22), true);
        place(hwnd, IDC_PROGRESS, statusX + statusW, rowY, dip(hwnd, 22), dip(hwnd, 22), session->printing);
    } else if (!image) {
        place(hwnd, IDC_STATUS, pad, top, width, dip(hwnd, 22), true);
        place(hwnd, IDC_PROGRESS, 0, 0, 0, 0, false);
    }
    if (g_moved) RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void enableButton(HWND hwnd, int id, bool on) { EnableWindow(GetDlgItem(hwnd, id), on ? TRUE : FALSE); }

LRESULT CALLBACK portProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_PORT, 1), reinterpret_cast<LPARAM>(hwnd));
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

HWND child(HWND parent, const wchar_t *klass, const wchar_t *text, DWORD style, int id, HINSTANCE inst) {
    return CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
}

void chooseFile(HWND hwnd) {
    IFileOpenDialog *dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return;
    COMDLG_FILTERSPEC types[] = {
        {L"GS2 printer ticket", L"*.gs2print"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(2, types);
    dialog->SetTitle(L"Open printer dump");
    if (SUCCEEDED(dialog->Show(hwnd))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                app().focused = reinterpret_cast<Session *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                app().handleFile(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

LRESULT CALLBACK jobProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *session = reinterpret_cast<Session *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto *created = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        session = static_cast<Session *>(created->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(session));
        HINSTANCE inst = created->hInstance;
        child(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, IDC_STATUS, inst);
        child(hwnd, PROGRESS_CLASSW, L"", PBS_MARQUEE, IDC_PROGRESS, inst);
        auto *binding = new PreviewBinding;
        binding->preview = &session->preview;
        CreateWindowExW(0, L"gs2print.view", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VIEW)), inst, binding);
        child(hwnd, L"BUTTON", L"Previous", BS_PUSHBUTTON, IDC_PREV, inst);
        child(hwnd, L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, IDC_PAGE, inst);
        child(hwnd, L"BUTTON", L"Next", BS_PUSHBUTTON, IDC_NEXT, inst);
        child(hwnd, L"BUTTON", L"\u2212", BS_PUSHBUTTON, IDC_ZOOM_OUT, inst);
        child(hwnd, L"BUTTON", L"+", BS_PUSHBUTTON, IDC_ZOOM_IN, inst);
        child(hwnd, L"BUTTON", L"Fit Width", BS_PUSHBUTTON, IDC_FIT_W, inst);
        child(hwnd, L"BUTTON", L"Fit Height", BS_PUSHBUTTON, IDC_FIT_H, inst);
        child(hwnd, L"STATIC", L"Wrote", SS_LEFT | SS_CENTERIMAGE, IDC_WROTE, inst);
        child(hwnd, L"STATIC", L"", SS_LEFT | SS_NOTIFY | SS_NOPREFIX | SS_CENTERIMAGE, IDC_PDF, inst);
        child(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_ERROR, inst);
        child(hwnd, L"STATIC", L"", SS_ETCHEDHORZ, IDC_SEP, inst);
        child(hwnd, L"BUTTON", L"Listen", BS_AUTOCHECKBOX, IDC_LISTEN, inst);
        child(hwnd, L"STATIC", L"Port", SS_RIGHT | SS_CENTERIMAGE, IDC_PORT_LABEL, inst);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PORT)), inst, nullptr);
        SetWindowSubclass(edit, portProc, 1, 0);
        child(hwnd, L"BUTTON", L"Other computers", BS_AUTOCHECKBOX, IDC_LAN, inst);
        child(hwnd, L"BUTTON", L"Close Job", BS_PUSHBUTTON, IDC_CLOSE, inst);
        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ADDRESS)), inst, nullptr);
        child(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_BIND, inst);
        applyFonts(hwnd);
        session->attach(hwnd);
        syncWindow(session);
        return 0;
    }
    case WM_SIZE:
        if (session) layoutWindow(hwnd, session);
        return 0;
    case WM_DPICHANGED: {
        auto *rect = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyFonts(hwnd);
        if (session) layoutWindow(hwnd, session);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd);
        RECT frame{0, 0, MulDiv(720, static_cast<int>(dpi), 96), MulDiv(640, static_cast<int>(dpi), 96)};
        DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&frame, style, TRUE, ex, dpi);
        auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
        info->ptMinTrackSize.x = frame.right - frame.left;
        info->ptMinTrackSize.y = frame.bottom - frame.top;
        return 0;
    }
    case WM_TIMER:
        if (wParam == kPreviewTimer && session) {
            KillTimer(hwnd, kPreviewTimer);
            session->onTimer();
        }
        return 0;
    case WM_PREVIEW:
        if (session) syncWindow(session);
        return 0;
    case WM_TICKET:
        if (session) session->drainTicket();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && session) app().focused = session;
        break;
    case WM_COMMAND:
        if (!session) break;
        switch (LOWORD(wParam)) {
        case IDM_OPEN:
            chooseFile(hwnd);
            return 0;
        case IDM_EXIT:
            app().quit();
            return 0;
        case IDM_SETTINGS:
            showSettings(hwnd);
            return 0;
        case IDC_PREV:
            session->step(-1);
            return 0;
        case IDC_NEXT:
            session->step(1);
            return 0;
        case IDC_ZOOM_OUT:
            session->zoom(1.0 / kPreviewZoom);
            return 0;
        case IDC_ZOOM_IN:
            session->zoom(kPreviewZoom);
            return 0;
        case IDC_FIT_W:
            session->fitWidth();
            return 0;
        case IDC_FIT_H:
            session->fitHeight();
            return 0;
        case IDC_PDF:
            if (HIWORD(wParam) == STN_CLICKED) session->openSavedPdf();
            return 0;
        case IDC_CLOSE:
            session->closeActiveJob();
            syncWindow(session);
            return 0;
        case IDC_LISTEN: {
            bool want = SendMessageW(GetDlgItem(hwnd, IDC_LISTEN), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (want && !app().server.commitPort(controlText(hwnd, IDC_PORT), false)) {
                syncWindow(session);
                app().refreshAll();
                return 0;
            }
            app().server.setListening(want);
            app().refreshAll();
            return 0;
        }
        case IDC_LAN: {
            bool want = SendMessageW(GetDlgItem(hwnd, IDC_LAN), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!app().server.commitPort(controlText(hwnd, IDC_PORT), false)) {
                syncWindow(session);
                return 0;
            }
            app().server.setAllowLAN(want);
            app().refreshAll();
            return 0;
        }
        case IDC_PORT:
            if (HIWORD(wParam) == 1) {
                app().server.commitPort(controlText(hwnd, IDC_PORT), true);
                app().refreshAll();
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        COLORREF text = GetSysColor(COLOR_WINDOWTEXT);
        if (ctrl == GetDlgItem(hwnd, IDC_ERROR) || ctrl == GetDlgItem(hwnd, IDC_BIND)) text = RGB(180, 0, 0);
        else if (ctrl == GetDlgItem(hwnd, IDC_PDF)) text = RGB(0, 102, 204);
        else if (!IsWindowEnabled(ctrl)) text = GetSysColor(COLOR_GRAYTEXT);
        SetTextColor(dc, text);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wParam) == GetDlgItem(hwnd, IDC_PDF)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_DESTROY: {
        HFONT font = static_cast<HFONT>(GetPropW(hwnd, L"font"));
        HFONT link = static_cast<HFONT>(GetPropW(hwnd, L"linkfont"));
        if (font) DeleteObject(font);
        if (link) DeleteObject(link);
        RemovePropW(hwnd, L"font");
        RemovePropW(hwnd, L"linkfont");
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (session) {
            session->shutdownConnection();
            session->preview.shutdown();
            app().sessionClosed(session);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void registerJobClass(HINSTANCE inst) {
    g_inst = inst;
    WNDCLASSW cls{};
    cls.lpfnWndProc = jobProc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = L"gs2print.job";
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&cls);
}

HINSTANCE jobInstance() { return g_inst; }

HWND createJobWindow(Session *session, HINSTANCE inst, int show) {
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (g_cascade > 0) {
        int step = (g_cascade % 8) * 28;
        x = 60 + step;
        y = 60 + step;
    }
    ++g_cascade;
    HWND hwnd = CreateWindowExW(0, L"gs2print.job", L"gs2print", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, 1040,
                                 860, nullptr, nullptr, inst, session);
    if (!hwnd) return nullptr;
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"Open…\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings…");
    SetMenu(hwnd, menu);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    return hwnd;
}

void syncWindow(Session *session) {
    if (!session || !session->hwnd) return;
    HWND hwnd = session->hwnd;
    SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS), session->message.c_str());
    HWND progress = GetDlgItem(hwnd, IDC_PROGRESS);
    SendMessageW(progress, PBM_SETMARQUEE, session->printing ? TRUE : FALSE, 30);
    std::wstring page = L"Page " + std::to_wstring(session->pageIndex + 1) + L" of " + std::to_wstring(session->pageCount);
    SetWindowTextW(GetDlgItem(hwnd, IDC_PAGE), page.c_str());
    enableButton(hwnd, IDC_PREV, session->preview.hasImage() && session->pageIndex > 0);
    enableButton(hwnd, IDC_NEXT, session->preview.hasImage() && session->pageIndex + 1 < session->pageCount);
    enableButton(hwnd, IDC_ZOOM_OUT, session->canZoom(1.0 / kPreviewZoom));
    enableButton(hwnd, IDC_ZOOM_IN, session->canZoom(kPreviewZoom));
    enableButton(hwnd, IDC_CLOSE, session->canCloseJob());
    SetWindowTextW(GetDlgItem(hwnd, IDC_ERROR), session->error.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_PDF), fileName(session->pdfPath).c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_ADDRESS), app().server.address.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_BIND), app().server.bindError.c_str());
    SendMessageW(GetDlgItem(hwnd, IDC_LISTEN), BM_SETCHECK, app().server.listening ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, IDC_LAN), BM_SETCHECK, app().server.allowLAN ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND edit = GetDlgItem(hwnd, IDC_PORT);
    if (GetFocus() != edit) SetWindowTextW(edit, std::to_wstring(app().server.port).c_str());
    layoutWindow(hwnd, session);
    if (HWND view = GetDlgItem(hwnd, IDC_VIEW)) InvalidateRect(view, nullptr, FALSE);
}
