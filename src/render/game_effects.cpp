// game_effects.cpp - switches off NieR effects that don't suit VR: vignette, shadows, ambient occlusion, low-health filter.
#include "render/game_effects.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include <stdio.h>
#include <string.h>
#include <d3dcompiler.h>

namespace {
// --- vignette ----------------------------------------------------------------------------------
// The 48-byte PSParamBuffer1 bound at b1 when the tone-mapping shader draws; g_VignettoParam.x
// (float 8) is the radius where darkening ends, as in v = pow(saturate((1 - |uv - 0.5|) / x), y).
// A tiny x keeps v = 1 everywhere.
const UINT kVignetteBufferSize = 48;
ID3D11Resource* s_vignetteBuffer;
void* s_vignetteMapped;

void PatchVignette(float* f) {
	if (!g_cfg.vignette) f[8] = 0.0001f;
}

void FindVignetteBuffer(ID3D11DeviceContext* c) {
	ID3D11Buffer* b = nullptr;
	c->PSGetConstantBuffers(1, 1, &b);
	if (!b) return;
	D3D11_BUFFER_DESC d;
	b->GetDesc(&d);
	if (d.ByteWidth == kVignetteBufferSize) {
		s_vignetteBuffer = b;
		Log("vignette: parameters found (buffer of %u bytes); %s", d.ByteWidth,
			g_cfg.vignette ? "kept (Vignette=1)" : "removed from the next frame on");
	}
	b->Release();
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
	HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
	static const char src[] = "float4 main() : SV_Target { return 1; }";  // no inputs: links to any VS
	ID3DBlob *b = nullptr, *err = nullptr;
	HRESULT hr = compile ? compile(src, sizeof(src) - 1, "lit", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &err) : E_FAIL;
	ID3D11Device* dev = nullptr;
	c->GetDevice(&dev);
	if (SUCCEEDED(hr) && dev) hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s_litShader);
	if (dev) dev->Release();
	if (FAILED(hr)) {
		s_litShader = nullptr;
		Log("shadows off: no replacement shader (0x%08x); the shadow pass stays on", (unsigned)hr);
	}
	if (b) b->Release();
	if (err) err->Release();
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
unsigned s_filterSkips;
DWORD s_filterReportTick;
bool s_filterLogged;
// The low-health effect's pixel shaders (FNV-1a of the bytecode, low 32 bits): grey, ripples, blur,
// and two more drawn with them.
const uint32_t kLowHealthShaders[] = {0x86575858, 0x833497f9, 0xcebc6f6d, 0x8b838343, 0x32c5d826};
uint32_t s_keptLogged[16];
int s_keptCount;

bool LowHealthShader(uint64_t hash) {
	for (uint32_t h : kLowHealthShaders)
		if ((uint32_t)hash == h) return true;
	return false;
}

// Which effect shaders were skipped, for the minute report (bytecode hash, count).
struct Skipped {
	uint64_t hash;
	unsigned count;
	UINT slot;
};
Skipped s_skipped[8];

void NoteSkip(uint64_t hash, UINT slot) {
	for (auto& e : s_skipped)
		if (e.hash == hash && e.count) {
			e.count++;
			return;
		}
	for (auto& e : s_skipped)
		if (!e.count) {
			e = {hash, 1, slot};
			return;
		}
}

ID3D11Resource* BoundResource(ID3D11DeviceContext* c, UINT slot) {
	ID3D11ShaderResourceView* v = nullptr;
	c->PSGetShaderResources(slot, 1, &v);
	if (!v) return nullptr;
	ID3D11Resource* r = nullptr;
	v->GetResource(&r);
	v->Release();
	if (r) r->Release();  // still bound: stays alive for this draw
	return r;
}

bool Tex2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* d) {
	if (!r) return false;
	D3D11_RESOURCE_DIMENSION dim;
	r->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
	((ID3D11Texture2D*)r)->GetDesc(d);
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
	ID3D11RenderTargetView* rtv = nullptr;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (rtv) {
		const FLOAT one[4] = {1, 1, 1, 1};
		c->ClearRenderTargetView(rtv, one);
		rtv->Release();
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
	ID3D11RenderTargetView* rtv = nullptr;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (!rtv) return false;
	ID3D11Resource* rt = nullptr;
	rtv->GetResource(&rt);
	rtv->Release();
	if (rt) rt->Release();
	D3D11_TEXTURE2D_DESC td;
	if (!Tex2D(rt, &td)) return false;
	bool skip = false;
	UINT hit = 0;
	for (UINT slot = 0; slot < 32 && !skip; slot++) {
		if (!(sampled >> slot & 1)) continue;
		ID3D11Resource* r = BoundResource(c, slot);
		D3D11_TEXTURE2D_DESC d;
		if (!r || r == rt || r == s_refractionCopy || !Tex2D(r, &d)) continue;
		skip = d.Width == td.Width && d.Height == td.Height && d.Format == td.Format;
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
		size_t at = 0;
		list[0] = 0;
		for (auto& e : s_skipped)
			if (e.count && at + 40 < sizeof(list))
				at += snprintf(list + at, sizeof(list) - at, " %08x/t%u x%u", (unsigned)(e.hash & 0xffffffff),
							   e.slot, e.count);
		Log("screen filter: %u draws removed in the last minute (shader/slot/count:%s)", s_filterSkips, list);
		for (auto& e : s_skipped) e = {};
		s_filterSkips = 0;
		s_filterReportTick = GetTickCount();
	}
	return true;
}

void GameEffectsOnMap(ID3D11Resource* r, void* data, UINT size) {
	if (r == s_vignetteBuffer && size == kVignetteBufferSize) s_vignetteMapped = data;
}

void GameEffectsBeforeUnmap(ID3D11Resource* r) {
	if (r != s_vignetteBuffer || !s_vignetteMapped) return;
	PatchVignette((float*)s_vignetteMapped);
	s_vignetteMapped = nullptr;
}

const void* GameEffectsPatchUpdate(ID3D11Resource* r, const void* data, UINT size, float* scratch) {
	if (r != s_vignetteBuffer || size != kVignetteBufferSize) return data;
	if (data != scratch) memcpy(scratch, data, kVignetteBufferSize);
	PatchVignette(scratch);
	return scratch;
}
