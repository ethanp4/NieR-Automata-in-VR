// gpuprof.h - GPU timing of one frame every 5 s, pass by pass (gpuprof.cpp, diagnostic).
#pragma once
#include <d3d11.h>

void GpuProfFrameStart(ID3D11DeviceContext* c);
void GpuProfMark(ID3D11DeviceContext* c, const char* label);
void GpuProfTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
					const char* prefix);
void GpuProfCompute(ID3D11DeviceContext* c, const char* prefix, int kind);
void GpuProfDraw();
void GpuProfFrameEnd(ID3D11DeviceContext* c);
void GpuProfPoll(ID3D11DeviceContext* c);
