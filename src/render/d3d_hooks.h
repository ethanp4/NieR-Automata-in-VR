// d3d_hooks.h - hooks on the game's D3D11 device and immediate context, passing each call on to the
// features that need it (camera patching, HUD split, both-eyes recording, effect toggles, capture).
#pragma once
#include "core/common.h"

void D3dHooksInstall(ID3D11Device* gameDevice);         // once, on the first device the game creates
void D3dHooksFollowDevice(ID3D11Device* presentingDevice);  // if the game presents from another device
void FrameLatencyTick();  // applies [Render] MaxFrameLatency to the game's device (every Present)

// The unhooked D3D11 functions, for the mod's own calls on the game's context
void OriginalDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
void OriginalDraw(ID3D11DeviceContext* c, const DrawArgs& d);
void OriginalSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
							  ID3D11DepthStencilView* dsv);

// Second-eye playback (replay.cpp): a recorded constant-buffer upload with the same patching as the
// live hooks (eye camera, vignette). Returns `data` or `scratch` (64 floats).
const void* PatchConstantBufferForReplay(ID3D11Resource* r, const void* data, UINT size, float* scratch);
