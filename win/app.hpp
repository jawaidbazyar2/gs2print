#pragma once

#include "server.hpp"
#include "session.hpp"

#include <memory>
#include <string>
#include <vector>

struct App {
    PrintServer server;
    Session primary;
    std::vector<std::unique_ptr<Session>> extras;
    Session *focused = nullptr;
    Session *tcp = nullptr;

    App();
    void openPaths(const std::vector<std::wstring> &paths);
    void handleFile(const std::wstring &path);
    void raisePrimary();
    void sessionClosed(Session *session);
    void refreshAll();
    int windowCount() const;
    void quit();

    void onBytes(std::vector<uint8_t> bytes);
    void onIdle();
    void onClient(bool connected);
    void onDisconnect();

private:
    struct Seen {
        std::wstring path;
        unsigned long long at = 0;
    };
    std::vector<Seen> seen;
    Session *claimIdle();
    Session *dumpTarget();
    bool isLive(const std::string &socket) const;
};

App &app();
void setApp(App *instance);
