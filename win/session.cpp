#include "ticket.hpp"

#include "session.hpp"

#include "app.hpp"
#include "settings.hpp"
#include "text.hpp"
#include "window.hpp"

#include <knownfolders.h>
#include <shlobj.h>

#include <vector>

namespace {

std::vector<uint8_t> readAll(const std::wstring &path, std::wstring &error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = systemMessage(GetLastError());
        return {};
    }
    std::vector<uint8_t> data;
    uint8_t buf[64 * 1024];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(file, buf, sizeof buf, &n, nullptr)) {
            error = systemMessage(GetLastError());
            CloseHandle(file);
            return {};
        }
        if (n == 0) break;
        data.insert(data.end(), buf, buf + n);
    }
    CloseHandle(file);
    return data;
}

std::wstring pdfBeside(const std::wstring &source) {
    auto slash = source.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"" : source.substr(0, slash + 1);
    std::wstring name = slash == std::wstring::npos ? source : source.substr(slash + 1);
    auto dot = name.rfind(L'.');
    std::wstring pdfName;
    if (dot != std::wstring::npos && name.size() - dot - 1 >= 1 && name.size() - dot - 1 <= 8) {
        pdfName = name.substr(0, dot) + L".pdf";
    } else {
        pdfName = name + L".pdf";
    }
    return dir + pdfName;
}

bool fileExists(const std::wstring &path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

Session::Session() = default;

Session::~Session() {
    ticket.reset();
    preview.shutdown();
    if (job) {
        gs2_job_free(job);
        job = nullptr;
    }
}

bool Session::isIdle() const {
    return !ticket && !ownsTCP && !networkJobOpen && !printing && pdfPath.empty() && !preview.hasImage() &&
           pageCount == 0 && error.empty();
}

bool Session::canCloseJob() const {
    return ticket != nullptr || (ownsTCP && app().server.clientConnected);
}

bool Session::holdsSocket(const std::string &path) const { return ticket != nullptr && ticketPath == path; }

void Session::attach(HWND window) {
    hwnd = window;
    preview.attach(GetDlgItem(window, IDC_VIEW));
}

void Session::showError(const std::wstring &text) {
    error = text;
    syncWindow(this);
}

void Session::raise() {
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
}

void Session::step(int delta) {
    int next = pageIndex + delta;
    if (next < 0 || next >= pageCount) return;
    pageIndex = next;
    followTail = next == pageCount - 1;
    preview.notePage(pageIndex);
    requestPreview();
    syncWindow(this);
}

void Session::closeActiveJob() { relinquish(); }

void Session::openSavedPdf() {
    if (pdfPath.empty()) return;
    HINSTANCE opened = ShellExecuteW(hwnd, L"open", pdfPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) {
        error = L"Could not open " + fileName(pdfPath) + L".";
        syncWindow(this);
    }
}

void Session::zoom(double factor) {
    HWND view = hwnd ? GetDlgItem(hwnd, IDC_VIEW) : nullptr;
    if (!view) return;
    RECT rect{};
    GetClientRect(view, &rect);
    preview.zoom(factor, rect.right, rect.bottom, static_cast<int>(GetDpiForWindow(view)));
    InvalidateRect(view, nullptr, FALSE);
    syncWindow(this);
}

void Session::fitWidth() {
    preview.fitWidth();
    if (HWND view = hwnd ? GetDlgItem(hwnd, IDC_VIEW) : nullptr) InvalidateRect(view, nullptr, FALSE);
}

void Session::fitHeight() {
    preview.fitHeight();
    if (HWND view = hwnd ? GetDlgItem(hwnd, IDC_VIEW) : nullptr) InvalidateRect(view, nullptr, FALSE);
}

bool Session::canZoom(double factor) const {
    HWND view = hwnd ? GetDlgItem(hwnd, IDC_VIEW) : nullptr;
    if (!view || !preview.hasImage()) return false;
    RECT rect{};
    GetClientRect(view, &rect);
    return preview.canZoom(factor, rect.right, rect.bottom, static_cast<int>(GetDpiForWindow(view)));
}

void Session::openDump(const std::wstring &path) {
    relinquish();
    error.clear();
    pdfPath.clear();
    followTail = false;
    std::wstring readError;
    std::vector<uint8_t> data = readAll(path, readError);
    if (!readError.empty()) {
        error = readError;
        syncWindow(this);
        return;
    }
    size_t offset = 0;
    if (!data.empty()) offset = gs2_dump_payload_offset(data.data(), data.size());
    if (job) {
        gs2_job_free(job);
        job = nullptr;
    }
    preview.clear();
    job = gs2_job_new();
    if (!job) {
        error = L"Could not start a print job.";
        syncWindow(this);
        return;
    }
    gs2_job_set_auto_lf(job, autoLineFeed() ? 1 : 0);
    if (offset < data.size()) gs2_job_write(job, data.data() + offset, data.size() - offset);
    gs2_job_finish(job);
    pageCount = gs2_job_page_count(job);
    pageIndex = 0;
    preview.notePage(0);
    message = fileName(path);
    printing = false;
    networkJobOpen = false;
    requestPreview();

    std::wstring pdf = pdfBeside(path);
    std::string utf8 = narrow(pdf);
    if (gs2_job_save_pdf(job, utf8.c_str()) == 0) {
        pdfPath = pdf;
        error.clear();
    } else {
        pdfPath.clear();
        error = L"Could not write " + fileName(pdf) + L".";
    }
    syncWindow(this);
}

void Session::openTicket(const std::string &socketPath) {
    relinquish();
    error.clear();
    pdfPath.clear();
    preview.clear();
    pageIndex = 0;
    pageCount = 0;
    message = L"Connecting";
    printing = true;
    ticketPath = socketPath;
    ticket = std::make_unique<TicketClient>(socketPath, hwnd);
    raise();
    ticket->start();
    syncWindow(this);
}

void Session::receive(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    if (!networkJobOpen) beginNetworkJob();
    if (!networkJobOpen || !job) return;
    sawBytes = true;
    gs2_job_write(job, data, len);
    noteInk();
}

void Session::finishNetworkJob() {
    if (!networkJobOpen) return;
    networkJobOpen = false;
    printing = false;
    previewScheduled = false;
    if (hwnd) KillTimer(hwnd, kPreviewTimer);
    if (!sawBytes || !job) {
        syncWindow(this);
        return;
    }
    gs2_job_finish(job);
    int count = gs2_job_page_count(job);
    pageCount = count;
    if (count <= 0) {
        preview.clear();
        syncWindow(this);
        return;
    }
    if (followTail || pageIndex >= count) pageIndex = count - 1;
    preview.notePage(pageIndex);
    requestPreview();
    std::wstring pdf = saveDocuments();
    std::string utf8 = narrow(pdf);
    if (gs2_job_save_pdf(job, utf8.c_str()) == 0) {
        pdfPath = pdf;
        error.clear();
        message = fileName(pdf);
    } else {
        pdfPath.clear();
        error = L"Could not write " + fileName(pdf) + L".";
    }
    syncWindow(this);
}

void Session::drainTicket() {
    if (!ticket) return;
    auto events = ticket->take();
    for (const auto &event : events) {
        switch (event.kind) {
        case TicketClient::Kind::Ready:
            message = L"Printing";
            printing = true;
            raise();
            break;
        case TicketClient::Kind::Bytes:
            receive(event.bytes.data(), event.bytes.size());
            break;
        case TicketClient::Kind::Idle:
        case TicketClient::Kind::Closed:
            relinquish();
            break;
        case TicketClient::Kind::Failed:
            failTicket(event.text);
            break;
        }
        if (!ticket) break;
    }
    syncWindow(this);
}

void Session::shutdownConnection() {
    relinquish();
    if (job) {
        gs2_job_free(job);
        job = nullptr;
    }
}

void Session::onTimer() {
    previewScheduled = false;
    if (!networkJobOpen) return;
    previewDue = GetTickCount64() + 100;
    refreshLivePreview();
}

void Session::relinquish() {
    if (ticket) {
        std::unique_ptr<TicketClient> client = std::move(ticket);
        ticketPath.clear();
        client->stop();
        if (networkJobOpen) finishNetworkJob();
        else {
            printing = false;
            if (pdfPath.empty() && !preview.hasImage()) message = L"Open a printer dump.";
        }
        syncWindow(this);
        return;
    }
    if (ownsTCP) {
        ownsTCP = false;
        app().server.closeClient();
        return;
    }
    if (networkJobOpen) finishNetworkJob();
}

void Session::failTicket(const std::wstring &text) {
    if (ticket) {
        std::unique_ptr<TicketClient> client = std::move(ticket);
        ticketPath.clear();
        client->stop();
    }
    printing = false;
    error = text;
    message = L"Open a printer dump.";
    syncWindow(this);
}

void Session::beginNetworkJob() {
    pdfPath.clear();
    preview.clear();
    if (job) {
        gs2_job_free(job);
        job = nullptr;
    }
    job = gs2_job_new();
    if (!job) {
        error = L"Could not start a print job.";
        networkJobOpen = false;
        syncWindow(this);
        return;
    }
    gs2_job_set_auto_lf(job, autoLineFeed() ? 1 : 0);
    networkJobOpen = true;
    sawBytes = false;
    followTail = true;
    pageIndex = 0;
    pageCount = 1;
    error.clear();
    message = L"Printing";
    printing = true;
    raise();
    syncWindow(this);
}

void Session::noteInk() {
    unsigned long long now = GetTickCount64();
    if (now >= previewDue) {
        previewDue = now + 100;
        refreshLivePreview();
        return;
    }
    if (previewScheduled || !hwnd) return;
    previewScheduled = true;
    UINT delay = static_cast<UINT>(previewDue - now);
    SetTimer(hwnd, kPreviewTimer, delay == 0 ? 1 : delay, nullptr);
}

void Session::refreshLivePreview() {
    if (!networkJobOpen || !job) return;
    int count = gs2_job_page_count(job);
    if (count <= 0) return;
    pageCount = count;
    if (followTail || pageIndex >= count) pageIndex = count - 1;
    preview.notePage(pageIndex);
    requestPreview();
    syncWindow(this);
}

void Session::requestPreview() {
    if (!job) return;
    int width = 0;
    int height = 0;
    int stride = 0;
    const uint8_t *bits = gs2_job_page_bits(job, pageIndex, &width, &height, &stride);
    if (!bits || width <= 0 || height <= 0 || stride <= 0) return;
    std::vector<uint8_t> copy(bits, bits + static_cast<size_t>(height) * static_cast<size_t>(stride));
    preview.submit(std::move(copy), width, height, stride, ++generation);
}

std::wstring Session::saveDocuments() {
    PWSTR docs = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        dir = docs;
        CoTaskMemFree(docs);
    } else {
        wchar_t profile[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        dir = n > 0 && n < MAX_PATH ? std::wstring(profile) + L"\\Documents" : L".";
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[40];
    swprintf_s(stamp, L"GS2-Print-%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond);
    std::wstring path = dir + L"\\" + stamp + L".pdf";
    for (int n = 2; fileExists(path); ++n) {
        path = dir + L"\\" + stamp + L"-" + std::to_wstring(n) + L".pdf";
    }
    return path;
}
