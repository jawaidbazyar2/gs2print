#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TicketClient {
public:
    enum class Kind { Ready, Bytes, Idle, Closed, Failed };

    struct Event {
        Kind kind = Kind::Closed;
        std::vector<uint8_t> bytes;
        std::wstring text;
    };

    TicketClient(std::string path, HWND hwnd);
    ~TicketClient();

    void start();
    void stop();
    std::vector<Event> take();

private:
    void readerMain();
    void idleMain();
    void push(Event event);
    void publishSocket(SOCKET fd);
    void publishPipe(HANDLE pipe);

    std::string path;
    HWND hwnd = nullptr;
    std::atomic<bool> stopRequested{false};
    std::mutex mu;
    std::vector<Event> queue;
    SOCKET sock = INVALID_SOCKET;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::atomic<unsigned long long> idleDeadline{0};
    std::thread reader;
    std::thread idle;
    bool started = false;
};
