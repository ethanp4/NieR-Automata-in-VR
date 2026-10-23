// upscale.cpp - temporal upscaling of the 3D image for the headset ([Upscaler] Mode, Quality).
//
// The game renders at its own (lowered) resolution; at Present each eye's HUD-free scene image is
// enlarged by DLSS (render/dlss.cpp) to the output size and handed to the headset in its place. DLSS
// builds detail up over several frames, so every frame:
//   - the eye's projection is shifted by a different fraction of a pixel (UpscaleJitter, applied in
//     stereo.cpp's PatchScene; a Halton sequence per eye),
//   - the game's depth is kept at the first UI draw (the scene is finished), like reproject.cpp,
//   - a compute pass makes what DLSS needs: the colour in a typed format, the depth as R32_FLOAT and
//     motion vectors. NieR has none of its own, so they come from depth and the camera: each pixel is
//     put back into 3D and projected with the same eye's previous camera. That covers camera motion;
//     things that move by themselves (2B's animation, enemies) have none and rely on DLSS.
// Each eye has its own DLSS history: it is only ever compared with its own previous frame (generic
// injectors mix the eyes and smear in VR). Alternate eyes: each eye's history moves at 30 images a second.
//
// The scene image's alpha channel holds the HUD's coverage (uisplit.cpp); DLSS only does colour, so a
// last full-screen pass writes DLSS's colour with the scene's alpha (stretched) into the image that
// goes to the headset. The same pass just stretches frames DLSS can't take (flat/menu frames, no depth
// or camera), so every scene image of a ring has the same size.
#include "core/common.h"
#include "render/dlss.h"
#include <d3dcompiler.h>

namespace {
const char kHlsl[] = R"(
cbuffer P : register(b0) {
	row_major float4x4 T;  // this eye's view now -> the same eye's view in its previous frame
	float4 cur;            // projection now: x scale, y scale, P22, P23 (z_ndc = (P22 z + P23) / -z)
	float4 prev;           // its previous projection
	float4 jit;            // this frame's jitter in NDC (x, y), render width, height
	float4 opt;            // x > 0: the previous frame is known (else no motion)
};
Texture2D<float> dep : register(t0);
Texture2D col : register(t1);
RWTexture2D<float4> outCol : register(u0);
RWTexture2D<float> outDep : register(u1);
RWTexture2D<float2> outMv : register(u2);
[numthreads(8, 8, 1)]
void prep(uint3 id : SV_DispatchThreadID) {
	if (id.x >= (uint)jit.z || id.y >= (uint)jit.w) return;
	float d = dep.Load(int3(id.xy, 0));
	outCol[id.xy] = col.Load(int3(id.xy, 0));
	outDep[id.xy] = d;
	float2 mv = 0;
	if (opt.x > 0) {
		float2 uv = (id.xy + 0.5) / jit.zw;
		float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2) - jit.xy;  // where the point is without jitter
		float den = -d - cur.z;
		float z = abs(den) > 1e-9 ? cur.w / den : -1e7;
		if (!(z < -1e-4) || z < -1e7) z = -1e7;  // sky: so far away only turning moves it
		float3 p = float3(ndc.x * -z / cur.x, ndc.y * -z / cur.y, z);
		float3 q = mul(T, float4(p, 1)).xyz;
		if (q.z < -1e-4) {
			float2 pn = float2(q.x * prev.x, q.y * prev.y) / -q.z;
			mv = float2(pn.x - ndc.x, ndc.y - pn.y) * 0.5 * jit.zw;  // pixels to where it was (y down)
		}
	}
	outMv[id.xy] = mv;
}

Texture2D src : register(t0);    // DLSS's output, or the scene to stretch
Texture2D alpha : register(t1);  // the scene: HUD coverage in alpha
SamplerState smp : register(s0);
cbuffer C : register(b0) { float4 size; };  // output width, height
float4 vs(uint id : SV_VertexID) : SV_Position {
	float2 t = float2((id << 1) & 2, id & 2);
	return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
	float2 uv = pos.xy / size.xy;
	return float4(src.SampleLevel(smp, uv, 0).rgb, alpha.SampleLevel(smp, uv, 0).a);
}
)";

template <typename T>
void Rel(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

bool g_init, g_failed;
ID3D11ComputeShader* g_prep;
ID3D11VertexShader* g_vs;
ID3D11PixelShader* g_ps;
ID3D11Buffer* g_cbPrep;
ID3D11Buffer* g_cbCompose;
ID3D11SamplerState* g_smp;

// this frame (from UpscaleBeginPresent)
bool g_active;       // scene images are enlarged
UINT g_rw, g_rh;     // render size
UINT g_ow, g_oh;     // output size
int g_mode;          // DLSS mode for that enlargement (pure::upscale_mode)
UINT g_targetW, g_targetH;  // the output the headset wants (native x Output), 0 = not known yet
unsigned g_present;  // Presents so far (the depth copies are tagged with it)

// per eye
struct Eye {
	ID3D11Texture2D* depth;  // readable twin of the game's depth buffer
	ID3D11ShaderResourceView* depthSrv;
	unsigned depthPresent;   // g_present of the frame the depth belongs to
	bool depthOk;
	bool reset;              // the next DLSS frame starts a new history
	uint32_t jitterIndex;
	unsigned frames, fallbacks;  // since the last report
} g_eye[2];
D3D11_TEXTURE2D_DESC g_depthDesc;

// render-size inputs for DLSS (shared by the eyes: they are used one after the other)
ID3D11Texture2D *g_color, *g_dep, *g_mv;
ID3D11UnorderedAccessView *g_colorUav, *g_depUav, *g_mvUav;
// output size
ID3D11Texture2D* g_dlssOut;
ID3D11ShaderResourceView* g_dlssOutSrv;
ID3D11Texture2D* g_final;  // what the headset gets: the scene's format
ID3D11RenderTargetView* g_finalRtv;
UINT g_tw, g_th, g_tow, g_toh;
DXGI_FORMAT g_tFmt;
// SRV of the game-side scene texture (uisplit.cpp keeps it while the size stays)
ID3D11Texture2D* g_sceneFor;
ID3D11ShaderResourceView* g_sceneSrv;

SRWLOCK g_statusLock = SRWLOCK_INIT;
char g_status[128];
char g_advice[160];
DWORD g_reportTick;

const char* const kMode[5] = {"DLAA", "Quality", "Balanced", "Performance", "Ultra performance"};

DXGI_FORMAT Typed(DXGI_FORMAT f) {
	switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
	}
}
bool Is8Bit(DXGI_FORMAT f) {
	f = Typed(f);
	return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM ||
		   f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
// readable twin of a depth format, and the view that reads its depth
bool DepthFormats(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* srv) {
	switch (f) {
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24G8_TYPELESS: *tex = DXGI_FORMAT_R24G8_TYPELESS; *srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_TYPELESS: *tex = DXGI_FORMAT_R32_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT; return true;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32G8X24_TYPELESS: *tex = DXGI_FORMAT_R32G8X24_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_TYPELESS: *tex = DXGI_FORMAT_R16_TYPELESS; *srv = DXGI_FORMAT_R16_UNORM; return true;
		default: return false;
	}
}

void SetStatus(const char* text, const char* advice) {
	AcquireSRWLockExclusive(&g_statusLock);
	snprintf(g_status, sizeof(g_status), "%s", text);
	snprintf(g_advice, sizeof(g_advice), "%s", advice);
	ReleaseSRWLockExclusive(&g_statusLock);
}

bool InitShaders(ID3D11Device* dev) {
	if (g_init || g_failed) return g_init;
	ID3DBlob *cs = nullptr, *vs = nullptr, *ps = nullptr, *err = nullptr;
	const UINT fl = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "Upscale", nullptr, nullptr, "prep", "cs_5_0", fl, 0, &cs, &err);
	if (SUCCEEDED(hr)) hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "Upscale", nullptr, nullptr, "vs", "vs_5_0", fl, 0, &vs, &err);
	if (SUCCEEDED(hr)) hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "Upscale", nullptr, nullptr, "ps", "ps_5_0", fl, 0, &ps, &err);
	if (SUCCEEDED(hr)) hr = dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_prep);
	if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
	if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
	if (FAILED(hr)) Log("ERROR: upscaler shaders: 0x%08x %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
	Rel(cs), Rel(vs), Rel(ps), Rel(err);
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 128;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &g_cbPrep);
	bd.ByteWidth = 16;
	if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &g_cbCompose);
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(hr)) hr = dev->CreateSamplerState(&sd, &g_smp);
	g_init = SUCCEEDED(hr);
	g_failed = !g_init;
	return g_init;
}

void ReleaseTargets() {
	Rel(g_colorUav), Rel(g_depUav), Rel(g_mvUav);
	Rel(g_color), Rel(g_dep), Rel(g_mv);
	Rel(g_dlssOutSrv), Rel(g_dlssOut);
	Rel(g_finalRtv), Rel(g_final);
	Rel(g_sceneSrv), Rel(g_sceneFor);
	g_tw = g_th = g_tow = g_toh = 0;
}

ID3D11Texture2D* Tex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w, d.Height = h, d.MipLevels = d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = bind;
	ID3D11Texture2D* t = nullptr;
	return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) ? t : nullptr;
}

// Textures for this render size, output size and scene format.
bool EnsureTargets(ID3D11Device* dev, DXGI_FORMAT sceneFmt) {
	if (g_final && g_tw == g_rw && g_th == g_rh && g_tow == g_ow && g_toh == g_oh && g_tFmt == sceneFmt) return true;
	ReleaseTargets();
	DlssReleaseFeatures();  // sizes changed: new histories
	for (auto& e : g_eye) e.reset = true;
	// DLSS works in 8 bits per channel for 8-bit images, else half floats
	DXGI_FORMAT work = Is8Bit(sceneFmt) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
	const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	g_color = Tex(dev, g_rw, g_rh, work, rw);
	g_dep = Tex(dev, g_rw, g_rh, DXGI_FORMAT_R32_FLOAT, rw);
	g_mv = Tex(dev, g_rw, g_rh, DXGI_FORMAT_R16G16_FLOAT, rw);
	g_dlssOut = Tex(dev, g_ow, g_oh, work, rw);
	g_final = Tex(dev, g_ow, g_oh, sceneFmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
	bool ok = g_color && g_dep && g_mv && g_dlssOut && g_final;
	ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(g_color, nullptr, &g_colorUav)) &&
		 SUCCEEDED(dev->CreateUnorderedAccessView(g_dep, nullptr, &g_depUav)) &&
		 SUCCEEDED(dev->CreateUnorderedAccessView(g_mv, nullptr, &g_mvUav)) &&
		 SUCCEEDED(dev->CreateShaderResourceView(g_dlssOut, nullptr, &g_dlssOutSrv));
	D3D11_RENDER_TARGET_VIEW_DESC rv = {};
	rv.Format = Typed(sceneFmt);
	rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	ok = ok && SUCCEEDED(dev->CreateRenderTargetView(g_final, &rv, &g_finalRtv));
	if (!ok) {
		Log("ERROR: upscaler: could not create its textures (%ux%u -> %ux%u, format %d)", g_rw, g_rh, g_ow, g_oh,
			(int)sceneFmt);
		ReleaseTargets();
		return false;
	}
	g_tw = g_rw, g_th = g_rh, g_tow = g_ow, g_toh = g_oh, g_tFmt = sceneFmt;
	return true;
}

ID3D11ShaderResourceView* SceneSrv(ID3D11Device* dev, ID3D11Texture2D* scene) {
	if (scene == g_sceneFor && g_sceneSrv) return g_sceneSrv;
	Rel(g_sceneSrv), Rel(g_sceneFor);
	D3D11_TEXTURE2D_DESC d;
	scene->GetDesc(&d);
	D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
	v.Format = Typed(d.Format);
	v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	v.Texture2D.MipLevels = 1;
	if (FAILED(dev->CreateShaderResourceView(scene, &v, &g_sceneSrv))) return nullptr;
	g_sceneFor = scene;
	scene->AddRef();  // the view's texture: compared by address, so keep it alive
	return g_sceneSrv;
}

// The depth twin for this depth buffer's size and format.
bool EnsureDepth(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
	if (g_eye[0].depth && g_depthDesc.Width == src.Width && g_depthDesc.Height == src.Height &&
		g_depthDesc.Format == src.Format)
		return true;
	DXGI_FORMAT tf, sf;
	if (!DepthFormats(src.Format, &tf, &sf) || src.SampleDesc.Count != 1) return false;
	for (auto& e : g_eye) {
		Rel(e.depthSrv), Rel(e.depth);
		e.depthOk = false;
		D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
		v.Format = sf;
		v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		v.Texture2D.MipLevels = 1;
		e.depth = Tex(dev, src.Width, src.Height, tf, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		if (!e.depth || FAILED(dev->CreateShaderResourceView(e.depth, &v, &e.depthSrv))) {
			Log("upscaler: could not create a readable depth copy (format %d)", (int)src.Format);
			Rel(e.depth);
			return false;
		}
	}
	g_depthDesc = src;
	return true;
}

void CopyDepth(ID3D11DeviceContext* c, int eye, unsigned present) {
	ID3D11Resource* d = ReprojectSceneDepth();
	if (!g_active || eye < 0 || eye > 1 || !d) return;
	ID3D11Device* dev = nullptr;
	c->GetDevice(&dev);
	if (!dev) return;
	D3D11_TEXTURE2D_DESC dd;
	((ID3D11Texture2D*)d)->GetDesc(&dd);
	if (dd.Width == g_rw && dd.Height == g_rh && EnsureDepth(dev, dd)) {
		c->CopyResource(g_eye[eye].depth, d);
		g_eye[eye].depthOk = true;
		g_eye[eye].depthPresent = present;
	}
	dev->Release();
}

// Upscales `scene` (render size) for `eye` into g_dlssOut. False if this frame can't be.
bool RunDlss(ID3D11DeviceContext* c, ID3D11Device* dev, int eye, ID3D11ShaderResourceView* sceneSrv) {
	Eye& e = g_eye[eye];
	float T[16], cur[4], prev[4], jit[2];
	bool hasPrev = false;
	if (!e.depthOk || e.depthPresent != g_present) return false;
	if (!StereoUpscaleCams(eye, T, cur, prev, jit, &hasPrev)) return false;
	// depth convention: farther = larger value normally, smaller with reversed depth
	float dNear = -cur[2] - cur[3] / -1.0f, dFar = -cur[2] - cur[3] / -1000.0f;
	bool inverted = dFar < dNear;
	if (!DlssPrepare(c, eye, g_rw, g_rh, g_ow, g_oh, g_mode, g_cfg.dlssModel, inverted)) return false;
	struct {
		float T[16];
		float cur[4], prev[4], jit[4], opt[4];
	} p;
	static_assert(sizeof(p) == 128, "cbuffer P");
	memcpy(p.T, T, 64);
	memcpy(p.cur, cur, 16);
	memcpy(p.prev, prev, 16);
	p.jit[0] = 2 * jit[0] / (float)g_rw;
	p.jit[1] = -2 * jit[1] / (float)g_rh;
	p.jit[2] = (float)g_rw;
	p.jit[3] = (float)g_rh;
	p.opt[0] = hasPrev ? 1.f : 0.f;
	p.opt[1] = p.opt[2] = p.opt[3] = 0;
	c->UpdateSubresource(g_cbPrep, 0, nullptr, &p, 0, 0);
	c->CSSetShader(g_prep, nullptr, 0);
	c->CSSetConstantBuffers(0, 1, &g_cbPrep);
	ID3D11ShaderResourceView* srvs[2] = {e.depthSrv, sceneSrv};
	c->CSSetShaderResources(0, 2, srvs);
	ID3D11UnorderedAccessView* uavs[3] = {g_colorUav, g_depUav, g_mvUav};
	c->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	c->Dispatch((g_rw + 7) / 8, (g_rh + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrv[2] = {};
	ID3D11UnorderedAccessView* nullUav[3] = {};
	c->CSSetShaderResources(0, 2, nullSrv);
	c->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
	c->CSSetShader(nullptr, nullptr, 0);
	DlssInputs in = {};
	in.color = g_color;
	in.depth = g_dep;
	in.motion = g_mv;
	in.output = g_dlssOut;
	in.jitterX = g_cfg.upscaleJitterFlip & 1 ? -jit[0] : jit[0];
	in.jitterY = g_cfg.upscaleJitterFlip & 2 ? -jit[1] : jit[1];
	in.reset = e.reset || !hasPrev;
	(void)dev;
	if (!DlssEvaluate(c, eye, in)) return false;
	e.reset = false;
	e.jitterIndex++;
	return true;
}
}  // namespace

bool UpscaleWantsDepth() { return g_cfg.upscaler != 0; }

bool UpscaleBeginPresent(ID3D11DeviceContext* c, UINT w, UINT h, UINT* outW, UINT* outH) {
	g_present++;
	*outW = w;
	*outH = h;
	bool was = g_active;
	g_active = false;
	// The output follows the headset (its recommended detail x Output), as in other games the
	// display resolution does; the game's resolution is the input and decides the mode. Small changes
	// of the target (a few %) keep the current size: every change rebuilds DLSS and the hand-off.
	EnterCriticalSection(&g_share.cs);
	UINT nw = g_share.nativeW, nh = g_share.nativeH;
	LeaveCriticalSection(&g_share.cs);
	UINT tw = (UINT)(nw * g_cfg.upscaleOutput / 100), th = (UINT)(nh * g_cfg.upscaleOutput / 100);
	if (!g_targetW || tw > g_targetW * 1.03f || tw * 1.03f < g_targetW || th > g_targetH * 1.03f || th * 1.03f < g_targetH)
		g_targetW = tw, g_targetH = th;
	{
		ID3D11Device* dev = nullptr;
		c->GetDevice(&dev);
		if (dev) DlssPoll(dev), dev->Release();
	}
	if (g_cfg.upscaler) {
		ID3D11Device* dev = nullptr;
		c->GetDevice(&dev);
		if (dev) {
			UINT ow, oh;
			if (DlssInit(dev) && InitShaders(dev) && pure::upscale_output(w, h, g_targetW, g_targetH, &ow, &oh)) {
				g_active = true;
				g_rw = w, g_rh = h, g_ow = ow, g_oh = oh;
				g_mode = pure::upscale_mode((float)ow / (float)w);
				*outW = ow;
				*outH = oh;
			}
			dev->Release();
		}
	}
	if (was && !g_active) {
		ReleaseTargets();
		DlssReleaseFeatures();
		for (auto& e : g_eye) e.depthOk = false;
	}
	char s[128], adv[160];
	adv[0] = 0;
	if (!g_cfg.upscaler)
		s[0] = 0;
	else if (g_active)
		snprintf(s, sizeof(s), "%s %s (%.2fx), %ux%u -> %ux%u", DlssStatus(), kMode[g_mode], (double)g_ow / g_rw, g_rw,
				 g_rh, g_ow, g_oh);
	else
		snprintf(s, sizeof(s), "%s", DlssStatus());
	if (g_active && g_targetW && g_targetH) {
		// the game resolutions that give each mode for this output (same shape as the game's image)
		uint32_t qw = w, qh = h, bw, bh, pw, ph;
		pure::upscale_output(w, h, g_targetW, g_targetH, &qw, &qh);  // the target in the game's shape
		if (qw == w) {  // the game is at or above the target: show the target itself
			float r = (float)g_targetW / w > (float)g_targetH / h ? (float)g_targetW / w : (float)g_targetH / h;
			qw = (uint32_t)(w * r) & ~1u, qh = (uint32_t)(h * r) & ~1u;
		}
		uint32_t ow = qw, oh = qh;
		pure::upscale_input_for(ow, oh, 1, &qw, &qh);
		pure::upscale_input_for(ow, oh, 2, &bw, &bh);
		pure::upscale_input_for(ow, oh, 3, &pw, &ph);
		snprintf(adv, sizeof(adv), "Set the game to: DLAA %ux%u, Quality %ux%u, Balanced %ux%u, Performance %ux%u", ow, oh,
				 qw, qh, bw, bh, pw, ph);
	}
	SetStatus(s, adv);
	static char lastAdv[160];
	if (strcmp(lastAdv, adv)) {
		snprintf(lastAdv, sizeof(lastAdv), "%s", adv);
		MenuSetInfo(17, adv);
	}
	if (GetTickCount() - g_reportTick > 30000) {
		g_reportTick = GetTickCount();
		if (g_active && (g_eye[0].frames + g_eye[0].fallbacks + g_eye[1].frames + g_eye[1].fallbacks)) {
			Log("upscaler: %s; last 30 s: left %u upscaled / %u stretched, right %u / %u", s, g_eye[0].frames,
				g_eye[0].fallbacks, g_eye[1].frames, g_eye[1].fallbacks);
			char info[160];
			snprintf(info, sizeof(info), "Upscaler: %s; %u of %u eye images upscaled (30 s)", s,
					 g_eye[0].frames + g_eye[1].frames,
					 g_eye[0].frames + g_eye[1].frames + g_eye[0].fallbacks + g_eye[1].fallbacks);
			MenuSetInfo(16, info);
		} else if (g_cfg.upscaler) {
			char info[160];
			snprintf(info, sizeof(info), "Upscaler: %s", s);
			MenuSetInfo(16, info);
		}
		for (auto& e : g_eye) e.frames = e.fallbacks = 0;
	}
	return g_active;
}

ID3D11Texture2D* UpscaleScene(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* scene) {
	if (!g_active || !scene) return scene;
	ID3D11Device* dev = nullptr;
	c->GetDevice(&dev);
	if (!dev) return nullptr;
	dev->Release();  // the context keeps it alive
	D3D11_TEXTURE2D_DESC sd;
	scene->GetDesc(&sd);
	if (sd.Width != g_rw || sd.Height != g_rh || !EnsureTargets(dev, sd.Format)) return nullptr;
	ID3D11ShaderResourceView* sceneSrv = SceneSrv(dev, scene);
	if (!sceneSrv) return nullptr;
	g_replaying = true;  // our own calls (and DLSS's): the hooks pass them straight through
	StateSaveTemp(c);
	c->ClearState();
	bool up = eye >= 0 && eye < 2 && RunDlss(c, dev, eye, sceneSrv);
	if (eye >= 0 && eye < 2) {
		if (up) g_eye[eye].frames++;
		else {
			g_eye[eye].fallbacks++;
			g_eye[eye].reset = true;
		}
	}
	// the image for the headset: DLSS's colour (or the stretched scene) with the scene's HUD coverage
	c->ClearState();
	float size[4] = {(float)g_ow, (float)g_oh, 0, 0};
	c->UpdateSubresource(g_cbCompose, 0, nullptr, size, 0, 0);
	D3D11_VIEWPORT vp = {0, 0, (float)g_ow, (float)g_oh, 0, 1};
	c->RSSetViewports(1, &vp);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_vs, nullptr, 0);
	c->PSSetShader(g_ps, nullptr, 0);
	c->PSSetConstantBuffers(0, 1, &g_cbCompose);
	ID3D11ShaderResourceView* srvs[2] = {up ? g_dlssOutSrv : sceneSrv, sceneSrv};
	c->PSSetShaderResources(0, 2, srvs);
	c->PSSetSamplers(0, 1, &g_smp);
	c->OMSetRenderTargets(1, &g_finalRtv, nullptr);
	c->Draw(3, 0);
	StateRestoreTemp(c);
	g_replaying = false;
	return g_final;
}

void UpscaleAtUiStart(ID3D11DeviceContext* c) { CopyDepth(c, StereoFrameEye(), g_present + 1); }  // for the coming Present
void UpscaleCaptureDepth(ID3D11DeviceContext* c, int eye) { CopyDepth(c, eye, g_present); }

bool UpscaleJitter(int eye, float ndc[2], float pixels[2]) {
	if (!g_active || eye < 0 || eye > 1 || !g_rw || !g_rh) return false;
	uint32_t n = pure::jitter_phases((float)g_ow / (float)g_rw);
	uint32_t i = g_eye[eye].jitterIndex % n + 1;
	pixels[0] = pure::halton(i, 2) - 0.5f;
	pixels[1] = pure::halton(i, 3) - 0.5f;
	ndc[0] = 2 * pixels[0] / (float)g_rw;
	ndc[1] = -2 * pixels[1] / (float)g_rh;
	return true;
}

void UpscaleRelease() {
	ReleaseTargets();
	DlssReleaseFeatures();
	for (auto& e : g_eye) {
		Rel(e.depthSrv), Rel(e.depth);
		e.depthOk = false;
		e.reset = true;
	}
	g_depthDesc = {};
}

void UpscaleStatus(char* out, size_t cap) {
	AcquireSRWLockShared(&g_statusLock);
	snprintf(out, cap, "%s", g_status);
	ReleaseSRWLockShared(&g_statusLock);
}
