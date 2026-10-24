// d3d_hooks.cpp - hooks on the game's D3D11 device and immediate context. Every call the game makes
// is passed on to the features that need it: camera patching (camera.cpp), the HUD split
// (hud_layer.cpp), synchronized recording (replay.cpp), the view-space fix (viewfix.cpp), GPU timing,
// the frame capture, and the game-effect toggles (game_effects.cpp).
// While replay.cpp plays a frame back, the hooks pass calls straight through.
#include "render/d3d_hooks.h"
#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "debug/frame_capture.h"
#include "debug/gpu_timing.h"
#include "render/camera.h"
#include "render/frame_handoff.h"
#include "render/game_effects.h"
#include "render/hud_layer.h"
#include "render/replay.h"
#include "render/reproject.h"
#include "render/shader_table.h"
#include "render/viewfix.h"

namespace {
// vtable slots (verified against the d3d11.h layout at build time, see tools/vtable_check.c)
enum {
	CTX_VSSetConstantBuffers = 7,
	CTX_PSSetShaderResources = 8,
	CTX_PSSetShader = 9,
	CTX_VSSetShader = 11,
	CTX_DrawIndexed = 12,
	CTX_Draw = 13,
	CTX_Map = 14,
	CTX_Unmap = 15,
	CTX_PSSetConstantBuffers = 16,
	CTX_DrawIndexedInstanced = 20,
	CTX_DrawInstanced = 21,
	CTX_GSSetConstantBuffers = 22,
	CTX_OMSetRenderTargets = 33,
	CTX_Dispatch = 41,
	CTX_RSSetViewports = 44,
	CTX_UpdateSubresource = 48,
	CTX_ExecuteCommandList = 58,
	CTX_CSSetShaderResources = 67,
	CTX_CSSetUnorderedAccessViews = 68,
	CTX_CSSetShader = 69,
	CTX_CSSetConstantBuffers = 71,
	DEV_CreateVertexShader = 12,
	DEV_CreatePixelShader = 15,
	DEV_CreateComputeShader = 18,
	DEV_CreateDeferredContext = 27,
};

ID3D11Device* g_dev;
ID3D11DeviceContext* g_ctx;
volatile LONG g_deferredContexts;

typedef void(STDMETHODCALLTYPE* PFN_SetCB)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext*, void*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
											D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
											 ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_UpdateSubresource)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
													   const void*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandList)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_SetSRV)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetUAV)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
											const UINT*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeferredContext)(ID3D11Device*, UINT, ID3D11DeviceContext**);

PFN_SetCB o_SetCB[4];
PFN_SetShader o_VSSetShader, o_PSSetShader, o_CSSetShader;
PFN_DrawIndexed o_DrawIndexed;
PFN_Draw o_Draw;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced;
PFN_DrawInstanced o_DrawInstanced;
PFN_Map o_Map;
PFN_Unmap o_Unmap;
PFN_OMSetRT o_OMSetRT;
PFN_UpdateSubresource o_UpdateSubresource;
PFN_RSSetViewports o_RSSetViewports;
PFN_ExecuteCommandList o_ExecuteCommandList;
PFN_Dispatch o_Dispatch;
PFN_SetSRV o_PSSetSRV, o_CSSetSRV;
PFN_SetUAV o_CSSetUAV;
PFN_CreateShader o_CreateVS, o_CreatePS, o_CreateCS;
PFN_CreateDeferredContext o_CreateDeferredContext;

bool IsConstantBuffer(ID3D11Resource* r, UINT* size) {
	D3D11_RESOURCE_DIMENSION dim;
	r->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
	D3D11_BUFFER_DESC d;
	((ID3D11Buffer*)r)->GetDesc(&d);
	*size = d.ByteWidth;
	return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

// The game's current pixel and compute shaders
PixelShaderClass g_psClass;
uint32_t g_psSampled;  // texture registers it samples
uint64_t g_psHash;     // bytecode hash (stable between runs, for the log)
ComputeShaderClass g_csClass;

// --- hooks -------------------------------------------------------------------------------------
template <int Stage>
void STDMETHODCALLTYPE H_SetCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs) {
	if (g_replaying) return o_SetCB[Stage](c, start, n, bufs);
	if (Stage == ST_VS && c == g_ctx && bufs) UiOnSetVSConstantBuffers(start, n, bufs);
	if (RecOn(c)) RecSetCB(Stage == ST_VS ? RS_VS : Stage == ST_PS ? RS_PS
												: Stage == ST_GS   ? RS_GS
																   : RS_CS,
						   start, n, bufs);
	if (CaptureRecording(c)) CaptureSetConstantBuffers((ShaderStage)Stage, start, n, bufs);
	o_SetCB[Stage](c, start, n, bufs);
}

void STDMETHODCALLTYPE H_VSSetShader(ID3D11DeviceContext* c, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
	if (g_replaying) return o_VSSetShader(c, sh, ci, n);
	if (RecOn(c)) RecSetShader(RS_VS, sh, ci, n);
	if (c == g_ctx) UiOnSetVS(ShaderTableIsUi(sh));
	if (CaptureRecording(c)) CaptureSetShader(ST_VS, sh);
	o_VSSetShader(c, sh, ci, n);
}

void STDMETHODCALLTYPE H_PSSetShader(ID3D11DeviceContext* c, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
	if (g_replaying) return o_PSSetShader(c, sh, ci, n);
	if (RecOn(c)) RecSetShader(RS_PS, sh, ci, n);
	if (c == g_ctx) {
		PixelShaderClass was = g_psClass;
		g_psClass = ShaderTableClassifyPixel(sh, &g_psSampled, &g_psHash);
		// GPU timing: the expensive full-screen passes get their own marks
		static const char* const kNames[6] = {nullptr, "AO (SAO)", "shadow mask", nullptr, nullptr, "light scattering"};
		if (g_psClass != was) {
			const char* now = kNames[g_psClass];
			if (now) GpuTimingMark(c, now);
			else if (kNames[was]) GpuTimingMark(c, "(after a full-screen pass)");
		}
	}
	if (CaptureRecording(c)) CaptureSetShader(ST_PS, sh);
	o_PSSetShader(c, sh, ci, n);
}

// Shared by the four draw hooks: true = the game's draw must not run.
bool PreDraw(ID3D11DeviceContext* c) {
	CapturePollMidFrame();
	StereoCountDraw();
	if (GameEffectsBeforeDraw(c, g_psClass)) return true;
	if (UiBeforeDraw(c)) return true;
	if (g_psClass == PS_EFFECT && GameEffectsSkipEffectDraw(c, g_psSampled, g_psHash)) return true;
	GpuTimingDraw();
	return false;
}
void PostDraw(ID3D11DeviceContext* c, const DrawArgs& a) {
	GameEffectsAfterDraw(c);
	UiAfterDraw(c, a);
}

void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT n, UINT s) {
	if (g_replaying) return o_Draw(c, n, s);
	bool game = c == g_ctx;
	if (game && PreDraw(c)) return;
	if (RecOn(c)) RecDraw(0, n, s, 0, 0, 0);
	if (CaptureRecording(c)) CaptureDraw(0, n, s, 0);
	o_Draw(c, n, s);
	if (game) PostDraw(c, {0, n, s, 0, 0, 0});
}
void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b) {
	if (g_replaying) return o_DrawIndexed(c, n, s, b);
	bool game = c == g_ctx;
	if (game && PreDraw(c)) return;
	if (RecOn(c)) RecDraw(1, n, s, b, 0, 0);
	if (CaptureRecording(c)) CaptureDraw(1, n, s, (UINT)b);
	o_DrawIndexed(c, n, s, b);
	if (game) PostDraw(c, {1, n, s, b, 0, 0});
}
void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT v, UINT i, UINT sv, UINT si) {
	if (g_replaying) return o_DrawInstanced(c, v, i, sv, si);
	bool game = c == g_ctx;
	if (game && PreDraw(c)) return;
	if (RecOn(c)) RecDraw(2, v, i, (INT)sv, si, 0);
	if (CaptureRecording(c)) CaptureDraw(2, v, i, sv);
	o_DrawInstanced(c, v, i, sv, si);
	if (game) PostDraw(c, {2, v, i, (INT)sv, si, 0});
}
void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si) {
	if (g_replaying) return o_DrawIndexedInstanced(c, n, i, s, b, si);
	bool game = c == g_ctx;
	if (game && PreDraw(c)) return;
	if (RecOn(c)) RecDraw(3, n, i, (INT)s, (UINT)b, si);
	if (CaptureRecording(c)) CaptureDraw(3, n, i, s);
	o_DrawIndexedInstanced(c, n, i, s, b, si);
	if (game) PostDraw(c, {3, n, i, (INT)s, (UINT)b, si});
}

HRESULT STDMETHODCALLTYPE H_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
								D3D11_MAPPED_SUBRESOURCE* m) {
	if (!g_replaying && r && RecOn(c)) RecBeforeMap(r, type);
	HRESULT hr = o_Map(c, r, sub, type, flags, m);
	if (g_replaying) return hr;
	if (SUCCEEDED(hr) && RecOn(c)) RecMap(r, sub, type, flags, m);
	UINT size;
	if (c != g_ctx || FAILED(hr) || !m || !r || type == D3D11_MAP_READ) return hr;
	if (!IsConstantBuffer(r, &size)) return hr;
	if (StereoWatching()) StereoOnMap(r, m->pData, size);
	GameEffectsOnMap(r, m->pData, size);
	UiOnMap(r, m->pData, size);
	if (CaptureRecording(c)) CaptureMap(r, m->pData, size);
	return hr;
}

void STDMETHODCALLTYPE H_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
	if (g_replaying) return o_Unmap(c, r, sub);
	if (RecOn(c) && r) RecUnmap(r, sub);  // the game's own data, before the camera is patched in
	if (c == g_ctx && r) {
		StereoBeforeUnmap(r);  // patch the camera for this frame's eye
		UiBeforeUnmap(r);      // remember UI draw parameters
		GameEffectsBeforeUnmap(r);
	}
	if (CaptureRecording(c) && r) CaptureUnmap(r);
	o_Unmap(c, r, sub);
}

void STDMETHODCALLTYPE H_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
										   const void* data, UINT rp, UINT dp) {
	if (g_replaying) return o_UpdateSubresource(c, r, sub, box, data, rp, dp);
	if (RecOn(c)) RecUpdate(r, sub, box, data, rp, dp);
	UINT size;
	float scratch[64];
	if (c == g_ctx && r && data && !box && IsConstantBuffer(r, &size)) {
		// Constant buffers can only be updated whole, so `data` holds byteWidth bytes.
		if (StereoWatching()) data = StereoPatchUpdate(size, data, scratch);
		data = GameEffectsPatchUpdate(r, data, size, scratch);
		UiOnUpdate(r, data, size);
		if (CaptureRecording(c)) CaptureUpdate(r, data, size);
	}
	o_UpdateSubresource(c, r, sub, box, data, rp, dp);
}

void STDMETHODCALLTYPE H_OMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
								 ID3D11DepthStencilView* dsv) {
	if (g_replaying) return o_OMSetRT(c, n, rtvs, dsv);
	if (RecOn(c)) RecOMSetRT(n, rtvs, dsv);
	if (c == g_ctx) {
		GpuTimingTargets(c, n, rtvs, dsv, "");
		if (dsv) ReprojectNoteDsv(dsv);
	}
	if (CaptureRecording(c)) CaptureSetRenderTargets(n, rtvs, dsv);
	o_OMSetRT(c, n, rtvs, dsv);
}

void STDMETHODCALLTYPE H_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vp) {
	if (g_replaying) return o_RSSetViewports(c, n, vp);
	if (RecOn(c)) RecViewports(n, vp);
	if (CaptureRecording(c)) CaptureViewports(n, vp);
	o_RSSetViewports(c, n, vp);
}

void STDMETHODCALLTYPE H_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
	if (RecOn(c)) RecUnsupported("ExecuteCommandList (deferred rendering)");
	if (CaptureRecording(c)) CaptureExecuteCommandList();
	o_ExecuteCommandList(c, cl, restore);
}

void STDMETHODCALLTYPE H_PSSetSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
	if (g_replaying) return o_PSSetSRV(c, s, n, v);
	if (RecOn(c)) RecSetSRV(RS_PS, s, n, v);
	if (CaptureRecording(c)) CaptureShaderResources(ST_PS, s, n, v);
	o_PSSetSRV(c, s, n, v);
}
void STDMETHODCALLTYPE H_CSSetSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
	if (g_replaying) return o_CSSetSRV(c, s, n, v);
	if (RecOn(c)) RecSetSRV(RS_CS, s, n, v);
	if (CaptureRecording(c)) CaptureShaderResources(ST_CS, s, n, v);
	o_CSSetSRV(c, s, n, v);
}
void STDMETHODCALLTYPE H_CSSetUAV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* v,
								  const UINT* counts) {
	if (g_replaying) return o_CSSetUAV(c, s, n, v, counts);
	if (RecOn(c)) RecCSSetUAV(s, n, v, counts);
	if (CaptureRecording(c)) CaptureUnorderedAccessViews(s, n, v);
	o_CSSetUAV(c, s, n, v, counts);
}

void STDMETHODCALLTYPE H_CSSetShader(ID3D11DeviceContext* c, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
	if (g_replaying) return o_CSSetShader(c, sh, ci, n);
	if (RecOn(c)) RecSetShader(RS_CS, sh, ci, n);
	if (c == g_ctx) g_csClass = ShaderTableClassifyCompute(sh);
	if (CaptureRecording(c)) CaptureSetShader(ST_CS, sh);
	o_CSSetShader(c, sh, ci, n);
}

void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
	if (g_replaying) return o_Dispatch(c, x, y, z);
	if (RecOn(c)) RecDispatch(x, y, z, c == g_ctx ? g_csClass : 0);
	if (c == g_ctx) GpuTimingCompute(c, "", g_csClass);
	if (CaptureRecording(c)) CaptureDispatch(x, y, z);
	bool fix = c == g_ctx && g_csClass != CS_OTHER;
	if (fix) {  // the fix's own calls aren't the game's: not recorded (playback runs the fix again)
		InterlockedIncrement(&g_recSuspend);
		ViewFixBeforeDispatch(c, g_csClass);
		InterlockedDecrement(&g_recSuspend);
	}
	o_Dispatch(c, x, y, z);
	if (fix) {
		InterlockedIncrement(&g_recSuspend);
		ViewFixAfterDispatch(c);
		InterlockedDecrement(&g_recSuspend);
	}
}

HRESULT STDMETHODCALLTYPE H_CreateVS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l, void** out) {
	HRESULT hr = o_CreateVS(d, bc, len, l, out);
	if (d == g_dev && SUCCEEDED(hr) && out) ShaderTableRemember(*out, bc, len, ST_VS);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreatePS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l, void** out) {
	HRESULT hr = o_CreatePS(d, bc, len, l, out);
	if (d == g_dev && SUCCEEDED(hr) && out) ShaderTableRemember(*out, bc, len, ST_PS);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateCS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l, void** out) {
	HRESULT hr = o_CreateCS(d, bc, len, l, out);
	if (d == g_dev && SUCCEEDED(hr) && out) ShaderTableRemember(*out, bc, len, ST_CS);
	return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out) {
	HRESULT hr = o_CreateDeferredContext(d, flags, out);
	if (d == g_dev && SUCCEEDED(hr)) {
		LONG n = InterlockedIncrement(&g_deferredContexts);
		Log("d3d hooks: game created deferred context #%d (multithreaded rendering)", (int)n);
	}
	return hr;
}

bool Hook(void** vtbl, int index, void* detour, void* orig, const char* name) {
	return HookFunction(vtbl[index], detour, (void**)orig, name);
}
}  // namespace

// Frame queue: by default DXGI lets the game's CPU run up to 3 frames ahead of the GPU. The head
// pose is read when the CPU starts a frame, so each queued frame adds a frame time of delay before
// it is drawn. [Render] MaxFrameLatency (1 by default; 0 = leave the game's) is applied to the
// game's device and checked every 2 s in case the game sets its own.
void FrameLatencyTick() {
	static DWORD last;
	static int applied = -1;
	static UINT gameOwn;  // the game's own queue length, seen before we changed it (0 = not yet)
	DWORD t = GetTickCount();
	if (!g_dev || (t - last < 2000 && applied == g_cfg.maxFrameLatency)) return;
	last = t;
	IDXGIDevice1* d = nullptr;
	if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d)) || !d) return;
	UINT cur = 0;
	d->GetMaximumFrameLatency(&cur);
	if (!gameOwn) gameOwn = cur ? cur : 3;
	if (g_cfg.maxFrameLatency == 0 && applied > 0 && cur != gameOwn) {  // back to the game's own
		d->SetMaximumFrameLatency(gameOwn);
		cur = gameOwn;
	}
	if (g_cfg.maxFrameLatency > 0 && cur != (UINT)g_cfg.maxFrameLatency) {
		HRESULT hr = d->SetMaximumFrameLatency((UINT)g_cfg.maxFrameLatency);
		static int logs;
		if (logs++ < 5)
			Log("frame queue: the game's device queued up to %u frames; now %d (0x%08x)%s", cur, g_cfg.maxFrameLatency,
				(unsigned)hr, logs == 5 ? " (not logged again)" : "");
	} else if (applied != g_cfg.maxFrameLatency) {
		Log("frame queue: up to %u frames (MaxFrameLatency=%d%s)", cur, g_cfg.maxFrameLatency,
			g_cfg.maxFrameLatency ? "" : ": the game's own");
	}
	applied = g_cfg.maxFrameLatency;
	d->Release();
}

void D3dHooksInstall(ID3D11Device* dev) {
	static volatile LONG done = 0;
	if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
	if (!g_cfg.probe) Log("frame capture key disabled ([Debug] Probe=0)");
	g_dev = dev;
	dev->GetImmediateContext(&g_ctx);  // keep the reference: lives as long as the game's device
	CaptureSetDevice(g_dev, g_ctx);
	ShaderTableInit();

	void** dv = *(void***)dev;
	void** cv = *(void***)g_ctx;
	int ok = 0;
	ok += Hook(dv, DEV_CreateVertexShader, (void*)&H_CreateVS, &o_CreateVS, "CreateVertexShader");
	ok += Hook(dv, DEV_CreatePixelShader, (void*)&H_CreatePS, &o_CreatePS, "CreatePixelShader");
	ok += Hook(dv, DEV_CreateComputeShader, (void*)&H_CreateCS, &o_CreateCS, "CreateComputeShader");
	ok += Hook(dv, DEV_CreateDeferredContext, (void*)&H_CreateDeferredContext, &o_CreateDeferredContext,
			   "CreateDeferredContext");
	ok += Hook(cv, CTX_VSSetConstantBuffers, (void*)&H_SetCB<ST_VS>, &o_SetCB[ST_VS], "VSSetConstantBuffers");
	ok += Hook(cv, CTX_PSSetConstantBuffers, (void*)&H_SetCB<ST_PS>, &o_SetCB[ST_PS], "PSSetConstantBuffers");
	ok += Hook(cv, CTX_GSSetConstantBuffers, (void*)&H_SetCB<ST_GS>, &o_SetCB[ST_GS], "GSSetConstantBuffers");
	ok += Hook(cv, CTX_CSSetConstantBuffers, (void*)&H_SetCB<ST_CS>, &o_SetCB[ST_CS], "CSSetConstantBuffers");
	ok += Hook(cv, CTX_VSSetShader, (void*)&H_VSSetShader, &o_VSSetShader, "VSSetShader");
	ok += Hook(cv, CTX_PSSetShader, (void*)&H_PSSetShader, &o_PSSetShader, "PSSetShader");
	ok += Hook(cv, CTX_Draw, (void*)&H_Draw, &o_Draw, "Draw");
	ok += Hook(cv, CTX_DrawIndexed, (void*)&H_DrawIndexed, &o_DrawIndexed, "DrawIndexed");
	ok += Hook(cv, CTX_DrawInstanced, (void*)&H_DrawInstanced, &o_DrawInstanced, "DrawInstanced");
	ok += Hook(cv, CTX_DrawIndexedInstanced, (void*)&H_DrawIndexedInstanced, &o_DrawIndexedInstanced,
			   "DrawIndexedInstanced");
	ok += Hook(cv, CTX_Map, (void*)&H_Map, &o_Map, "Map");
	ok += Hook(cv, CTX_Unmap, (void*)&H_Unmap, &o_Unmap, "Unmap");
	ok += Hook(cv, CTX_UpdateSubresource, (void*)&H_UpdateSubresource, &o_UpdateSubresource, "UpdateSubresource");
	ok += Hook(cv, CTX_OMSetRenderTargets, (void*)&H_OMSetRT, &o_OMSetRT, "OMSetRenderTargets");
	ok += Hook(cv, CTX_RSSetViewports, (void*)&H_RSSetViewports, &o_RSSetViewports, "RSSetViewports");
	ok += Hook(cv, CTX_ExecuteCommandList, (void*)&H_ExecuteCommandList, &o_ExecuteCommandList, "ExecuteCommandList");
	ok += Hook(cv, CTX_Dispatch, (void*)&H_Dispatch, &o_Dispatch, "Dispatch");
	ok += Hook(cv, CTX_PSSetShaderResources, (void*)&H_PSSetSRV, &o_PSSetSRV, "PSSetShaderResources");
	ok += Hook(cv, CTX_CSSetShaderResources, (void*)&H_CSSetSRV, &o_CSSetSRV, "CSSetShaderResources");
	ok += Hook(cv, CTX_CSSetUnorderedAccessViews, (void*)&H_CSSetUAV, &o_CSSetUAV, "CSSetUnorderedAccessViews");
	ok += Hook(cv, CTX_CSSetShader, (void*)&H_CSSetShader, &o_CSSetShader, "CSSetShader");
	ReplayInstall(g_ctx);
	Log("D3D11 hooks ready: %d/25 installed (capture key 0x%x)", ok, (unsigned)g_cfg.keyCapture);
}

void D3dHooksFollowDevice(ID3D11Device* presentingDevice) {
	if (!g_dev || presentingDevice == g_dev) return;
	Log("d3d hooks: the presenting device differs from the first created device - following the presenting one");
	if (g_ctx) g_ctx->Release();
	g_dev = presentingDevice;
	g_dev->GetImmediateContext(&g_ctx);
	CaptureSetDevice(g_dev, g_ctx);
}

void OriginalDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) { o_Dispatch(c, x, y, z); }

void OriginalDraw(ID3D11DeviceContext* c, const DrawArgs& d) {
	switch (d.kind) {
		case 0: o_Draw(c, d.a, d.b); break;
		case 1: o_DrawIndexed(c, d.a, d.b, d.c); break;
		case 2: o_DrawInstanced(c, d.a, d.b, (UINT)d.c, d.d); break;
		case 3: o_DrawIndexedInstanced(c, d.a, d.b, (UINT)d.c, (INT)d.d, d.e); break;
	}
}

void OriginalSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
							  ID3D11DepthStencilView* dsv) {
	o_OMSetRT(c, n, rtvs, dsv);
}

const void* PatchConstantBufferForReplay(ID3D11Resource* r, const void* data, UINT size, float* scratch) {
	data = StereoPatchUpdate(size, data, scratch);
	return GameEffectsPatchUpdate(r, data, size, scratch);
}
