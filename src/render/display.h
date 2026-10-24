// display.h - render resolution: extra resolutions and the swapchain (display.cpp).
#pragma once
#include <d3d11.h>

void DisplayInstallHooks();
void DisplayEarlyHooks();  // from DllMain: patches code only
void DisplayHookSwapChain(IDXGISwapChain* sc);
void DisplayOnFirstPresent(IDXGISwapChain* sc);
