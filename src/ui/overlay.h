// overlay.h - the stats overlay, drawn with Dear ImGui on the VR thread ([Screen] StatsOverlay, overlay.cpp).
#pragma once
#include <d3d11.h>

struct OverlayStats {
	const char* mode;  // "alternate eyes", "flat screen", ...
	float headsetFps;
	float gameFps[3];              // game images per second: left, right, flat
	unsigned gameW, gameH;         // the game's render size
	unsigned sceneW, sceneH;       // the 3D image's size in the headset (larger when upscaling)
	const char* upscaler;          // "DLSS 310.9.16 Quality" or why not; "" = off
	float sharpX, sharpY;          // % of the headset's recommended detail, across / up-down; 0 = unknown
	float poseDelayMs, predictMs;  // poseDelayMs < 0 = unknown
	const float* frameMs;          // headset frame times, a ring buffer for the graph
	int frameCount, frameOffset;   // its size, and the index of the oldest entry
};
bool OverlayRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h,
				   const OverlayStats& s);
