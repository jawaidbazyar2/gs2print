#pragma once

#include <windows.h>

struct Session;

void registerJobClass(HINSTANCE inst);
HINSTANCE jobInstance();
HWND createJobWindow(Session *session, HINSTANCE inst, int show);
void syncWindow(Session *session);
