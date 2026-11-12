#include "ticket.hpp"

#include "text.hpp"
#include "ui.hpp"

#include <afunix.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>

TicketClient::TicketClient(std::string path, HWND hwnd) : path(std::move(path)), hwnd(hwnd) {}

TicketClient::~TicketClient() { stop(); }

void TicketClient::start() {
    if (started) return;
    started = true;
    reader = std::thread([this] { readerMain(); });
    idle = std::thread([this] { idleMain(); });
}

void TicketClient::stop() {
    stopRequested = true;
    idleDeadline = 0;
    HANDLE pipeCopy = INVALID_HANDLE_VALUE;
    SOCKET sockCopy = INVALID_SOCKET;
    {
        std::lock_guard lock(mu);
        pipeCopy = pipe;
        sockCopy = sock;
        sock = INVALID_SOCKET;
        pipe = INVALID_HANDLE_VALUE;
        queue.clear();
    }
    if (sockCopy != INVALID_SOCKET) {
        shutdown(sockCopy, SD_BOTH);
        closesocket(sockCopy);
    }
    if (pipeCopy != INVALID_HANDLE_VALUE) CancelIoEx(pipeCopy, nullptr);
    if (reader.joinable()) reader.join();
    if (idle.joinable()) idle.join();
    if (pipeCopy != INVALID_HANDLE_VALUE) CloseHandle(pipeCopy);
}

std::vector<TicketClient::Event> TicketClient::take() {
    std::lock_guard lock(mu);
    std::vector<Event> out;
    out.swap(queue);
    return out;
}

void TicketClient::push(Event event) {
    if (stopRequested) return;
    {
        std::lock_guard lock(mu);
        if (stopRequested) return;
        queue.push_back(std::move(event));
    }
    if (hwnd) PostMessageW(hwnd, WM_TICKET, 0, 0);
}

void TicketClient::publishSocket(SOCKET fd) {
    std::lock_guard lock(mu);
    if (stopRequested) {
        closesocket(fd);
        return;
    }
    sock = fd;
}

void TicketClient::publishPipe(HANDLE handle) {
    std::lock_guard lock(mu);
    if (stopRequested) {
        CloseHandle(handle);
        return;
    }
    pipe = handle;
}

void TicketClient::idleMain() {
    while (!stopRequested) {
        Sleep(50);
        unsigned long long due = idleDeadline.load();
        if (due != 0 && GetTickCount64() >= due && !stopRequested) {
            idleDeadline = 0;
            push(Event{Kind::Idle, {}, {}});
        }
    }
}

void TicketClient::readerMain() {
    bool pipePath = path.rfind("\\\\.\\pipe\\", 0) == 0 || path.rfind("\\\\?\\pipe\\", 0) == 0;
    if (pipePath) {
        HANDLE handle = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            Event failed;
            failed.kind = Kind::Failed;
            failed.text = L"Could not connect to " + widen(path) + L": " + systemMessage(GetLastError());
            push(std::move(failed));
            return;
        }
        publishPipe(handle);
        if (stopRequested) return;
        push(Event{Kind::Ready, {}, {}});
        idleDeadline = GetTickCount64() + 10000;
        uint8_t buf[16384];
        for (;;) {
            if (stopRequested) break;
            DWORD n = 0;
            BOOL ok = ReadFile(handle, buf, sizeof buf, &n, nullptr);
            if (stopRequested) break;
            if (!ok || n == 0) break;
            Event bytes;
            bytes.kind = Kind::Bytes;
            bytes.bytes.assign(buf, buf + n);
            push(std::move(bytes));
            idleDeadline = GetTickCount64() + 10000;
        }
        if (!stopRequested) push(Event{Kind::Closed, {}, {}});
        return;
    }

    if (path.size() + 1 > sizeof(sockaddr_un::sun_path)) {
        push(Event{Kind::Failed, {}, L"Socket path is too long."});
        return;
    }
    SOCKET fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        Event failed;
        failed.kind = Kind::Failed;
        failed.text = L"Could not connect to " + widen(path) + L": " + systemMessage(WSAGetLastError());
        push(std::move(failed));
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int addrLen = static_cast<int>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    int rc = connect(fd, reinterpret_cast<sockaddr *>(&addr), addrLen);
    if (rc != 0) {
        int err = WSAGetLastError();
        closesocket(fd);
        Event failed;
        failed.kind = Kind::Failed;
        failed.text = L"Could not connect to " + widen(path) + L": " + systemMessage(static_cast<unsigned long>(err));
        push(std::move(failed));
        return;
    }
    publishSocket(fd);
    if (stopRequested) return;
    push(Event{Kind::Ready, {}, {}});
    idleDeadline = GetTickCount64() + 10000;
    uint8_t buf[16384];
    for (;;) {
        if (stopRequested) break;
        int n = recv(fd, reinterpret_cast<char *>(buf), sizeof buf, 0);
        if (stopRequested) break;
        if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
        if (n <= 0) break;
        Event bytes;
        bytes.kind = Kind::Bytes;
        bytes.bytes.assign(buf, buf + n);
        push(std::move(bytes));
        idleDeadline = GetTickCount64() + 10000;
    }
    if (!stopRequested) push(Event{Kind::Closed, {}, {}});
}
