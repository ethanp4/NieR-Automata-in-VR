// xr_device.cpp - the VR thread's own D3D11 device (on the game's GPU) and its blit shaders, the
// game's frames copied onto it, and menu detection.
#include "xr/xr_thread.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include <string.h>
#include <d3dcompiler.h>

// ---------------------------------------------------------------------------------------------
// Our D3D11 device and the blit shader
// ---------------------------------------------------------------------------------------------
static const char kShader[] = R"(
cbuffer Params : register(b0) { float4 params; };   // params.x: 0 = copy, 1 = sRGB->linear, 2 = linear->sRGB
Texture2D src : register(t0);
Texture2D src2 : register(t1);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float3 ToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float3 ToSrgb(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308)); }
float3 Convert(float3 c) {
	if (params.x > 1.5) return ToSrgb(c);
	if (params.x > 0.5) return ToLinear(c);
	return c;
}
float4 PS(VSOut i) : SV_Target {
	return float4(Convert(src.Sample(samp, i.uv).rgb), 1);
}
// Late warp: the eye image re-aimed to the headset's pose for this frame, covering exactly
// the eye's own field of view. Output pixel -> direction in the shown eye (outTan: tangents of left,
// right, up, down) -> rotated into the eye the image was rendered from (m0..m2 = R_render^T R_shown)
// -> its symmetric frustum (srcTan: tangent half-width, half-height). Rotation is exact; the small
// eye translation between the two poses is ignored, as a compositor's rotational reprojection does.
cbuffer Warp : register(b1) { float4 outTan; float4 srcTan; float4 m0; float4 m1; float4 m2; };
float4 PSWarp(VSOut i) : SV_Target {
	float3 d = float3(lerp(outTan.x, outTan.y, i.uv.x), lerp(outTan.z, outTan.w, i.uv.y), -1);
	float3 r = float3(dot(m0.xyz, d), dot(m1.xyz, d), dot(m2.xyz, d));
	if (r.z > -1e-4) return float4(0, 0, 0, 1);
	float2 t = r.xy / -r.z;
	float2 uv = float2(t.x / srcTan.x * 0.5 + 0.5, 0.5 - t.y / srcTan.y * 0.5);
	if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
	return float4(Convert(src.SampleLevel(samp, uv, 0).rgb), 1);
}
// HUD/menus. src = full frame, src2 = scene without UI, whose alpha channel holds the UI's real
// coverage (accumulated on the game side from the UI draws' own alpha). Since
// full = ui + (1 - a) * scene, the HUD layer is ui = full - (1 - a) * scene, premultiplied:
// translucent panels darken the 3D scene instead of carrying a flat copy of it.
// params.y/z: where nothing was drawn (a = 0), differences below y are noise, above z kept
// (additive glows).
float4 PSKey(VSOut i) : SV_Target {
	float3 f = src.Sample(samp, i.uv).rgb;
	float4 s = src2.Sample(samp, i.uv);
	float a = saturate(s.a);
	float3 d = abs(f - s.rgb);
	float3 pm = max(f - (1 - a) * s.rgb, 0);
	pm *= max(step(0.004, a), smoothstep(params.y, params.z, max(d.r, max(d.g, d.b))));
	float3 c = a > 0.01 ? Convert(saturate(pm / a)) * a : Convert(pm);
	return float4(c, a);
}
)";

static bool CompileShaders() {
	HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
	if (!compile) {
		Log("ERROR: d3dcompiler_47.dll not available (error %u)", (unsigned)GetLastError());
		return false;
	}
	ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
	HRESULT hr = compile(kShader, sizeof(kShader) - 1, "niervr_blit", nullptr, nullptr, "VS", "vs_4_0",
						 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
	if (FAILED(hr)) {
		Log("ERROR: VS compile 0x%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
		SafeRelease(err);
		return false;
	}
	hr = compile(kShader, sizeof(kShader) - 1, "niervr_blit", nullptr, nullptr, "PS", "ps_4_0",
				 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
	if (FAILED(hr)) {
		Log("ERROR: PS compile 0x%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
		SafeRelease(err);
		SafeRelease(vsb);
		return false;
	}
	hr = g_vr.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vr.vs);
	if (SUCCEEDED(hr)) hr = g_vr.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_vr.ps);
	SafeRelease(vsb);
	SafeRelease(psb);
	if (SUCCEEDED(hr)) {
		hr = compile(kShader, sizeof(kShader) - 1, "niervr_key", nullptr, nullptr, "PSKey", "ps_4_0",
					 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
		if (FAILED(hr)) Log("ERROR: PSKey compile 0x%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
		SafeRelease(err);
		if (SUCCEEDED(hr)) hr = g_vr.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_vr.psKey);
		SafeRelease(psb);
	}
	if (SUCCEEDED(hr)) {  // optional: without it the eyes are submitted as rendered (LateWarp off)
		HRESULT he = compile(kShader, sizeof(kShader) - 1, "niervr_warp", nullptr, nullptr, "PSWarp", "ps_4_0",
							 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
		if (FAILED(he)) Log("late warp: shader compile 0x%08x: %s", (unsigned)he, err ? (const char*)err->GetBufferPointer() : "");
		SafeRelease(err);
		if (SUCCEEDED(he)) g_vr.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_vr.psWarp);
		SafeRelease(psb);
		D3D11_BUFFER_DESC wb = {};
		wb.ByteWidth = 80;
		wb.Usage = D3D11_USAGE_DEFAULT;
		wb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (g_vr.psWarp && FAILED(g_vr.dev->CreateBuffer(&wb, nullptr, &g_vr.cbWarp))) SafeRelease(g_vr.psWarp);
	}
	if (FAILED(hr)) {
		Log("ERROR: CreateShader 0x%08x", (unsigned)hr);
		return false;
	}
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	hr = g_vr.dev->CreateSamplerState(&sd, &g_vr.sampler);
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr)) hr = g_vr.dev->CreateBuffer(&bd, nullptr, &g_vr.cbuf);
	if (FAILED(hr)) Log("ERROR: sampler/cbuffer 0x%08x", (unsigned)hr);
	return SUCCEEDED(hr);
}

// Created on the game's GPU as soon as the VR thread starts, before OpenXR is involved, because it
// also owns the texture the game copies its frames into.
bool InitDevice() {
	// Use the system d3d11.dll directly so a chain-loaded mod never wraps our private device.
	wchar_t path[MAX_PATH];
	UINT n = GetSystemDirectoryW(path, MAX_PATH - 16);
	lstrcpyW(path + n, L"\\d3d11.dll");
	HMODULE d3d11 = LoadLibraryW(path);
	PFN_D3D11_CREATE_DEVICE createDevice =
		d3d11 ? (PFN_D3D11_CREATE_DEVICE)(void*)GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
	PFN_CreateDXGIFactory1 createFactory =
		dxgi ? (PFN_CreateDXGIFactory1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
	if (!createDevice || !createFactory) {
		Log("ERROR: cannot load D3D11/DXGI entry points");
		return false;
	}
	IDXGIFactory1* factory = nullptr;
	if (FAILED(createFactory(__uuidof(IDXGIFactory1), (void**)&factory))) {
		Log("ERROR: CreateDXGIFactory1 failed");
		return false;
	}
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d;
		bool match = SUCCEEDED(adapter->GetDesc1(&d)) &&
					 (!g_share.gameLuidValid || (d.AdapterLuid.LowPart == g_share.gameLuid.LowPart &&
												 d.AdapterLuid.HighPart == g_share.gameLuid.HighPart));
		if (match) {
			Log("VR device on GPU: %s", Utf8(d.Description).s);
			g_vr.devLuid = d.AdapterLuid;
			break;
		}
		SafeRelease(adapter);
	}
	factory->Release();
	if (!adapter) {
		Log("ERROR: could not find the game's GPU for the VR device");
		return false;
	}
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
										D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL got;
	HRESULT hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
							  D3D11_SDK_VERSION, &g_vr.dev, &got, &g_vr.ctx);
	if (hr == E_INVALIDARG)  // Windows without 11.1 support
		hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
						  D3D11_SDK_VERSION, &g_vr.dev, &got, &g_vr.ctx);
	adapter->Release();
	if (FAILED(hr)) {
		Log("ERROR: VR D3D11CreateDevice failed 0x%08x", (unsigned)hr);
		return false;
	}
	g_vr.devLevel = got;
	Log("VR device created (feature level 0x%x)", (unsigned)got);
	// 0.5.1: the VR thread's few small GPU jobs per headset frame must not wait behind the game's
	// frame on the same GPU, or the runtime sees late frames and throttles the app (0.5.0 Pico log:
	// headset 27-32 fps in synchronized mode while the GPU needed only 15-19 ms per game frame)
	{
		IDXGIDevice* xd = nullptr;
		if (SUCCEEDED(g_vr.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && xd) {
			HRESULT ph = E_FAIL;
			int prio = 7;
			for (; prio > 0 && FAILED(ph = xd->SetGPUThreadPriority(prio)); prio -= 3) {}
			Log("VR device GPU priority %d%s (0x%08x)", SUCCEEDED(ph) ? prio : 0,
				SUCCEEDED(ph) ? " (above the game)" : ": not allowed, normal", (unsigned)ph);
			xd->Release();
		}
	}
	return CompileShaders();
}

// ---------------------------------------------------------------------------------------------
// Game frame -> our device
// ---------------------------------------------------------------------------------------------
static void ReleaseContent() {
	for (auto& r : g_vr.ring) SafeRelease(r);
	for (auto& r : g_vr.ringScene) SafeRelease(r);
	for (auto& r : g_vr.ringKey) SafeRelease(r);
	for (auto& v : g_vr.views) {
		SafeRelease(v.srv);
		SafeRelease(v.tex);
		SafeRelease(v.sceneSrv);
		SafeRelease(v.keySrv);
		SafeRelease(v.keyTex);
		SafeRelease(v.sceneTex);
		v.has = false;
		v.seq = 0;
	}
}

static void TypelessFor(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* view, bool* linear) {
	*linear = false;
	switch (f) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			*tex = DXGI_FORMAT_R8G8B8A8_TYPELESS;
			*view = DXGI_FORMAT_R8G8B8A8_UNORM;
			return;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			*tex = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			*view = DXGI_FORMAT_B8G8R8A8_UNORM;
			return;
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			*tex = DXGI_FORMAT_B8G8R8X8_TYPELESS;
			*view = DXGI_FORMAT_B8G8R8X8_UNORM;
			return;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			*tex = DXGI_FORMAT_R10G10B10A2_TYPELESS;
			*view = DXGI_FORMAT_R10G10B10A2_UNORM;
			return;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			*tex = DXGI_FORMAT_R16G16B16A16_TYPELESS;
			*view = DXGI_FORMAT_R16G16B16A16_FLOAT;
			*linear = true;  // scRGB
			return;
		default:
			*tex = *view = f;
			return;
	}
}

// Answers the game thread's request for capture textures: a ring of kRingSize created here, on our
// device, and shared with the game's device by handle. Called continuously from the VR loop.
void ServiceShare() {
	EnterCriticalSection(&g_share.cs);
	LONG reqGen = g_share.reqGen;
	UINT w = g_share.reqW, h = g_share.reqH;
	UINT sw = g_share.reqSceneW ? g_share.reqSceneW : w, sh = g_share.reqSceneH ? g_share.reqSceneH : h;
	bool key = g_share.reqKey;
	DXGI_FORMAT fmt = g_share.reqFmt;
	LeaveCriticalSection(&g_share.cs);
	if (reqGen == g_vr.servedReqGen || w == 0 || h == 0) return;
	g_vr.servedReqGen = reqGen;  // one attempt per request

	ReleaseContent();
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	// full frames, scene images, and (upscaling) the scene images before upscaling: the HUD is cut out
	// as full - scene, which only works with two images that match pixel for pixel where there's no HUD
	// (an upscaled one left faint outlines of everything on the HUD layer)
	const bool up = key || sw != w || sh != h;  // upscaling (DLAA keeps the size)
	HANDLE handles[3 * kRingSize] = {};
	for (int i = 0; i < (up ? 3 : 2) * kRingSize; i++) {
		ID3D11Texture2D** t = i < kRingSize ? &g_vr.ring[i] : i < 2 * kRingSize ? &g_vr.ringScene[i - kRingSize]
																				: &g_vr.ringKey[i - 2 * kRingSize];
		bool big = i >= kRingSize && i < 2 * kRingSize;
		d.Width = big ? sw : w;
		d.Height = big ? sh : h;
		HRESULT hr = g_vr.dev->CreateTexture2D(&d, nullptr, t);
		IDXGIResource* res = nullptr;
		if (SUCCEEDED(hr)) hr = (*t)->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
		if (SUCCEEDED(hr)) {
			hr = res->GetSharedHandle(&handles[i]);
			res->Release();
		}
		if (FAILED(hr) || !handles[i]) {
			Log("ERROR: capture texture %ux%u format %d could not be created/shared (0x%08x)", d.Width, d.Height,
				(int)fmt, (unsigned)hr);
			ReleaseContent();
			return;
		}
	}

	// Local copies we sample from (left, right, mono), so the ring can be reused.
	DXGI_FORMAT texFmt, viewFmt;
	TypelessFor(fmt, &texFmt, &viewFmt, &g_vr.srcLinear);
	D3D11_TEXTURE2D_DESC ld = d;
	ld.Width = w;
	ld.Height = h;
	ld.Format = texFmt;
	ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ld.MiscFlags = 0;
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = viewFmt;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	D3D11_TEXTURE2D_DESC lsd = ld;
	lsd.Width = sw;
	lsd.Height = sh;
	for (auto& v : g_vr.views) {
		HRESULT hr = g_vr.dev->CreateTexture2D(&ld, nullptr, &v.tex);
		if (SUCCEEDED(hr)) hr = g_vr.dev->CreateShaderResourceView(v.tex, &sd, &v.srv);
		if (SUCCEEDED(hr)) hr = g_vr.dev->CreateTexture2D(&lsd, nullptr, &v.sceneTex);
		if (SUCCEEDED(hr)) hr = g_vr.dev->CreateShaderResourceView(v.sceneTex, &sd, &v.sceneSrv);
		if (SUCCEEDED(hr) && up) hr = g_vr.dev->CreateTexture2D(&ld, nullptr, &v.keyTex);
		if (SUCCEEDED(hr) && up) hr = g_vr.dev->CreateShaderResourceView(v.keyTex, &sd, &v.keySrv);
		if (FAILED(hr)) {
			Log("ERROR: VR-side copy texture %ux%u fmt %d failed 0x%08x", w, h, (int)texFmt, (unsigned)hr);
			ReleaseContent();
			return;
		}
	}
	g_vr.contentW = w;
	g_vr.contentH = h;
	g_vr.sceneW = sw;
	g_vr.sceneH = sh;

	EnterCriticalSection(&g_share.cs);
	memcpy(g_share.ring, handles, sizeof(g_share.ring));
	memcpy(g_share.ringScene, handles + kRingSize, sizeof(g_share.ringScene));
	memcpy(g_share.ringKey, handles + 2 * kRingSize, sizeof(g_share.ringKey));  // null when not upscaling
	g_share.w = w;
	g_share.h = h;
	g_share.sceneW = sw;
	g_share.sceneH = sh;
	g_share.fmt = fmt;
	g_share.forReqGen = reqGen;
	g_share.gen = ++g_vr.publishGen;
	for (auto& l : g_share.latest) l.seq = 0;
	LeaveCriticalSection(&g_share.cs);
	Log("capture ring published: %d x %ux%u format %d%s, scene images %ux%u", kRingSize, w, h, (int)fmt,
		g_vr.srcLinear ? " (linear/HDR)" : "", sw, sh);
}

static void SetMenuMode(bool on, float frac);

// The scene image the HUD is cut out against: the one before upscaling when there is one.
ID3D11ShaderResourceView* KeySrv(const GameView& v) {
	return v.keySrv ? v.keySrv : v.sceneSrv;
}

// Copies newly finished game frames (left / right / mono) into our local textures.
void UpdateContent() {
	if (!g_vr.ring[0]) return;
	FrameShare::Published latest[3];
	EnterCriticalSection(&g_share.cs);
	bool current = g_share.gen == g_vr.publishGen;
	memcpy(latest, g_share.latest, sizeof(latest));
	LeaveCriticalSection(&g_share.cs);
	if (!current) return;
	for (int k = 0; k < 3; k++) {
		if (!latest[k].seq || latest[k].seq == g_vr.views[k].seq || latest[k].slot < 0 || latest[k].slot >= kRingSize) continue;  // 0.3.27 still said > 3: half the frames lost
		g_vr.ctx->CopyResource(g_vr.views[k].tex, g_vr.ring[latest[k].slot]);
		if (latest[k].tag.hasScene) g_vr.ctx->CopyResource(g_vr.views[k].sceneTex, g_vr.ringScene[latest[k].slot]);
		if (latest[k].tag.hasScene && !latest[k].tag.noUI && g_vr.views[k].keyTex && g_vr.ringKey[latest[k].slot])
			g_vr.ctx->CopyResource(g_vr.views[k].keyTex, g_vr.ringKey[latest[k].slot]);
		// Eye frames without a scene-only image (the UI split didn't work on this screen, e.g.
		// during some damage effects): stay in stereo with the HUD inside the 3D image. (0.3.4-0.3.11
		// switched to the flat screen here, which made the view flicker to flat when hit; the title
		// screen that needed it is handled by FlatBelowDraws now.)
		if (k < 2) {
			if (latest[k].tag.hasScene) {
				g_vr.noSceneRun = 0;
			} else if (++g_vr.noSceneRun == 30) {
				static volatile LONG budget;
				if (LogBudget(&budget, 3)) Log("ui split: no HUD-free image for a moment; HUD shown inside the 3D view");
			}
		}
		g_vr.statSceneFrames += latest[k].tag.hasScene ? 1 : 0;
		g_vr.views[k].seq = latest[k].seq;
		g_vr.views[k].tag = latest[k].tag;
		g_vr.views[k].has = true;
		g_vr.statGameFrames[k]++;
		g_vr.ovGameFrames[k]++;
	}
}

void Blit(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11ShaderResourceView* srv,
		  ID3D11ShaderResourceView* scene, bool dstSrgb, const WarpParams* warp) {
	int mode = (!g_vr.srcLinear && dstSrgb) ? 1 : ((g_vr.srcLinear && !dstSrgb) ? 2 : 0);
	// key thresholds on the difference between full and scene-only frames (0..1 colour units)
	float p[4] = {(float)mode, 0.004f, 0.03f, 0};
	static float last[4] = {-1, -1, -1, -1};
	if (memcmp(p, last, sizeof(p)) != 0) {
		g_vr.ctx->UpdateSubresource(g_vr.cbuf, 0, nullptr, p, 0, 0);
		memcpy(last, p, sizeof(p));
	}
	D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
	g_vr.ctx->OMSetRenderTargets(1, &rtv, nullptr);
	g_vr.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	g_vr.ctx->OMSetDepthStencilState(nullptr, 0);
	g_vr.ctx->RSSetState(nullptr);
	g_vr.ctx->RSSetViewports(1, &vp);
	g_vr.ctx->IASetInputLayout(nullptr);
	g_vr.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_vr.ctx->VSSetShader(g_vr.vs, nullptr, 0);
	if (warp) {
		static_assert(sizeof(WarpParams) == 80, "matches the Warp cbuffer");
		g_vr.ctx->UpdateSubresource(g_vr.cbWarp, 0, nullptr, warp, 0, 0);
		g_vr.ctx->PSSetConstantBuffers(1, 1, &g_vr.cbWarp);
	}
	g_vr.ctx->PSSetShader(warp ? g_vr.psWarp : (scene ? g_vr.psKey : g_vr.ps), nullptr, 0);
	ID3D11ShaderResourceView* srvs[2] = {srv, scene};
	g_vr.ctx->PSSetShaderResources(0, 2, srvs);
	g_vr.ctx->PSSetSamplers(0, 1, &g_vr.sampler);
	g_vr.ctx->PSSetConstantBuffers(0, 1, &g_vr.cbuf);
	g_vr.ctx->Draw(3, 0);
	ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
	g_vr.ctx->PSSetShaderResources(0, 2, nulls);
	g_vr.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Menu detection: how much of the screen is UI? Rendered at 64x36 and read back a few frames
// later without blocking. Mostly-UI frames (pause menu, map, full-screen menus) switch the game
// to flat rendering until the UI is mostly gone again.
// ---------------------------------------------------------------------------------------------
static const int kCovW = 64, kCovH = 36;

static bool EnsureCoverageTargets() {
	if (g_vr.covTex) return true;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = kCovW;
	d.Height = kCovH;
	d.MipLevels = d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = g_vr.dev->CreateTexture2D(&d, nullptr, &g_vr.covTex);
	if (SUCCEEDED(hr)) hr = g_vr.dev->CreateRenderTargetView(g_vr.covTex, nullptr, &g_vr.covRtv);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (int i = 0; i < 3 && SUCCEEDED(hr); i++) hr = g_vr.dev->CreateTexture2D(&d, nullptr, &g_vr.covStaging[i]);
	if (FAILED(hr)) {
		Log("ERROR: UI coverage targets failed 0x%08x", (unsigned)hr);
		SafeRelease(g_vr.covRtv);
		SafeRelease(g_vr.covTex);
		for (auto& t : g_vr.covStaging) SafeRelease(t);
		return false;
	}
	return true;
}

static void SetMenuMode(bool on, float frac) {
	if (on == g_vr.menuMode) return;
	g_vr.menuMode = on;
	AcquireSRWLockExclusive(&g_stereo.lock);
	g_stereo.menuMode = on;
	ReleaseSRWLockExclusive(&g_stereo.lock);
	if (frac >= 0)
		Log("%s (UI covers %.0f%% of the screen)", on ? "menu detected: flat screen" : "menu closed: back to stereo",
			(double)(frac * 100));
}

void MeasureUICoverage() {
	if (!g_vr.psKey || !EnsureCoverageTargets()) return;
	for (int i = 0; i < 3; i++) {
		if (!g_vr.covPending[i]) continue;
		D3D11_MAPPED_SUBRESOURCE m;
		if (g_vr.ctx->Map(g_vr.covStaging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
		int n = 0;
		for (int y = 0; y < kCovH; y++) {
			const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
			for (int x = 0; x < kCovW; x++) n += row[x * 4 + 3] > 127;
		}
		g_vr.ctx->Unmap(g_vr.covStaging[i], 0);
		g_vr.covPending[i] = false;
		float frac = (float)n / (kCovW * kCovH);
		g_vr.statCovSum += frac;
		g_vr.statCovN++;
		if (frac > g_vr.statCovMax) g_vr.statCovMax = frac;
		// Coverage is now the UI's real opacity (> 50% per sample point), not "differs from the
		// scene", so these thresholds are lower than 0.3.2's 50% / 30%.
		if (frac > 0.30f)
			g_vr.menuVotes = g_vr.menuVotes < 0 ? 1 : g_vr.menuVotes + 1;
		else if (frac < 0.15f)
			g_vr.menuVotes = g_vr.menuVotes > 0 ? -1 : g_vr.menuVotes - 1;
		// Into the flat menu only after ~0.75 s of mostly-UI frames (full-screen damage flashes
		// are shorter), out again quickly.
		if (!g_vr.menuMode && g_vr.menuVotes >= 45) SetMenuMode(true, frac);
		if (g_vr.menuMode && g_vr.menuVotes <= -4) SetMenuMode(false, frac);
	}
	// measure the newest frame that has a scene-only image
	int k = -1;
	for (int i = 0; i < 3; i++)
		if (g_vr.views[i].has && g_vr.views[i].tag.hasScene && !g_vr.views[i].tag.noUI && (k < 0 || g_vr.views[i].seq > g_vr.views[k].seq))
			k = i;  // (images drawn without UI have none to measure)
	if (k < 0 || g_vr.views[k].seq == g_vr.covMeasuredSeq || g_vr.covPending[g_vr.covNext]) return;
	Blit(g_vr.covRtv, kCovW, kCovH, g_vr.views[k].srv, KeySrv(g_vr.views[k]), false);
	g_vr.ctx->CopyResource(g_vr.covStaging[g_vr.covNext], g_vr.covTex);
	g_vr.covPending[g_vr.covNext] = true;
	g_vr.covNext = (g_vr.covNext + 1) % 3;
	g_vr.covMeasuredSeq = g_vr.views[k].seq;
}
