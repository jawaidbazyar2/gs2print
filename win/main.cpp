#include "app.hpp"
#include "associate.hpp"
#include "preview.hpp"
#include "ui.hpp"
#include "window.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

namespace {

HANDLE g_mutex = nullptr;

bool takeInstance(const std::vector<std::wstring> &args) {
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\net.gssquared.gs2print");
    if (!g_mutex) return true;
    DWORD wait = WaitForSingleObject(g_mutex, 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return true;

    HWND ipc = nullptr;
    for (int i = 0; i < 40 && !ipc; ++i) {
        ipc = FindWindowW(L"gs2print.ipc", nullptr);
        if (!ipc) Sleep(50);
    }
    if (!ipc) {
        MessageBoxW(nullptr, L"gs2print is already running, but it did not answer.", L"gs2print", MB_ICONWARNING | MB_OK);
        CloseHandle(g_mutex);
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(ipc, &pid);
    AllowSetForegroundWindow(pid);
    std::vector<wchar_t> buf;
    for (const auto &arg : args) {
        buf.insert(buf.end(), arg.begin(), arg.end());
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');
    COPYDATASTRUCT data{};
    data.dwData = 1;
    data.cbData = args.empty() ? 0 : static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    data.lpData = args.empty() ? nullptr : buf.data();
    SendMessageW(ipc, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    CloseHandle(g_mutex);
    return false;
}

LRESULT CALLBACK ipcProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COPYDATA: {
        auto *data = reinterpret_cast<COPYDATASTRUCT *>(lParam);
        if (!data || data->cbData < sizeof(wchar_t) || !data->lpData) {
            app().raisePrimary();
            return TRUE;
        }
        auto *text = static_cast<const wchar_t *>(data->lpData);
        size_t chars = data->cbData / sizeof(wchar_t);
        size_t index = 0;
        bool any = false;
        while (index < chars && text[index] != L'\0') {
            std::wstring path;
            while (index < chars && text[index] != L'\0') path.push_back(text[index++]);
            if (index < chars && text[index] == L'\0') ++index;
            if (!path.empty()) {
                any = true;
                app().handleFile(path);
            }
        }
        if (!any) app().raisePrimary();
        return TRUE;
    }
    case WM_SERVER:
        app().server.drain();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetCurrentProcessExplicitAppUserModelID(L"net.gssquared.gs2print");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    registerFileAssociation();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    if (!takeInstance(args)) {
        WSACleanup();
        CoUninitialize();
        return 0;
    }

    registerPreviewClass(inst);
    registerJobClass(inst);
    WNDCLASSW ipcClass{};
    ipcClass.lpfnWndProc = ipcProc;
    ipcClass.hInstance = inst;
    ipcClass.lpszClassName = L"gs2print.ipc";
    RegisterClassW(&ipcClass);

    int code = 0;
    HWND ipc = nullptr;
    {
        App application;
        setApp(&application);
        createJobWindow(&application.primary, inst, show);
        ipc = CreateWindowExW(0, L"gs2print.ipc", L"gs2print.ipc", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        application.server.start(ipc);
        application.server.restore();
        application.openPaths(args);

        ACCEL accel[] = {{static_cast<BYTE>(FCONTROL | FVIRTKEY), static_cast<WORD>('O'), IDM_OPEN}};
        HACCEL table = CreateAcceleratorTableW(accel, 1);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!TranslateAcceleratorW(message.hwnd, table, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        code = static_cast<int>(message.wParam);
        DestroyAcceleratorTable(table);
        setApp(nullptr);
    }
    if (ipc) DestroyWindow(ipc);
    if (g_mutex) CloseHandle(g_mutex);
    WSACleanup();
    CoUninitialize();
    return code;
}
