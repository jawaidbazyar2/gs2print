#pragma once

#include <winsock2.h>
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PrintServer {
public:
    bool listening = false;
    int port = 9100;
    bool allowLAN = false;
    std::wstring address;
    std::wstring bindError;
    bool clientConnected = false;

    std::function<void(std::vector<uint8_t>)> onBytes;
    std::function<void()> onIdle;
    std::function<void()> onDisconnect;
    std::function<void(bool)> onClient;

    PrintServer();
    ~PrintServer();

    void start(HWND ui);
    void restore();
    bool commitPort(const std::wstring &text, bool rebindIfUnchanged);
    void setListening(bool on);
    void setAllowLAN(bool on);
    bool closeClient();
    void drain();

private:
    enum class Cmd { None, Apply, CloseClient, Quit };
    enum class EvKind { Bytes, Idle, Client, Closed };

    struct Ev {
        EvKind kind = EvKind::Closed;
        bool connected = false;
        std::vector<uint8_t> bytes;
    };

    HWND ui = nullptr;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    Cmd cmd = Cmd::None;
    bool cmdListen = false;
    int cmdPort = 9100;
    bool cmdLan = false;
    bool cmdDropped = false;
    bool cmdDone = false;

    std::mutex qmu;
    std::vector<Ev> queue;

    SOCKET wakeSend = INVALID_SOCKET;
    sockaddr_in wakeAddr{};
    bool threadStarted = false;

    void threadMain();
    bool request(Cmd which);
    void applyNow();
    void wake();
    void post(Ev event);
};
