// xr_main.h - the VR thread (xr_main.cpp).
#pragma once
#include <windows.h>

void XrStartThread();
extern volatile DWORD g_xrThreadId;
