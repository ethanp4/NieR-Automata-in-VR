// first_person.h - first person: 2B's head and feet from her bone uploads, her head hidden from your eyes.
#pragma once
#include <d3d11.h>
#include "render/head_bone.h"

// From the D3D11 hooks (the game's immediate context only)
void FirstPersonOnSetVSConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11Buffer* const* buffers);
void FirstPersonOnMap(ID3D11Resource* res, void* data, UINT byteWidth);
void FirstPersonBeforeUnmap(ID3D11Resource* res);
void FirstPersonOnSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
// Before each draw: true = skip it (her head in the main view while you see through her eyes, unless
// FirstPersonShowHead).
bool FirstPersonSkipDraw();

void FirstPersonNewFrame();                             // camera.cpp, after each Present
void FirstPersonSetGameSize(UINT width, UINT height);  // the game's resolution (frame_handoff.cpp, each Present)
bool FirstPersonHead(pure::HeadPose* out);              // her head as last drawn (false = not seen lately)
bool FirstPersonSteadyEyeHeight(float* out);            // her eyes' height without running bob (FirstPersonSteadyHeight)
void FirstPersonSetHiding(bool hide);                   // camera.cpp: this frame's eyes are in her head
// camera.cpp, each game frame in her eyes: your head (between your eyes, camera-to-world) and where it is
// from her eyes (world; the camera's own height / forward offsets left out). Her body follows it (FirstPersonBody).
void FirstPersonSetViewer(const pure::M4& view, pure::V3 fromHerEyes);
// camera.cpp, with the viewer: the controllers' grip poses in the world (camera-to-world style). Her arms follow them (FirstPersonArms).
void FirstPersonSetHands(const bool valid[2], const pure::M4 grips[2]);
bool FirstPersonFeet(pure::V3* out);  // the ground under her hips, as the game posed her (false = not seen lately)
