// com.h - COM references: ComPtr for local temporaries (released when they go out of scope), and
// SafeRelease for long-lived raw pointers. Globals stay raw on purpose: a ComPtr global would release
// its object from a destructor while the game exits (DLL unload, under the loader lock).
#pragma once
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

template <typename T>
void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}
