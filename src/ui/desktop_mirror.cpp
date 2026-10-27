// desktop_mirror.cpp - the game's window in stereo: the left eye's 3D image filling the window, the game's UI unsqueezed on top, and the mod's menu and stats overlay.
#include "ui/desktop_mirror.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/frame_share.h"
#include "render/replay.h"
#include "ui/menu.h"
#include <d3dcompiler.h>
#include <string.h>

namespace {
constexpr char kHlsl[] = R"(
cbuffer M : register(b0) {
	float4 bg;       // xy: the scene's centre part shown (fraction of it each way)
	float4 ui;       // rects in the window (x0, y0, x1, y1, 0..1): the game's UI
	float4 menu;     // the mod's menu
	float4 overlay;  // the stats overlay
	float4 flags;    // x: menu shown, y: overlay shown, z: the output is sRGB (the panels' colours are converted)
};
Texture2D full : register(t0);     // the frame, UI included
Texture2D scene : register(t1);    // the same without UI; alpha: UI coverage
Texture2D menuT : register(t2);    // premultiplied
Texture2D overT : register(t3);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
	V o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
bool Inside(float2 p, float4 r) { return all(p >= r.xy) && all(p < r.zw); }
float2 In(float2 p, float4 r) { return (p - r.xy) / (r.zw - r.xy); }
float3 ToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float4 Panel(Texture2D t, float2 u) {
	float4 p = t.Sample(smp, u);
	if (flags.z > 0.5) p.rgb = p.a > 0.001 ? ToLinear(saturate(p.rgb / p.a)) * p.a : 0;
	return p;
}
float4 ps(V i) : SV_Target {
	float3 c = scene.Sample(smp, 0.5 + (i.uv - 0.5) * bg.xy).rgb;
	if (Inside(i.uv, ui)) {  // the HUD cut out of the frame as the headset's HUD layer is (xr_device.cpp PSKey)
		float2 u = In(i.uv, ui);
		float3 f = full.Sample(smp, u).rgb;
		float4 s = scene.Sample(smp, u);
		float a = saturate(s.a);
		float3 d = abs(f - s.rgb);
		float3 pm = max(f - (1 - a) * s.rgb, 0) * max(step(0.004, a), smoothstep(0.004, 0.03, max(d.r, max(d.g, d.b))));
		c = pm + (1 - a) * c;
	}
	if (flags.y > 0.5 && Inside(i.uv, overlay)) {
		float4 o = Panel(overT, In(i.uv, overlay));
		c = o.rgb + (1 - o.a) * c;
	}
	if (flags.x > 0.5 && Inside(i.uv, menu)) {
		float4 m = Panel(menuT, In(i.uv, menu));
		c = m.rgb + (1 - m.a) * c;
	}
	return float4(c, 1);
}
)";
constexpr float kMaxHalfAngleTan = 0.70f;  // the window shows at most about 70 degrees up and down: a monitor isn't a headset
constexpr float kOverlayWidth = 0.3f, kMenuHeight = 0.85f, kMargin = 0.02f;

struct Constants {
	float bg[4], ui[4], menu[4], overlay[4], flags[4];
};

ID3D11VertexShader* s_vs;
ID3D11PixelShader* s_ps;
ID3D11Buffer* s_constants;
ID3D11SamplerState* s_sampler;
bool s_ready, s_failed;
ID3D11Texture2D* s_full;  // the frame's copy (the backbuffer can't be read by a shader)
ID3D11ShaderResourceView* s_fullSrv;
ID3D11Texture2D* s_mirror;
ID3D11RenderTargetView* s_mirrorRtv;
D3D11_TEXTURE2D_DESC s_desc;
ID3D11Texture2D* s_sceneTex;  // the scene texture s_sceneSrv views
ID3D11ShaderResourceView* s_sceneSrv;
struct SharedPanel {
	HANDLE handle;
	UINT width, height;
	ID3D11Texture2D* tex;
	ID3D11ShaderResourceView* srv;
} s_menu, s_overlay;
bool s_have;  // s_mirror holds an image
bool s_logged;

bool CreateObjects(ID3D11Device* device) {
	if (s_ready || s_failed) return s_ready;
	HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = compiler ? (pD3DCompile)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
	ComPtr<ID3DBlob> vs, ps, errors;
	HRESULT hr = compile ? compile(kHlsl, sizeof(kHlsl) - 1, "DesktopMirror", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors) : E_FAIL;
	if (SUCCEEDED(hr)) hr = compile(kHlsl, sizeof(kHlsl) - 1, "DesktopMirror", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors);
	if (SUCCEEDED(hr)) hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
	if (SUCCEEDED(hr)) hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_ps);
	D3D11_BUFFER_DESC bd = {sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	if (SUCCEEDED(hr)) hr = device->CreateBuffer(&bd, nullptr, &s_constants);
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sd, &s_sampler);
	if (FAILED(hr)) {
		Log("ERROR: desktop mirror: shaders failed (0x%08x) %s; the window shows the left eye as rendered", (unsigned)hr,
			errors ? (const char*)errors->GetBufferPointer() : "");
		s_failed = true;
		return false;
	}
	s_ready = true;
	return true;
}

void ReleaseTargets() {
	SafeRelease(s_fullSrv);
	SafeRelease(s_full);
	SafeRelease(s_mirrorRtv);
	SafeRelease(s_mirror);
	s_have = false;
}

bool EnsureTargets(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& backbuffer) {
	if (s_mirror && s_desc.Width == backbuffer.Width && s_desc.Height == backbuffer.Height && s_desc.Format == backbuffer.Format) return true;
	ReleaseTargets();
	s_desc = backbuffer;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = backbuffer.Width;
	desc.Height = backbuffer.Height;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = backbuffer.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bool ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &s_full)) && SUCCEEDED(device->CreateShaderResourceView(s_full, nullptr, &s_fullSrv));
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &s_mirror)) && SUCCEEDED(device->CreateRenderTargetView(s_mirror, nullptr, &s_mirrorRtv));
	if (!ok) {
		ReleaseTargets();
		Log("desktop mirror: its textures couldn't be made (format %d); the window shows the left eye as rendered", (int)backbuffer.Format);
	}
	return ok;
}

// The menu or overlay as the VR thread last drew it (null: not drawn yet)
ID3D11ShaderResourceView* OpenPanel(ID3D11Device* device, SharedPanel& panel, const FrameShare::Panel& shared) {
	if (shared.handle != panel.handle) {
		SafeRelease(panel.srv);
		SafeRelease(panel.tex);
		panel.handle = shared.handle;
		if (panel.handle && SUCCEEDED(device->OpenSharedResource(panel.handle, __uuidof(ID3D11Texture2D), (void**)&panel.tex)))
			device->CreateShaderResourceView(panel.tex, nullptr, &panel.srv);
	}
	panel.width = shared.width;
	panel.height = shared.height;
	return panel.srv;
}

bool IsSrgb(DXGI_FORMAT format) { return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

void SetRect(float* r, float x0, float y0, float x1, float y1) {
	r[0] = x0;
	r[1] = y0;
	r[2] = x1;
	r[3] = y1;
}
}  // namespace

void DesktopMirrorCompose(ID3D11DeviceContext* c, ID3D11Texture2D* backbuffer, ID3D11Texture2D* scene, HWND window) {
	float halfW, halfH;
	if (!scene || !StereoEyeTangents(&halfW, &halfH)) return;
	ComPtr<ID3D11Device> device;
	c->GetDevice(&device);
	D3D11_TEXTURE2D_DESC desc;
	backbuffer->GetDesc(&desc);
	if (!device || desc.SampleDesc.Count != 1 || !CreateObjects(device.Get()) || !EnsureTargets(device.Get(), desc)) return;
	if (scene != s_sceneTex) {
		SafeRelease(s_sceneSrv);
		s_sceneTex = scene;
		if (FAILED(device->CreateShaderResourceView(scene, nullptr, &s_sceneSrv))) s_sceneSrv = nullptr;
	}
	if (!s_sceneSrv) return;
	FrameShare::Panel menuShared, overlayShared;
	EnterCriticalSection(&g_share.lock);
	menuShared = g_share.menuPanel;
	overlayShared = g_share.overlayPanel;
	LeaveCriticalSection(&g_share.lock);
	ID3D11ShaderResourceView* menuSrv = OpenPanel(device.Get(), s_menu, menuShared);
	ID3D11ShaderResourceView* overlaySrv = OpenPanel(device.Get(), s_overlay, overlayShared);

	// the window's shape: the backbuffer is stretched to it
	RECT client = {};
	float windowAspect = window && GetClientRect(window, &client) && client.bottom > 0 ? (float)client.right / (float)client.bottom
																					   : (float)desc.Width / (float)desc.Height;
	Constants k = {};
	// the 3D image: its middle, filling the window, at most kMaxHalfAngleTan up and down
	float showH = halfH < kMaxHalfAngleTan ? halfH : kMaxHalfAngleTan, showW = showH * windowAspect;
	if (showW > halfW) {
		showW = halfW;
		showH = showW / windowAspect;
	}
	k.bg[0] = showW / halfW;
	k.bg[1] = showH / halfH;
	// the game's UI: laid out for a box of at least kGameMinAspect, the largest that fits, centred
	float ui = (float)desc.Width / (float)desc.Height;
	if (ui < kGameMinAspect) ui = kGameMinAspect;
	float uiW = ui < windowAspect ? ui / windowAspect : 1, uiH = ui < windowAspect ? 1 : windowAspect / ui;
	SetRect(k.ui, 0.5f - uiW / 2, 0.5f - uiH / 2, 0.5f + uiW / 2, 0.5f + uiH / 2);
	if (overlaySrv && overlayShared.height) {
		float h = kOverlayWidth * windowAspect * (float)overlayShared.height / (float)overlayShared.width;
		SetRect(k.overlay, kMargin, kMargin * windowAspect, kMargin + kOverlayWidth, kMargin * windowAspect + h);
		k.flags[1] = g_cfg.statsOverlay ? 1.0f : 0.0f;
	}
	if (menuSrv && menuShared.height) {
		float h = kMenuHeight, w = h * (float)menuShared.width / (float)menuShared.height / windowAspect;
		if (w > 1 - 2 * kMargin) {
			h *= (1 - 2 * kMargin) / w;
			w = 1 - 2 * kMargin;
		}
		SetRect(k.menu, 0.5f - w / 2, 0.5f - h / 2, 0.5f + w / 2, 0.5f + h / 2);
		k.flags[0] = MenuIsOpen() ? 1.0f : 0.0f;
	}
	k.flags[2] = IsSrgb(desc.Format) ? 1.0f : 0.0f;

	g_replaying = true;  // our own calls: the hooks pass them straight through
	StateSaveTemp(c);
	c->CopyResource(s_full, backbuffer);
	c->UpdateSubresource(s_constants, 0, nullptr, &k, 0, 0);
	D3D11_VIEWPORT viewport = {0, 0, (float)desc.Width, (float)desc.Height, 0, 1};
	c->OMSetRenderTargets(1, &s_mirrorRtv, nullptr);
	c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	c->OMSetDepthStencilState(nullptr, 0);
	c->RSSetState(nullptr);
	c->RSSetViewports(1, &viewport);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(s_vs, nullptr, 0);
	c->PSSetShader(s_ps, nullptr, 0);
	ID3D11ShaderResourceView* srvs[4] = {s_fullSrv, s_sceneSrv, menuSrv, overlaySrv};
	c->PSSetShaderResources(0, 4, srvs);
	c->PSSetSamplers(0, 1, &s_sampler);
	c->PSSetConstantBuffers(0, 1, &s_constants);
	c->Draw(3, 0);
	ID3D11ShaderResourceView* nulls[4] = {};
	c->PSSetShaderResources(0, 4, nulls);
	StateRestoreTemp(c);
	g_replaying = false;
	s_have = true;
	if (!s_logged) {
		s_logged = true;
		Log("desktop mirror: the window shows the left eye (%.0f x %.0f deg of it), the game's UI at %.2f:1 and the mod's panels",
			(double)(2 * pure::fatan(showW) * 57.29578f), (double)(2 * pure::fatan(showH) * 57.29578f), (double)ui);
	}
}

bool DesktopMirrorShow(ID3D11DeviceContext* c, ID3D11Texture2D* backbuffer) {
	if (!s_have) return false;
	D3D11_TEXTURE2D_DESC desc;
	backbuffer->GetDesc(&desc);
	if (desc.Width != s_desc.Width || desc.Height != s_desc.Height || desc.Format != s_desc.Format) {
		s_have = false;
		return false;
	}
	g_replaying = true;
	c->CopyResource(backbuffer, s_mirror);
	g_replaying = false;
	return true;
}
