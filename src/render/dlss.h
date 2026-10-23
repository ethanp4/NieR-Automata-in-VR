// dlss.h - NVIDIA DLSS through the driver's NGX loader (dlss.cpp), used by upscale.cpp.
#pragma once
#include "core/common.h"

struct DlssInputs {
	ID3D11Resource* color;   // render size, typed format
	ID3D11Resource* depth;   // render size, R32_FLOAT (the game's depth values)
	ID3D11Resource* motion;  // render size, R16G16_FLOAT, pixels: where each pixel was in the previous frame
	ID3D11Resource* output;  // output size, UAV-capable
	float jitterX, jitterY;  // this frame's sub-pixel offset, in render pixels
	bool reset;              // history is useless (first frame, camera cut, size change)
};

// Loads _nvngx.dll and starts NGX on the game's device (once). False: DLSS is unavailable; the
// reason is logged and kept for DlssStatus.
bool DlssInit(ID3D11Device* dev);
// The DLSS feature for one eye at these sizes, (re)created when something changed.
// quality: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra performance; model: 0 CNN (preset E),
// 1 transformer (preset K).
bool DlssPrepare(ID3D11DeviceContext* c, int eye, UINT rw, UINT rh, UINT ow, UINT oh, int quality, int model,
				 bool depthInverted);
bool DlssEvaluate(ID3D11DeviceContext* c, int eye, const DlssInputs& in);
void DlssReleaseFeatures();
const char* DlssStatus();  // "DLSS 310.9.16" or why it isn't available
void DlssPoll(ID3D11Device* dev);  // game thread, every Present: carries out a delete asked for in the menu
