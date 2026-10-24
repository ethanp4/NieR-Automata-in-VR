// game_effects.h - switches off NieR effects that don't suit VR: vignette, shadows, ambient occlusion, low-health filter (game_effects.cpp).
#pragma once
#include <d3d11.h>
#include <stdint.h>
#include "render/shader_table.h"

void GameEffectsBeginFrame();
// Before a draw with pixel shader class `ps` (shadows, ambient occlusion). True = skip the draw.
bool GameEffectsBeforeDraw(ID3D11DeviceContext* c, PixelShaderClass ps);
void GameEffectsAfterDraw(ID3D11DeviceContext* c);  // puts back a shader swapped for the draw
// An effect-system draw (particles, screen filters) that samples the texture registers in `sampled`:
// true = skip it (the low-health filter while in stereo, LowHealthEffect=0).
bool GameEffectsSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash);

// Vignette: its constant buffer is patched as the game uploads it
void GameEffectsOnMap(ID3D11Resource* r, void* data, UINT size);
void GameEffectsBeforeUnmap(ID3D11Resource* r);
// UpdateSubresource and replay: returns `data`, or `scratch` (at least 48 bytes) holding a patched copy
const void* GameEffectsPatchUpdate(ID3D11Resource* r, const void* data, UINT size, float* scratch);
