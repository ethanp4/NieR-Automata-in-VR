// gpu_timing.h - GPU timing of one frame every 5 s, pass by pass (gpu_timing.cpp, diagnostic).
#pragma once
#include <d3d11.h>

void GpuTimingFrameStart(ID3D11DeviceContext* c);
void GpuTimingMark(ID3D11DeviceContext* c, const char* label);
void GpuTimingTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
					  const char* prefix);
void GpuTimingCompute(ID3D11DeviceContext* c, const char* prefix, int kind);
void GpuTimingDraw();
void GpuTimingFrameEnd(ID3D11DeviceContext* c);
void GpuTimingPoll(ID3D11DeviceContext* c);
