#include "text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwctype>

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int bytes = static_cast<int>(utf8.size());
    int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), bytes, nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), bytes, out.data(), count);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    int chars = static_cast<int>(wide.size());
    int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), chars, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), chars, out.data(), count, nullptr, nullptr);
    return out;
}

std::wstring fileName(std::wstring_view path) {
    auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return std::wstring(path);
    return std::wstring(path.substr(slash + 1));
}

std::wstring systemMessage(unsigned long code) {
    wchar_t *buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
    std::wstring text = n && buf ? buf : L"Unknown error";
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}
