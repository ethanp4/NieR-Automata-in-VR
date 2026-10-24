// frame_capture.h - [Debug] CaptureKey (F11): records one frame of the game's D3D11 calls to a file.
#pragma once
#include <d3d11.h>
#include "render/shader_table.h"

void CaptureSetDevice(ID3D11Device* device, ID3D11DeviceContext* context);  // the game's (d3d_hooks.cpp)
void CaptureBeforePresent();  // ends a capture; reads the hotkey
void CaptureAfterPresent();   // starts the capture the hotkey asked for
bool CaptureRecording(ID3D11DeviceContext* c);  // a capture runs and `c` is the game's context

// Called by the hooks while CaptureRecording(c)
void CaptureSetConstantBuffers(ShaderStage stage, UINT start, UINT n, ID3D11Buffer* const* buffers);
void CaptureSetShader(ShaderStage stage, void* shader);
void CaptureDraw(uint8_t kind, UINT a, UINT b, UINT c);  // kind: 0 Draw, 1 DrawIndexed, 2 DrawInstanced, 3 DrawIndexedInstanced
void CaptureMap(ID3D11Resource* buffer, void* data, UINT size);
void CaptureUnmap(ID3D11Resource* buffer);
void CaptureUpdate(ID3D11Resource* buffer, const void* data, UINT size);
void CaptureSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void CaptureViewports(UINT n, const D3D11_VIEWPORT* viewports);
void CaptureExecuteCommandList();
void CaptureShaderResources(ShaderStage stage, UINT start, UINT n, ID3D11ShaderResourceView* const* views);
void CaptureUnorderedAccessViews(UINT start, UINT n, ID3D11UnorderedAccessView* const* views);
void CaptureDispatch(UINT x, UINT y, UINT z);
