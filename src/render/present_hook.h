// present_hook.h - hooks IDXGISwapChain::Present and copies each finished game frame into the
// textures shared with the VR thread (present_hook.cpp).
#pragma once
#include <d3d11.h>

void HookFromSwapChain(IDXGISwapChain* sc);
void HookWithDummySwapChain();
void CapturePollMidFrame();  // publish finished frames without waiting for Present
