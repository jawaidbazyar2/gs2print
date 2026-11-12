#include "associate.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace {

void setSz(const wchar_t *key, const wchar_t *name, const std::wstring &value) {
    const BYTE *bytes = reinterpret_cast<const BYTE *>(value.c_str());
    DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, bytes, size);
}

}  // namespace

void registerFileAssociation() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring command = L"\"";
    command += exe;
    command += L"\" \"%1\"";
    setSz(L"Software\\Classes\\.gs2print", nullptr, L"net.gssquared.gs2print.ticket");
    setSz(L"Software\\Classes\\net.gssquared.gs2print.ticket", nullptr, L"GS2 printer ticket");
    setSz(L"Software\\Classes\\net.gssquared.gs2print.ticket\\shell\\open\\command", nullptr, command);
}
