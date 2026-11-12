#pragma once

#include <string>
#include <string_view>

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
std::wstring fileName(std::wstring_view path);
std::wstring systemMessage(unsigned long code);
