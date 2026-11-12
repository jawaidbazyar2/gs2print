#pragma once

#include "gs2print.h"
#include "preview.hpp"

#include <memory>
#include <string>
#include <vector>

class TicketClient;

struct Session {
    bool isPrimary = false;
    HWND hwnd = nullptr;
    bool printing = false;
    bool ownsTCP = false;
    std::wstring message = L"Open a printer dump.";
    std::wstring error;
    std::wstring pdfPath;
    int pageIndex = 0;
    int pageCount = 0;
    PagePreview preview;

    Session();
    ~Session();
    bool isIdle() const;
    bool canCloseJob() const;
    bool holdsSocket(const std::string &path) const;
    void attach(HWND window);
    void openDump(const std::wstring &path);
    void openTicket(const std::string &socketPath);
    void showError(const std::wstring &text);
    void raise();
    void step(int delta);
    void closeActiveJob();
    void receive(const uint8_t *data, size_t len);
    void finishNetworkJob();
    void drainTicket();
    void shutdownConnection();
    void onTimer();
    void openSavedPdf();
    void zoom(double factor);
    void fitWidth();
    void fitHeight();
    bool canZoom(double factor) const;

private:
    gs2_job *job = nullptr;
    bool networkJobOpen = false;
    bool sawBytes = false;
    bool followTail = true;
    unsigned long long previewDue = 0;
    bool previewScheduled = false;
    uint64_t generation = 1;
    std::unique_ptr<TicketClient> ticket;
    std::string ticketPath;

    void relinquish();
    void beginNetworkJob();
    void noteInk();
    void refreshLivePreview();
    void requestPreview();
    void failTicket(const std::wstring &text);
    std::wstring saveDocuments();
};
