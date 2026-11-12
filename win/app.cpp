#include "app.hpp"

#include "text.hpp"
#include "window.hpp"

#include <algorithm>
#include <cwchar>
#include <cstring>

namespace {

std::wstring normalize(const std::wstring &path) {
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n == 0) return path;
    std::wstring full(n, L'\0');
    GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
    if (!full.empty() && full.back() == L'\0') full.pop_back();
    CharLowerBuffW(full.data(), static_cast<DWORD>(full.size()));
    return full;
}

enum class Kind { Live, Dump, Rejected };

struct Classified {
    Kind kind = Kind::Dump;
    std::string socket;
    std::wstring message;
};

Classified classify(const std::vector<uint8_t> &data) {
    Classified out;
    if (data.empty()) return out;
    char socket[4096];
    int code = gs2_ticket_field(data.data(), data.size(), "socket", socket, sizeof socket);
    if (code < 0) {
        out.kind = Kind::Rejected;
        out.message = L"Socket path is too long.";
        return out;
    }
    if (code == 0 || socket[0] == '\0') return out;
    char printer[256];
    int printerCode = gs2_ticket_field(data.data(), data.size(), "printer", printer, sizeof printer);
    if (printerCode != 1 || std::strcmp(printer, "imagewriter-ii") != 0) {
        out.kind = Kind::Rejected;
        out.message = L"Unsupported printer.";
        return out;
    }
    out.kind = Kind::Live;
    out.socket = socket;
    return out;
}

}  // namespace

namespace {
App *g_app = nullptr;
}

App &app() { return *g_app; }
void setApp(App *instance) { g_app = instance; }

App::App() {
    primary.isPrimary = true;
    server.onBytes = [this](std::vector<uint8_t> bytes) { onBytes(std::move(bytes)); };
    server.onIdle = [this] { onIdle(); };
    server.onClient = [this](bool connected) { onClient(connected); };
    server.onDisconnect = [this] { onDisconnect(); };
}

void App::openPaths(const std::vector<std::wstring> &paths) {
    for (const auto &path : paths) handleFile(path);
}

void App::handleFile(const std::wstring &path) {
    std::wstring key = normalize(path);
    unsigned long long now = GetTickCount64();
    seen.erase(std::remove_if(seen.begin(), seen.end(),
                               [&](const Seen &item) { return now - item.at >= 2000; }),
               seen.end());
    for (const auto &item : seen) {
        if (item.path == key && now - item.at < 500) return;
    }
    seen.push_back(Seen{key, now});

    std::wstring readError;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    std::vector<uint8_t> data;
    if (file == INVALID_HANDLE_VALUE) {
        readError = systemMessage(GetLastError());
    } else {
        uint8_t buf[64 * 1024];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(file, buf, sizeof buf, &n, nullptr)) {
                readError = systemMessage(GetLastError());
                break;
            }
            if (n == 0) break;
            data.insert(data.end(), buf, buf + n);
        }
        CloseHandle(file);
    }
    if (!readError.empty()) {
        Session *session = dumpTarget();
        if (session) {
            session->showError(readError);
            session->raise();
        }
        return;
    }

    Classified kind = classify(data);
    if (kind.kind == Kind::Live) {
        if (isLive(kind.socket)) return;
        if (Session *session = claimIdle()) session->openTicket(kind.socket);
        return;
    }
    if (kind.kind == Kind::Rejected) {
        if (Session *session = claimIdle()) {
            session->showError(kind.message);
            session->raise();
        }
        return;
    }
    if (Session *session = dumpTarget()) session->openDump(path);
}

void App::raisePrimary() {
    if (primary.hwnd) primary.raise();
    else if (!extras.empty() && extras.front()->hwnd) extras.front()->raise();
}

void App::sessionClosed(Session *session) {
    if (tcp == session) tcp = nullptr;
    if (focused == session) focused = nullptr;
    session->hwnd = nullptr;
    if (session != &primary) {
        extras.erase(std::remove_if(extras.begin(), extras.end(),
                                     [&](const std::unique_ptr<Session> &item) { return item.get() == session; }),
                     extras.end());
    }
    if (windowCount() == 0) PostQuitMessage(0);
}

void App::refreshAll() {
    if (primary.hwnd) syncWindow(&primary);
    for (auto &extra : extras) {
        if (extra->hwnd) syncWindow(extra.get());
    }
}

int App::windowCount() const {
    int count = primary.hwnd ? 1 : 0;
    for (const auto &extra : extras) {
        if (extra->hwnd) ++count;
    }
    return count;
}

void App::quit() {
    std::vector<HWND> windows;
    if (primary.hwnd) windows.push_back(primary.hwnd);
    for (const auto &extra : extras) {
        if (extra->hwnd) windows.push_back(extra->hwnd);
    }
    for (HWND window : windows) DestroyWindow(window);
}

void App::onBytes(std::vector<uint8_t> bytes) {
    if (!tcp) {
        tcp = claimIdle();
        if (tcp) tcp->ownsTCP = true;
    }
    if (tcp) tcp->receive(bytes.data(), bytes.size());
}

void App::onIdle() {
    if (tcp) tcp->finishNetworkJob();
}

void App::onClient(bool connected) {
    server.clientConnected = connected;
    if (connected && !tcp) {
        tcp = claimIdle();
        if (tcp) tcp->ownsTCP = true;
    }
    refreshAll();
}

void App::onDisconnect() {
    Session *session = tcp;
    tcp = nullptr;
    if (!session) return;
    session->ownsTCP = false;
    session->finishNetworkJob();
    refreshAll();
}

Session *App::claimIdle() {
    if (primary.hwnd && primary.isIdle()) return &primary;
    for (auto &extra : extras) {
        if (extra->hwnd && extra->isIdle()) return extra.get();
    }
    auto created = std::make_unique<Session>();
    Session *raw = created.get();
    extras.push_back(std::move(created));
    if (!createJobWindow(raw, jobInstance(), SW_SHOW)) {
        extras.pop_back();
        return nullptr;
    }
    return raw;
}

Session *App::dumpTarget() {
    if (focused && focused->hwnd) return focused;
    if (primary.hwnd) return &primary;
    return claimIdle();
}

bool App::isLive(const std::string &socket) const {
    if (primary.holdsSocket(socket)) return true;
    for (const auto &extra : extras) {
        if (extra->holdsSocket(socket)) return true;
    }
    return false;
}
