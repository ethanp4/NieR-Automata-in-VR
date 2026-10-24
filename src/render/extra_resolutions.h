// extra_resolutions.h - lets the game render above the monitor's resolution (extra_resolutions.cpp).
#pragma once
#include <d3d11.h>

void ExtraResolutionsInstallHooks();
void ExtraResolutionsEarlyHooks();  // from DllMain: patches code only
void ExtraResolutionsHookSwapChain(IDXGISwapChain* sc);
void ExtraResolutionsOnFirstPresent(IDXGISwapChain* sc);
