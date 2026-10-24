// screenfx.h - NieR's low-health grey and distortion, removed in stereo (screenfx.cpp).
#pragma once
#include <d3d11.h>
#include <stdint.h>

void ScreenFxBeginFrame();
// An effect-system draw (particles, screen filters) that samples the texture registers in `sampled`:
// true = skip it (a low-health screen filter while in stereo, LowHealthEffect=0).
bool ScreenFxSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash);
