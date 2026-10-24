// shader_table.cpp - every shader the game creates (d3d_hooks.cpp's Create*Shader hooks), with a copy
// of its bytecode. Passes are recognised by the cbuffer and variable names in the bytecode's
// reflection data.
#include "render/shader_table.h"
#include "core/pure.h"
#include <string.h>

namespace {
struct ShaderEntry {
	void* ptr;
	uint64_t hash;
	uint8_t* blob;
	uint32_t len;
	ShaderStage stage;
	bool usedInCapture;
	bool ui;          // vertex shader with the UIParam / cbUIParam cbuffers
	bool sao;         // ambient occlusion: "SaoGlobalParams"
	bool shadowMask;  // screen-space shadow pass: reads "ShadowView_Buffer", not "ScatterParam"
	bool scatter;     // light scattering (fog, light shafts): "ScatterParam" (also reads the shadow map)
	bool vignette;    // tone mapping: "g_VignettoParam"
	bool effect;      // effect system: "EffectParameterIndex"
	ComputeShaderClass computeClass;
	uint32_t sampled;  // texture registers it samples (bit mask)
};
const uint32_t kSlots = 1 << 16;
ShaderEntry* g_shaders;
SRWLOCK g_lock = SRWLOCK_INIT;
uint32_t g_count;

uint32_t SlotOf(void* p) {
	uint64_t h = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
	return (uint32_t)(h >> 48) & (kSlots - 1);
}

uint64_t Fnv1a(const uint8_t* d, size_t n) {
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
	return h;
}

bool Contains(const uint8_t* blob, uint32_t len, const char* name) {
	size_t n = strlen(name);
	for (uint32_t k = 0; k + n <= len; k++)
		if (memcmp(blob + k, name, n) == 0) return true;
	return false;
}

ShaderEntry* Find(void* shader) {
	if (!g_shaders || !shader) return nullptr;
	uint32_t i = SlotOf(shader);
	for (uint32_t probe = 0; probe < kSlots; probe++, i = (i + 1) & (kSlots - 1)) {
		if (g_shaders[i].ptr == shader) return &g_shaders[i];
		if (!g_shaders[i].ptr) return nullptr;
	}
	return nullptr;
}
}  // namespace

void ShaderTableInit() {
	if (!g_shaders) g_shaders = (ShaderEntry*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ShaderEntry) * kSlots);
}

void ShaderTableRemember(void* shader, const void* bytecode, SIZE_T len, ShaderStage stage) {
	if (!g_shaders || !shader || !bytecode || len == 0 || len > (1u << 24)) return;
	uint8_t* copy = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, len);
	if (!copy) return;
	memcpy(copy, bytecode, len);
	AcquireSRWLockExclusive(&g_lock);
	uint32_t i = SlotOf(shader);
	for (uint32_t probe = 0; probe < kSlots; probe++, i = (i + 1) & (kSlots - 1)) {
		ShaderEntry& e = g_shaders[i];
		if (e.ptr != shader && e.ptr != nullptr) continue;
		if (e.blob) HeapFree(GetProcessHeap(), 0, e.blob);  // pointer reused by a new shader
		if (!e.ptr) g_count++;
		e = {};
		e.ptr = shader;
		e.hash = Fnv1a(copy, len);
		e.blob = copy;
		e.len = (uint32_t)len;
		e.stage = stage;
		uint32_t n = e.len;
		if (stage == ST_VS) e.ui = Contains(copy, n, "UIParam");
		if (stage == ST_CS)
			for (uint32_t k = 0; k + 11 <= n && e.computeClass == CS_OTHER; k++) {  // whichever name comes first
				if (memcmp(copy + k, "g_LightAry", 10) == 0) e.computeClass = CS_LIGHT_CULLING;
				else if (memcmp(copy + k, "g_CubeInput", 11) == 0) e.computeClass = CS_PROBE_SELECTION;
			}
		if (stage == ST_PS) {
			e.sampled = pure::dxbc_sampled_textures(copy, len);
			e.sao = Contains(copy, n, "SaoGlobalParams");
			// The light-scattering shader reads ShadowView_Buffer too, but it is not the shadow mask:
			// "Shadows off" on it turned into dense grey fog.
			e.scatter = Contains(copy, n, "ScatterParam");
			e.shadowMask = !e.scatter && Contains(copy, n, "ShadowView_Buffer");
			e.vignette = Contains(copy, n, "g_VignettoParam");
			e.effect = Contains(copy, n, "EffectParameterIndex");
		}
		ReleaseSRWLockExclusive(&g_lock);
		return;
	}
	ReleaseSRWLockExclusive(&g_lock);
	HeapFree(GetProcessHeap(), 0, copy);  // table full
}

bool ShaderTableIsUi(void* vertexShader) {
	AcquireSRWLockShared(&g_lock);
	ShaderEntry* e = Find(vertexShader);
	bool ui = e && e->ui;
	ReleaseSRWLockShared(&g_lock);
	return ui;
}

PixelShaderClass ShaderTableClassifyPixel(void* pixelShader, uint32_t* sampled, uint64_t* hash) {
	AcquireSRWLockShared(&g_lock);
	ShaderEntry* e = Find(pixelShader);
	PixelShaderClass k = PS_OTHER;
	if (e) {
		if (e->sao) k = PS_AMBIENT_OCCLUSION;
		else if (e->shadowMask) k = PS_SHADOW_MASK;
		else if (e->vignette) k = PS_TONE_MAPPING;
		else if (e->effect) k = PS_EFFECT;
		else if (e->scatter) k = PS_LIGHT_SCATTERING;
	}
	*sampled = e ? e->sampled : 0;
	*hash = e ? e->hash : 0;
	ReleaseSRWLockShared(&g_lock);
	return k;
}

ComputeShaderClass ShaderTableClassifyCompute(void* computeShader) {
	AcquireSRWLockShared(&g_lock);
	ShaderEntry* e = Find(computeShader);
	ComputeShaderClass k = e ? e->computeClass : CS_OTHER;
	ReleaseSRWLockShared(&g_lock);
	return k;
}

void ShaderTableMarkUsed(void* shader) {
	AcquireSRWLockShared(&g_lock);
	ShaderEntry* e = Find(shader);
	if (e) e->usedInCapture = true;
	ReleaseSRWLockShared(&g_lock);
}

int ShaderTableTakeUsed(ShaderBlobVisitor visit) {
	if (!g_shaders) return 0;
	int count = 0;
	AcquireSRWLockExclusive(&g_lock);
	for (uint32_t i = 0; i < kSlots; i++) {
		ShaderEntry& e = g_shaders[i];
		if (!e.ptr || !e.usedInCapture) continue;
		e.usedInCapture = false;
		visit(e.stage, e.ptr, e.hash, e.blob, e.len);
		count++;
	}
	ReleaseSRWLockExclusive(&g_lock);
	return count;
}

uint32_t ShaderTableCount() { return g_count; }
