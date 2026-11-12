#include "server.hpp"

#include "settings.hpp"
#include "text.hpp"
#include "ui.hpp"

#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cwchar>

namespace {

std::wstring trim(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

std::wstring portInUse(int port) { return L"Port " + std::to_wstring(port) + L" is already in use."; }
std::wstring portFailed(int port) { return L"Could not listen on port " + std::to_wstring(port) + L"."; }

std::wstring addressText(int port, bool lan) {
    if (!lan) return L"127.0.0.1:" + std::to_wstring(port);
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
    }
    std::vector<std::wstring> ips;
    if (rc == NO_ERROR) {
        for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto *uni = adapter->FirstUnicastAddress; uni; uni = uni->Next) {
                if (!uni->Address.lpSockaddr || uni->Address.lpSockaddr->sa_family != AF_INET) continue;
                wchar_t host[64] = {};
                if (GetNameInfoW(uni->Address.lpSockaddr, uni->Address.iSockaddrLength, host, 64, nullptr, 0,
                                 NI_NUMERICHOST) != 0) {
                    continue;
                }
                if (host[0] == L'\0') continue;
                std::wstring ip = host;
                if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
            }
        }
    }
    std::sort(ips.begin(), ips.end());
    if (ips.empty()) return L"0.0.0.0:" + std::to_wstring(port);
    std::wstring joined;
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i) joined += L"\n";
        joined += ips[i];
        joined += L":";
        joined += std::to_wstring(port);
    }
    return joined;
}

SOCKET listenOn(int family, int port, bool lan, int *err) {
    SOCKET fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        *err = WSAGetLastError();
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
    BOOL yes = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&yes), sizeof yes);
    int rc = 0;
    if (family == AF_INET6) {
        BOOL only = TRUE;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char *>(&only), sizeof only);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(static_cast<u_short>(port));
        addr.sin6_addr = lan ? in6addr_any : in6addr_loopback;
        rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        if (lan) addr.sin_addr.s_addr = htonl(INADDR_ANY);
        else inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    }
    if (rc != 0) {
        *err = WSAGetLastError();
        closesocket(fd);
        return INVALID_SOCKET;
    }
    if (listen(fd, 8) != 0) {
        *err = WSAGetLastError();
        closesocket(fd);
        return INVALID_SOCKET;
    }
    *err = 0;
    return fd;
}

struct Loop {
    SOCKET wake = INVALID_SOCKET;
    SOCKET v4 = INVALID_SOCKET;
    SOCKET v6 = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    unsigned long long idleAt = 0;
    uint8_t buf[16384] = {};
};

void closeSocket(SOCKET *fd) {
    if (*fd != INVALID_SOCKET) {
        closesocket(*fd);
        *fd = INVALID_SOCKET;
    }
}

}  // namespace

PrintServer::PrintServer() {
    port = storedPort();
    listening = storedListen();
    allowLAN = storedLan();
}

PrintServer::~PrintServer() {
    if (thread.joinable()) {
        request(Cmd::Quit);
        thread.join();
    }
    if (wakeSend != INVALID_SOCKET) closesocket(wakeSend);
}

void PrintServer::start(HWND window) {
    ui = window;
    thread = std::thread([this] { threadMain(); });
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return threadStarted; });
}

void PrintServer::restore() { applyNow(); }

bool PrintServer::commitPort(const std::wstring &text, bool rebindIfUnchanged) {
    wchar_t *end = nullptr;
    std::wstring trimmed = trim(text);
    long value = wcstol(trimmed.c_str(), &end, 10);
    if (trimmed.empty() || !end || *end != L'\0' || value < 1 || value > 65535) {
        bindError = L"Port must be between 1 and 65535.";
        return false;
    }
    if (static_cast<int>(value) != port) {
        port = static_cast<int>(value);
        storePort(port);
        applyNow();
    } else if (rebindIfUnchanged && listening) {
        applyNow();
    }
    return true;
}

void PrintServer::setListening(bool on) {
    if (listening == on) return;
    listening = on;
    storeListen(on);
    applyNow();
}

void PrintServer::setAllowLAN(bool on) {
    if (allowLAN == on) return;
    allowLAN = on;
    storeLan(on);
    applyNow();
}

bool PrintServer::closeClient() {
    if (!clientConnected && !threadStarted) return false;
    bool dropped = request(Cmd::CloseClient);
    if (dropped) {
        clientConnected = false;
        if (onDisconnect) onDisconnect();
    }
    return dropped;
}

void PrintServer::drain() {
    std::vector<Ev> events;
    {
        std::lock_guard lock(qmu);
        events.swap(queue);
    }
    for (auto &event : events) {
        switch (event.kind) {
        case EvKind::Bytes:
            if (onBytes) onBytes(std::move(event.bytes));
            break;
        case EvKind::Idle:
            if (onIdle) onIdle();
            break;
        case EvKind::Client:
            clientConnected = event.connected;
            if (onClient) onClient(event.connected);
            break;
        case EvKind::Closed:
            clientConnected = false;
            if (onDisconnect) onDisconnect();
            break;
        }
    }
}

void PrintServer::applyNow() {
    bool dropped = request(Cmd::Apply);
    if (dropped) {
        clientConnected = false;
        if (onDisconnect) onDisconnect();
    }
}

bool PrintServer::request(Cmd which) {
    {
        std::lock_guard lock(mu);
        cmd = which;
        cmdListen = listening;
        cmdPort = port;
        cmdLan = allowLAN;
        cmdDone = false;
        cmdDropped = false;
    }
    wake();
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return cmdDone; });
    return cmdDropped;
}

void PrintServer::wake() {
    if (wakeSend == INVALID_SOCKET) return;
    char byte = 1;
    sendto(wakeSend, &byte, 1, 0, reinterpret_cast<sockaddr *>(&wakeAddr), sizeof wakeAddr);
}

void PrintServer::post(Ev event) {
    {
        std::lock_guard lock(qmu);
        queue.push_back(std::move(event));
    }
    if (ui) PostMessageW(ui, WM_SERVER, 0, 0);
}

void PrintServer::threadMain() {
    Loop loop;
    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bound.sin_port = 0;
    loop.wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bind(loop.wake, reinterpret_cast<sockaddr *>(&bound), sizeof bound);
    int nameLen = sizeof bound;
    getsockname(loop.wake, reinterpret_cast<sockaddr *>(&bound), &nameLen);
    u_long nb = 1;
    ioctlsocket(loop.wake, FIONBIO, &nb);
    wakeSend = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    wakeAddr = bound;
    {
        std::lock_guard lock(mu);
        threadStarted = true;
    }
    cv.notify_all();

    auto dropClient = [&](bool notify) {
        if (loop.client == INVALID_SOCKET) return false;
        closesocket(loop.client);
        loop.client = INVALID_SOCKET;
        loop.idleAt = 0;
        if (notify) post(Ev{EvKind::Closed, false, {}});
        return true;
    };

    auto acceptOn = [&](SOCKET listener) {
        for (;;) {
            SOCKET fd = accept(listener, nullptr, nullptr);
            if (fd == INVALID_SOCKET) return;
            if (loop.client != INVALID_SOCKET) {
                closesocket(fd);
                continue;
            }
            u_long one = 1;
            ioctlsocket(fd, FIONBIO, &one);
            loop.client = fd;
            loop.idleAt = GetTickCount64() + 10000;
            post(Ev{EvKind::Client, true, {}});
        }
    };

    auto readClient = [&] {
        if (loop.client == INVALID_SOCKET) return;
        for (;;) {
            int n = recv(loop.client, reinterpret_cast<char *>(loop.buf), sizeof loop.buf, 0);
            if (n > 0) {
                Ev ev;
                ev.kind = EvKind::Bytes;
                ev.bytes.assign(loop.buf, loop.buf + n);
                post(std::move(ev));
                loop.idleAt = GetTickCount64() + 10000;
                continue;
            }
            if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return;
            dropClient(true);
            return;
        }
    };

    for (;;) {
        Cmd local = Cmd::None;
        bool listen = false;
        int usePort = 0;
        bool lan = false;
        {
            std::lock_guard lock(mu);
            local = cmd;
            if (local != Cmd::None) cmd = Cmd::None;
            listen = cmdListen;
            usePort = cmdPort;
            lan = cmdLan;
        }
        if (local != Cmd::None) {
            bool dropped = false;
            if (local == Cmd::Quit) {
                dropClient(false);
                closeSocket(&loop.v4);
                closeSocket(&loop.v6);
                closeSocket(&loop.wake);
                {
                    std::lock_guard lock(mu);
                    cmdDone = true;
                }
                cv.notify_all();
                return;
            }
            if (local == Cmd::CloseClient) dropped = dropClient(false);
            if (local == Cmd::Apply) {
                dropped = dropClient(false);
                closeSocket(&loop.v4);
                closeSocket(&loop.v6);
                std::wstring nextAddress;
                std::wstring nextError;
                bool connected = false;
                if (!listen) {
                    nextAddress.clear();
                    nextError.clear();
                } else if (usePort < 1 || usePort > 65535) {
                    nextError = L"Port must be between 1 and 65535.";
                } else {
                    int err4 = 0;
                    int err6 = 0;
                    loop.v4 = listenOn(AF_INET, usePort, lan, &err4);
                    if (loop.v4 == INVALID_SOCKET) {
                        nextError = err4 == WSAEADDRINUSE ? portInUse(usePort) : portFailed(usePort);
                    } else {
                        loop.v6 = listenOn(AF_INET6, usePort, lan, &err6);
                        if (loop.v6 == INVALID_SOCKET && err6 == WSAEADDRINUSE) {
                            closeSocket(&loop.v4);
                            nextError = portInUse(usePort);
                        } else {
                            nextAddress = addressText(usePort, lan);
                        }
                    }
                }
                connected = loop.client != INVALID_SOCKET;
                address = nextAddress;
                bindError = nextError;
                clientConnected = connected;
            }
            {
                std::lock_guard lock(mu);
                cmdDropped = dropped;
                cmdDone = true;
            }
            cv.notify_all();
            continue;
        }

        if (loop.client != INVALID_SOCKET && loop.idleAt != 0 && GetTickCount64() >= loop.idleAt) {
            loop.idleAt = 0;
            post(Ev{EvKind::Idle, false, {}});
        }
        WSAPOLLFD fds[4];
        int count = 0;
        auto add = [&](SOCKET fd) {
            if (fd == INVALID_SOCKET) return;
            fds[count].fd = fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        };
        add(loop.wake);
        add(loop.v4);
        add(loop.v6);
        add(loop.client);
        int timeout = -1;
        if (loop.client != INVALID_SOCKET && loop.idleAt != 0) {
            unsigned long long now = GetTickCount64();
            timeout = loop.idleAt > now ? static_cast<int>(loop.idleAt - now) : 0;
        }
        int polled = WSAPoll(fds, static_cast<ULONG>(count), timeout);
        if (polled < 0 && WSAGetLastError() == WSAEINTR) continue;
        int index = 0;
        auto ready = [&](SOCKET fd) {
            if (fd == INVALID_SOCKET) return false;
            bool on = (fds[index].revents & (POLLIN | POLLERR | POLLHUP)) != 0;
            ++index;
            return on;
        };
        if (ready(loop.wake)) {
            char trash[32];
            while (recv(loop.wake, trash, sizeof trash, 0) > 0) {}
        }
        if (ready(loop.v4)) acceptOn(loop.v4);
        if (ready(loop.v6)) acceptOn(loop.v6);
        if (ready(loop.client)) readClient();
    }
}
