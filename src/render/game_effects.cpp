// game_effects.cpp - switches off NieR effects that don't suit VR: vignette, shadows, ambient occlusion, low-health filter.
#include "render/game_effects.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include <stdio.h>
#include <string.h>
#include <d3dcompiler.h>

namespace {

constexpr UINT kVignetteBufferSize = 48;
ID3D11Resource* s_vignetteBuffer;
void* s_vignetteMapped;

void PatchVignette(float* floats) {
	if (!g_cfg.vignette) floats[8] = 0.0001f;
}

void FindVignetteBuffer(ID3D11DeviceContext* c) {
	ComPtr<ID3D11Buffer> buffer;
	c->PSGetConstantBuffers(1, 1, &buffer);
	if (!buffer) return;
	D3D11_BUFFER_DESC desc;
	buffer->GetDesc(&desc);
	if (desc.ByteWidth == kVignetteBufferSize) {
		s_vignetteBuffer = buffer.Get();
		Log("vignette: parameters found (buffer of %u bytes); %s", desc.ByteWidth,
			g_cfg.vignette ? "kept (Vignette=1)" : "removed from the next frame on");
	}
}

// --- shadows -----------------------------------------------------------------------------------
// Shadows off: the game's shadow-mask draw runs with a pixel shader that just writes 1 ("lit")
// instead of sampling the shadow map, keeping the game's own blend state, write mask, stencil and
// render targets.
ID3D11PixelShader* s_litShader;
bool s_litShaderTried;
ID3D11PixelShader* s_savedShader;
ID3D11ClassInstance* s_savedInstances[8];
UINT s_savedInstanceCount;
bool s_restoreShader;

ID3D11PixelShader* LitShader(ID3D11DeviceContext* c) {
	if (s_litShaderTried) return s_litShader;
	s_litShaderTried = true;
	HMODULE compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = compilerDll ? (pD3DCompile)(void*)GetProcAddress(compilerDll, "D3DCompile") : nullptr;
	static const char source[] = "float4 main() : SV_Target { return 1; }";  // no inputs: links to any VS
	ID3DBlob *blob = nullptr, *errors = nullptr;
	HRESULT hr = compile ? compile(source, sizeof(source) - 1, "lit", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors) : E_FAIL;
	ComPtr<ID3D11Device> device;
	c->GetDevice(&device);
	if (SUCCEEDED(hr) && device) hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_litShader);
	if (FAILED(hr)) {
		s_litShader = nullptr;
		Log("shadows off: no replacement shader (0x%08x); the shadow pass stays on", (unsigned)hr);
	}
	if (blob) blob->Release();
	if (errors) errors->Release();
	return s_litShader;
}

// --- low-health filter -------------------------------------------------------------------------
// NieR's effect system draws the low-health grey, blur and ripples like particles: quads whose
// texture is a copy of the 3D scene made in the middle of the effect pass, drawn back over the whole
// view. Normal frames only let effects sample one full-size scene copy, the refraction copy bound at
// t10 when the first effect draws. In stereo, effect draws that sample any other full-size copy AND
// use one of the known low-health shaders are skipped. Other shaders that read a scene copy (Pod
// 042's shots, flight-unit effects) are kept and only logged.
ID3D11Resource* s_refractionCopy;  // this frame's normal screen copy (t10 at the first effect draw)
bool s_sawEffect;
uint32_t s_filterSkips;
DWORD s_filterReportTick;
bool s_filterLogged;

constexpr uint32_t kLowHealthShaders[] = {0x86575858, 0x833497f9, 0xcebc6f6d, 0x8b838343, 0x32c5d826};
uint32_t s_keptLogged[16];
int32_t s_keptCount;

bool LowHealthShader(uint64_t hash) {
	for (uint32_t known : kLowHealthShaders)
		if ((uint32_t)hash == known) return true;
	return false;
}

// Which effect shaders were skipped, for the minute report (bytecode hash, count).
struct Skipped {
	uint64_t hash;
	uint32_t count;
	UINT slot;
};
Skipped s_skipped[8];

void NoteSkip(uint64_t hash, UINT slot) {
	for (auto& entry : s_skipped)
		if (entry.hash == hash && entry.count) {
			entry.count++;
			return;
		}
	for (auto& entry : s_skipped)
		if (!entry.count) {
			entry = {hash, 1, slot};
			return;
		}
}

ID3D11Resource* BoundResource(ID3D11DeviceContext* c, UINT slot) {
	ComPtr<ID3D11ShaderResourceView> view;
	c->PSGetShaderResources(slot, 1, &view);
	if (!view) return nullptr;
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	return res.Get();
}

bool Tex2D(ID3D11Resource* res, D3D11_TEXTURE2D_DESC* desc) {
	if (!res) return false;
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
	((ID3D11Texture2D*)res)->GetDesc(desc);
	return true;
}
}  // namespace

void GameEffectsBeginFrame() {
	s_refractionCopy = nullptr;
	s_sawEffect = false;
}

bool GameEffectsBeforeDraw(ID3D11DeviceContext* c, PixelShaderClass ps) {
	if (ps == PS_TONE_MAPPING && !s_vignetteBuffer) FindVignetteBuffer(c);
	if (ps == PS_SHADOW_MASK && !g_cfg.shadows && LitShader(c)) {
		// swap the pixel shader for this draw (through the hooked call, so synchronized mode records it too)
		s_savedInstanceCount = 8;
		c->PSGetShader(&s_savedShader, s_savedInstances, &s_savedInstanceCount);
		c->PSSetShader(s_litShader, nullptr, 0);
		s_restoreShader = true;
		return false;
	}
	// ambient occlusion off: instead of the SAO pass, its output is cleared to 1 (no occlusion)
	if (ps != PS_AMBIENT_OCCLUSION || g_cfg.ambientOcclusion) return false;
	ComPtr<ID3D11RenderTargetView> rtv;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (rtv) {
		const FLOAT one[4] = {1, 1, 1, 1};
		c->ClearRenderTargetView(rtv.Get(), one);
	}
	return true;
}

void GameEffectsAfterDraw(ID3D11DeviceContext* c) {
	if (!s_restoreShader) return;
	s_restoreShader = false;
	c->PSSetShader(s_savedShader, s_savedInstanceCount ? s_savedInstances : nullptr, s_savedInstanceCount);
	if (s_savedShader) s_savedShader->Release();
	for (UINT i = 0; i < s_savedInstanceCount && i < 8; i++)
		if (s_savedInstances[i]) s_savedInstances[i]->Release();
	s_savedShader = nullptr;
	s_savedInstanceCount = 0;
}

bool GameEffectsSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash) {
	if (!s_sawEffect) {
		s_sawEffect = true;
		s_refractionCopy = BoundResource(c, 10);
	}
	if (g_cfg.lowHealthFx || !StereoActive() || !sampled) return false;
	ComPtr<ID3D11RenderTargetView> rtv;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (!rtv) return false;
	ComPtr<ID3D11Resource> target;
	rtv->GetResource(&target);
	D3D11_TEXTURE2D_DESC targetDesc;
	if (!Tex2D(target.Get(), &targetDesc)) return false;
	bool skip = false;
	UINT hit = 0;
	for (UINT slot = 0; slot < 32 && !skip; slot++) {
		if (!(sampled >> slot & 1)) continue;
		ID3D11Resource* res = BoundResource(c, slot);
		D3D11_TEXTURE2D_DESC desc;
		if (!res || res == target.Get() || res == s_refractionCopy || !Tex2D(res, &desc)) continue;
		skip = desc.Width == targetDesc.Width && desc.Height == targetDesc.Height && desc.Format == targetDesc.Format;
		hit = slot;
	}
	if (!skip) return false;
	if (!LowHealthShader(shaderHash)) {
		// reads a scene copy too, but isn't the low-health effect (e.g. Pod shots): kept, logged once
		bool seen = false;
		for (int i = 0; i < s_keptCount; i++) seen |= s_keptLogged[i] == (uint32_t)shaderHash;
		if (!seen && s_keptCount < 16) {
			s_keptLogged[s_keptCount++] = (uint32_t)shaderHash;
			Log("screen filter: effect %08x reads a scene copy at t%u but isn't a known low-health shader: kept",
				(unsigned)(uint32_t)shaderHash, hit);
		}
		return false;
	}
	s_filterSkips++;
	NoteSkip(shaderHash, hit);
	if (!s_filterLogged) {
		s_filterLogged = true;
		s_filterReportTick = GetTickCount();
		Log("screen filter effect (redraws a copy of the scene: low-health grey, blur, ripples) "
			"removed while in stereo (LowHealthEffect=0)");
	} else if (GetTickCount() - s_filterReportTick > 60000) {
		char list[300];
		size_t used = 0;
		list[0] = 0;
		for (auto& entry : s_skipped)
			if (entry.count && used + 40 < sizeof(list))
				used += snprintf(list + used, sizeof(list) - used, " %08x/t%u x%u", (unsigned)(entry.hash & 0xffffffff),
								 entry.slot, entry.count);
		Log("screen filter: %u draws removed in the last minute (shader/slot/count:%s)", s_filterSkips, list);
		for (auto& entry : s_skipped) entry = {};
		s_filterSkips = 0;
		s_filterReportTick = GetTickCount();
	}
	return true;
}

void GameEffectsOnMap(ID3D11Resource* res, void* data, UINT size) {
	if (res == s_vignetteBuffer && size == kVignetteBufferSize) s_vignetteMapped = data;
}

void GameEffectsBeforeUnmap(ID3D11Resource* res) {
	if (res != s_vignetteBuffer || !s_vignetteMapped) return;
	PatchVignette((float*)s_vignetteMapped);
	s_vignetteMapped = nullptr;
}

const void* GameEffectsPatchUpdate(ID3D11Resource* res, const void* data, UINT size, float* scratch) {
	if (res != s_vignetteBuffer || size != kVignetteBufferSize) return data;
	if (data != scratch) memcpy(scratch, data, kVignetteBufferSize);
	PatchVignette(scratch);
	return scratch;
}
