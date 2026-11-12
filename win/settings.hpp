#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void showSettings(HWND owner);

bool autoLineFeed();
void setAutoLineFeed(bool on);

bool storedListen();
void storeListen(bool on);
int storedPort();
void storePort(int port);
bool storedLan();
void storeLan(bool on);
