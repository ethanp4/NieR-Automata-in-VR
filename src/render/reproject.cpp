// reproject.cpp - "reproject" one eyes frame at the other eyes perspective
#include "render/reproject.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/frame_share.h"
#include "render/replay.h"
#include "render/upscale.h"
#include "ui/menu.h"
#include <stdio.h>
#include <string.h>
#include <d3dcompiler.h>

namespace {
const char kHlsl[] = R"(
cbuffer P : register(b0) {
	row_major float4x4 T;   // old eye view -> eye view now
	float4 proj;            // x scale, y scale, P22, P23 (z_ndc = (P22 z + P23) / -z)
	float4 size;            // width, height, grid step, grid width in cells
	float4 edge;            // depth ratio and stretch (in grid steps) above which a triangle is a gap; x = 0: off
	float4 dscale;          // depth pixels per colour pixel (x, y): 1, or less for an upscaled colour image
};
Texture2D<float> dep : register(t0);
Texture2D col : register(t1);
SamplerState smp : register(s0);
struct V {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	nointerpolation float gap : TEXCOORD1;
};
// one grid corner: the old image's pixel there, moved into the eye's view as it is now
void Corner(uint2 c, out float4 clip, out float2 uv, out float dist) {
	float2 px = min(float2(c) * size.z, size.xy - 1);
	float d = dep.Load(int3(px * dscale.xy, 0));
	uv = (px + 0.5) / size.xy;
	float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
	float z = proj.w / (-d - proj.z);                   // view-space z (negative in front)
	float3 p = float3(ndc.x * -z / proj.x, ndc.y * -z / proj.y, z);
	float3 q = mul(T, float4(p, 1)).xyz;
	clip = float4(q.x * proj.x, q.y * proj.y, proj.z * q.z + proj.w, -q.z);
	clip.z *= 0.999;  // real surfaces stay in front of the gap fill, the sky (depth 1) included
	dist = -z;
}
V vs(uint id : SV_VertexID) {
	uint cell = id / 6, k = id % 6, j = k % 3;
	uint gw = (uint)size.w;
	uint2 c = uint2(cell % gw, cell / gw);
	// each vertex works out its whole triangle, so all three agree whether it spans a gap
	uint2 c0 = k < 3 ? uint2(0, 0) : uint2(1, 0);
	uint2 c1 = k < 3 ? uint2(1, 0) : uint2(1, 1);
	uint2 c2 = uint2(0, 1);
	float4 p0, p1, p2;
	float2 u0, u1, u2;
	float d0, d1, d2;
	Corner(c + c0, p0, u0, d0);
	Corner(c + c1, p1, u1, d1);
	Corner(c + c2, p2, u2, d2);
	float dmin = min(d0, min(d1, d2)), dmax = max(d0, max(d1, d2));
	bool gap = false;
	if (edge.x > 0 && p0.w > 0 && p1.w > 0 && p2.w > 0) {
		float2 h = 0.5 * size.xy;
		float2 s0 = p0.xy / p0.w * h, s1 = p1.xy / p1.w * h, s2 = p2.xy / p2.w * h;
		float e = max(length(s0 - s1), max(length(s1 - s2), length(s2 - s0)));
		// a depth edge in the old image that the camera motion pulled open
		gap = dmax > dmin * edge.x && e > size.z * edge.y;
	}
	V o;
	o.pos = j == 0 ? p0 : (j == 1 ? p1 : p2);
	o.uv = j == 0 ? u0 : (j == 1 ? u1 : u2);
	o.gap = gap ? 1 : 0;
	if (gap) o.pos.z = o.pos.w * 0.9999;  // behind everything real: only fills what nothing else covers
	return o;
}
float4 ps(V i) : SV_Target {
	if (i.gap < 0.5) return col.SampleLevel(smp, i.uv, 0);
	// uncovered background: take the colour of the farthest pixel around (the background, not 2B's
	// edge stretched over the gap)
	float2 px = i.uv * size.xy - 0.5;
	float best = -1;
	float2 bp = px;
	[unroll] for (int y = -1; y <= 1; y++)
		[unroll] for (int x = -1; x <= 1; x++) {
			float2 q = clamp(px + float2(x, y) * size.z, 0, size.xy - 1);
			float d = dep.Load(int3(q * dscale.xy, 0));
			if (d > best) { best = d; bp = q; }
		}
	return col.Load(int3(bp, 0));
}
)";

ID3D11VertexShader* s_warpVs;
ID3D11PixelShader* s_warpPs;
ID3D11Buffer* s_warpConstants;
ID3D11SamplerState* s_linearSampler;
ID3D11DepthStencilState* s_nearestWinsDepth;
ID3D11RasterizerState* s_noCullRasterizer;
ID3D11BlendState* s_opaqueBlend;
bool s_gpuObjectsReady, s_gpuObjectsFailed;

struct EyeState {
	ID3D11Texture2D* sceneCopy;
	ID3D11ShaderResourceView* sceneCopySrv;
	ID3D11Texture2D* depthCopy;
	ID3D11ShaderResourceView* depthCopySrv;
	unsigned depthPresentCount;  // s_presentCount when the depth was copied
	bool sceneValid, depthValid;
};
EyeState s_eyes[2];
ID3D11Texture2D* s_warpedImage;
ID3D11RenderTargetView* s_warpedImageRtv;
ID3D11Texture2D* s_warpedDepth;
ID3D11DepthStencilView* s_warpedDepthDsv;
D3D11_TEXTURE2D_DESC s_sceneCopyDesc, s_depthCopyDesc;
ID3D11Resource* s_sceneDepth;  // raw: the game's full-size depth buffer, as last bound
UINT s_backbufferW, s_backbufferH;
unsigned s_presentCount;
unsigned s_warpedCount, s_skippedCount;
// why frames weren't corrected (logged with the 30 s report)
enum SkipReason { SKIP_NO_IMAGE,
				  SKIP_NO_DEPTH,
				  SKIP_OLD_DEPTH,
				  SKIP_SIZE_MISMATCH,
				  SKIP_NO_CAMERA,
				  SKIP_REASON_COUNT };
unsigned s_skipCounts[SKIP_REASON_COUNT];
const char* const kSkipReasonText[SKIP_REASON_COUNT] = {"no image of the other eye", "no depth of the other eye",
														"the other eye's depth isn't from the previous frame", "image and depth sizes differ",
														"the camera wasn't found in one of the two frames"};
DWORD s_lastReportTick;
bool s_loggedWorking;

template <typename T>
void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

void ReleaseTargets() {
	for (auto& e : s_eyes) {
		SafeRelease(e.sceneCopySrv);
		SafeRelease(e.sceneCopy);
		SafeRelease(e.depthCopySrv);
		SafeRelease(e.depthCopy);
		e.sceneValid = e.depthValid = false;
	}
	SafeRelease(s_warpedImageRtv);
	SafeRelease(s_warpedImage);
	SafeRelease(s_warpedDepthDsv);
	SafeRelease(s_warpedDepth);
	s_sceneCopyDesc = {};
	s_depthCopyDesc = {};
}

bool CreateGpuObjects(ID3D11Device* dev) {
	if (s_gpuObjectsReady || s_gpuObjectsFailed) return s_gpuObjectsReady;
	HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
	ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *err = nullptr;
	HRESULT hr = compile ? compile(kHlsl, sizeof(kHlsl) - 1, "Reproject", nullptr, nullptr, "vs", "vs_5_0",
								   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &err)
						 : E_FAIL;
	if (SUCCEEDED(hr)) hr = compile(kHlsl, sizeof(kHlsl) - 1, "Reproject", nullptr, nullptr, "ps", "ps_5_0",
									D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &err);
	if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s_warpVs);
	if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &s_warpPs);
	if (FAILED(hr))
		Log("ERROR: depth reprojection shader: 0x%08x %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
	if (vsBlob) vsBlob->Release();
	if (psBlob) psBlob->Release();
	if (err) err->Release();
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 128;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &s_warpConstants);
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(hr)) hr = dev->CreateSamplerState(&sd, &s_linearSampler);
	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = TRUE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dd.DepthFunc = D3D11_COMPARISON_LESS;
	if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &s_nearestWinsDepth);
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &s_noCullRasterizer);
	D3D11_BLEND_DESC bl = {};
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (SUCCEEDED(hr)) hr = dev->CreateBlendState(&bl, &s_opaqueBlend);
	s_gpuObjectsReady = SUCCEEDED(hr);
	s_gpuObjectsFailed = !s_gpuObjectsReady;
	if (s_gpuObjectsFailed) Log("depth reprojection: unavailable (0x%08x); alternating mode works as before", (unsigned)hr);
	return s_gpuObjectsReady;
}

// Readable twin of the game's depth format, and the view format to sample its depth.
bool DepthFormats(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* srv) {
	switch (f) {
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24G8_TYPELESS:
			*tex = DXGI_FORMAT_R24G8_TYPELESS;
			*srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			return true;
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_TYPELESS:
			*tex = DXGI_FORMAT_R32_TYPELESS;
			*srv = DXGI_FORMAT_R32_FLOAT;
			return true;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32G8X24_TYPELESS:
			*tex = DXGI_FORMAT_R32G8X24_TYPELESS;
			*srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			return true;
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_TYPELESS:
			*tex = DXGI_FORMAT_R16_TYPELESS;
			*srv = DXGI_FORMAT_R16_UNORM;
			return true;
		default: return false;
	}
}
DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
	switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
	}
}

bool EnsureDepth(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
	if (s_eyes[0].depthCopy && s_depthCopyDesc.Width == src.Width && s_depthCopyDesc.Height == src.Height &&
		s_depthCopyDesc.Format == src.Format)
		return true;
	DXGI_FORMAT texFormat, srvFormat;
	if (!DepthFormats(src.Format, &texFormat, &srvFormat) || src.SampleDesc.Count != 1) return false;
	for (auto& e : s_eyes) {
		SafeRelease(e.depthCopySrv);
		SafeRelease(e.depthCopy);
		e.depthValid = false;
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = src.Width;
		d.Height = src.Height;
		d.MipLevels = d.ArraySize = 1;
		d.Format = texFormat;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
		v.Format = srvFormat;
		v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		v.Texture2D.MipLevels = 1;
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.depthCopy)) || FAILED(dev->CreateShaderResourceView(e.depthCopy, &v, &e.depthCopySrv))) {
			Log("depth reprojection: could not create a readable depth copy (format %d)", (int)src.Format);
			return false;
		}
	}
	s_depthCopyDesc = src;
	return true;
}

// The depth covers the same picture as the colour image (same size, or the colour enlarged).
bool SameShape(const D3D11_TEXTURE2D_DESC& depth, const D3D11_TEXTURE2D_DESC& color) {
	if (depth.Width > color.Width || depth.Height > color.Height || !depth.Height || !color.Height) return false;
	float depthAspect = (float)depth.Width / depth.Height, colorAspect = (float)color.Width / color.Height;
	return depthAspect > colorAspect * 0.99f && depthAspect < colorAspect * 1.01f;
}

bool EnsureColor(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
	if (s_warpedImage && s_sceneCopyDesc.Width == src.Width && s_sceneCopyDesc.Height == src.Height && s_sceneCopyDesc.Format == src.Format)
		return true;
	for (auto& e : s_eyes) {
		SafeRelease(e.sceneCopySrv);
		SafeRelease(e.sceneCopy);
		e.sceneValid = false;
	}
	SafeRelease(s_warpedImageRtv);
	SafeRelease(s_warpedImage);
	SafeRelease(s_warpedDepthDsv);
	SafeRelease(s_warpedDepth);
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = src.Width;
	d.Height = src.Height;
	d.MipLevels = d.ArraySize = 1;
	d.Format = src.Format;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
	v.Format = TypedFormat(src.Format);
	v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	v.Texture2D.MipLevels = 1;
	bool ok = true;
	for (auto& e : s_eyes)
		ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &e.sceneCopy)) &&
			 SUCCEEDED(dev->CreateShaderResourceView(e.sceneCopy, &v, &e.sceneCopySrv));
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	D3D11_RENDER_TARGET_VIEW_DESC rv = {};
	rv.Format = TypedFormat(src.Format);
	rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &s_warpedImage)) && SUCCEEDED(dev->CreateRenderTargetView(s_warpedImage, &rv, &s_warpedImageRtv));
	D3D11_TEXTURE2D_DESC dd = d;
	dd.Format = DXGI_FORMAT_D32_FLOAT;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ok = ok && SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &s_warpedDepth)) &&
		 SUCCEEDED(dev->CreateDepthStencilView(s_warpedDepth, nullptr, &s_warpedDepthDsv));
	if (!ok) {
		Log("depth reprojection: could not create its %ux%u textures", src.Width, src.Height);
		ReleaseTargets();
		return false;
	}
	s_sceneCopyDesc = src;
	return true;
}
}  // namespace

// From the OMSetRenderTargets hook (game thread): remember the game's full-size depth buffer (also
// for the upscaler, upscale.cpp).
void ReprojectNoteDsv(ID3D11DepthStencilView* dsv) {
	if (!((g_cfg.depthReproject && !g_cfg.synchronized) || UpscaleWantsDepth()) || !dsv || !s_backbufferW) return;
	ID3D11Resource* r = nullptr;
	dsv->GetResource(&r);
	if (!r) return;
	D3D11_RESOURCE_DIMENSION dim;
	r->GetType(&dim);
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC d;
		((ID3D11Texture2D*)r)->GetDesc(&d);
		if (d.Width == s_backbufferW && d.Height == s_backbufferH && d.SampleDesc.Count == 1) s_sceneDepth = r;
	}
	r->Release();  // raw pointer: only compared and copied from while the game still uses it
}

// At the first UI draw: the scene is finished; copy its depth for next frame's reprojection.
void ReprojectAtUiStart(ID3D11DeviceContext* c) {
	int eye = StereoFrameEye();
	if (!g_cfg.depthReproject || g_cfg.synchronized || eye < 0 || !s_sceneDepth) return;
	ID3D11Device* dev = nullptr;
	c->GetDevice(&dev);
	if (!dev) return;
	D3D11_TEXTURE2D_DESC d;
	((ID3D11Texture2D*)s_sceneDepth)->GetDesc(&d);
	if (EnsureDepth(dev, d)) {
		c->CopyResource(s_eyes[eye].depthCopy, s_sceneDepth);
		s_eyes[eye].depthValid = true;
		s_eyes[eye].depthPresentCount = s_presentCount;
	}
	dev->Release();
}

void ReprojectRelease() {
	s_sceneDepth = nullptr;
	ReleaseTargets();
}

ID3D11Resource* ReprojectSceneDepth() { return s_sceneDepth; }
void ReprojectSetBackbufferSize(UINT w, UINT h) {
	s_backbufferW = w;
	s_backbufferH = h;
}

// At Present (alternating), after this frame's eye was pushed to the headset: keep this eye's
// scene for next frame and return the other eye's previous image moved to this frame's camera.
// `scene` may be larger than the depth (upscaled, upscale.cpp): the depth is then read scaled.
ID3D11Texture2D* ReprojectOtherEye(ID3D11DeviceContext* c, ID3D11Texture2D* scene, ID3D11Texture2D* bb, FrameTag* tag) {
	// s_presentCount = Presents finished so far: the frame being presented copied its depth with
	// this value, so the other eye's depth from the frame before has s_presentCount - 1. It is only
	// counted up on return, so every check below sees the same value.
	struct CountPresentOnReturn {
		~CountPresentOnReturn() { s_presentCount++; }
	} countPresent;
	{
		D3D11_TEXTURE2D_DESC b;
		bb->GetDesc(&b);
		s_backbufferW = b.Width;
		s_backbufferH = b.Height;
	}
	int eye = StereoFrameEye();
	if (!g_cfg.depthReproject || g_cfg.synchronized || eye < 0) {
		s_eyes[0].sceneValid = s_eyes[1].sceneValid = false;
		return nullptr;
	}
	ID3D11Device* dev = nullptr;
	c->GetDevice(&dev);
	if (!dev) return nullptr;
	dev->Release();  // the context keeps it alive
	if (!CreateGpuObjects(dev)) return nullptr;
	ID3D11Texture2D* src = scene ? scene : bb;
	D3D11_TEXTURE2D_DESC sceneDesc;
	src->GetDesc(&sceneDesc);
	if (!EnsureColor(dev, sceneDesc)) return nullptr;
	int other = 1 - eye;
	float T[16], proj[4];
	ID3D11Texture2D* result = nullptr;
	EyeState& otherEye = s_eyes[other];
	int skip = !otherEye.sceneValid                               ? SKIP_NO_IMAGE
			   : !otherEye.depthValid                             ? SKIP_NO_DEPTH
			   : otherEye.depthPresentCount + 1 != s_presentCount ? SKIP_OLD_DEPTH
			   : !SameShape(s_depthCopyDesc, sceneDesc)           ? SKIP_SIZE_MISMATCH
			   : !StereoReprojectParams(other, T, proj, tag)      ? SKIP_NO_CAMERA
																  : -1;
	bool ready = skip < 0;
	if (skip >= 0) s_skipCounts[skip]++;
	if (ready) {
		struct {
			float T[16];
			float proj[4];
			float size[4];
			float edge[4];
			float dscale[4];
		} constants;
		static_assert(sizeof(constants) == 128, "cbuffer P");
		const float gridStep = 4;
		UINT gridW = (sceneDesc.Width + 3) / 4, gridH = (sceneDesc.Height + 3) / 4;
		memcpy(constants.T, T, 64);
		memcpy(constants.proj, proj, 16);
		constants.size[0] = (float)sceneDesc.Width;
		constants.size[1] = (float)sceneDesc.Height;
		constants.size[2] = gridStep;
		constants.size[3] = (float)gridW;
		// gaps: depth jumps by more than 3% inside a triangle that the motion stretched to more
		// than 2.5 grid steps (a normal triangle's longest side is 1.4 steps)
		constants.edge[0] = g_cfg.reprojectEdges ? 1.03f : 0.f; //TODO this has a strange borderline useless effect
		constants.edge[1] = 2.5f;
		constants.edge[2] = constants.edge[3] = 0;
		constants.dscale[0] = (float)s_depthCopyDesc.Width / (float)sceneDesc.Width;
		constants.dscale[1] = (float)s_depthCopyDesc.Height / (float)sceneDesc.Height;
		constants.dscale[2] = constants.dscale[3] = 0;
		g_replaying = true;  // our own calls: the hooks pass them straight through
		StateSaveTemp(c);
		c->UpdateSubresource(s_warpConstants, 0, nullptr, &constants, 0, 0);
		const FLOAT black[4] = {0, 0, 0, 1};
		c->ClearRenderTargetView(s_warpedImageRtv, black);
		c->ClearDepthStencilView(s_warpedDepthDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
		c->ClearState();
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c->VSSetShader(s_warpVs, nullptr, 0);
		c->PSSetShader(s_warpPs, nullptr, 0);
		c->VSSetConstantBuffers(0, 1, &s_warpConstants);
		ID3D11ShaderResourceView* srvs[2] = {otherEye.depthCopySrv, otherEye.sceneCopySrv};
		c->VSSetShaderResources(0, 2, srvs);
		c->PSSetShaderResources(0, 2, srvs);
		c->PSSetSamplers(0, 1, &s_linearSampler);
		D3D11_VIEWPORT vp = {0, 0, (float)sceneDesc.Width, (float)sceneDesc.Height, 0, 1};
		c->RSSetViewports(1, &vp);
		c->RSSetState(s_noCullRasterizer);
		c->OMSetDepthStencilState(s_nearestWinsDepth, 0);
		c->OMSetBlendState(s_opaqueBlend, nullptr, 0xffffffff);
		c->OMSetRenderTargets(1, &s_warpedImageRtv, s_warpedDepthDsv);
		c->Draw(gridW * gridH * 6, 0);
		StateRestoreTemp(c);
		g_replaying = false;
		result = s_warpedImage;
		s_warpedCount++;
		if (!s_loggedWorking) {
			s_loggedWorking = true;
			Log("depth reprojection: working (%ux%u, depth format %d, grid every %.0f pixels); the older eye now follows "
				"the camera",
				sceneDesc.Width, sceneDesc.Height, (int)s_depthCopyDesc.Format, (double)gridStep);
		}
	} else {
		s_skippedCount++;
	}
	// this eye's scene, for the next frame
	c->CopyResource(s_eyes[eye].sceneCopy, src);
	s_eyes[eye].sceneValid = true;
	if (GetTickCount() - s_lastReportTick > 30000) {
		s_lastReportTick = GetTickCount();
		if (s_warpedCount || s_skippedCount) {
			char reasonList[300];
			size_t at = 0;
			reasonList[0] = 0;
			for (int i = 0; i < SKIP_REASON_COUNT; i++)
				if (s_skipCounts[i]) at += snprintf(reasonList + at, sizeof(reasonList) - at, "%s%u x %s", at ? ", " : "", s_skipCounts[i], kSkipReasonText[i]);
			Log("depth reprojection: %u images moved to the new camera, %u frames without (30 s)%s%s", s_warpedCount, s_skippedCount,
				at ? ": " : "", reasonList);
		}
		memset(s_skipCounts, 0, sizeof(s_skipCounts));
		char info[160];
		snprintf(info, sizeof(info), "Depth reprojection: %u of %u frames in the last 30 s", s_warpedCount, s_warpedCount + s_skippedCount);
		MenuSetInfo(INFO_REPROJECT, info);
		s_warpedCount = s_skippedCount = 0;
	}
	return result;
}
