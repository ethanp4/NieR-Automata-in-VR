// frame_handoff.cpp - hooks IDXGISwapChain::Present (inline detour via MinHook) and copies each
// finished game frame into a texture shared with the VR thread. The game is never made to wait.
#include "render/frame_handoff.h"
#include "core/config.h"
#include "core/d3d11_proxy.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/main.h"
#include "debug/frame_capture.h"
#include "debug/gpu_timing.h"
#include "render/camera.h"
#include "render/d3d_hooks.h"
#include "render/extra_resolutions.h"
#include "render/frame_share.h"
#include "render/game_effects.h"
#include "render/hud_layer.h"
#include "render/replay.h"
#include "render/reproject.h"
#include "render/upscale.h"
#include "xr/xr_main.h"
#include <string.h>

FrameShare g_share;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
static const int kPresentIndex = 8;  // IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) -> Present
static const int kMaxTargets = 2;

static PFN_Present g_origPresent[kMaxTargets];
static void* g_hookTarget[kMaxTargets];
static volatile LONG g_hookCount;
static SRWLOCK g_hookLock = SRWLOCK_INIT;
static DWORD g_tlsDepth = TLS_OUT_OF_INDEXES;  // per-thread Present nesting depth

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static const int kResizeBuffersIndex = 13;
static PFN_ResizeBuffers g_origResizeBuffers;
static volatile LONG g_resizeLogBudget;

// ---------------------------------------------------------------------------------------------
// Game-side capture state (only touched at nesting depth 0 on the presenting thread)
//
// The VR thread owns a ring of 4 shared textures. Each frame the backbuffer is copied into the
// next ring slot and an event query is issued behind the copy; once the GPU has finished it
// (polled without blocking), the frame is published with its eye/pose tag. The game device
// cannot use keyed mutexes (DXGI 1.0 factory), so the query is the synchronisation: a slot is
// only rewritten 4 frames later, long after the VR thread has copied it.
// ---------------------------------------------------------------------------------------------
static const int kRing = kRingSize;
static ID3D11Device* s_dev;
static ID3D11DeviceContext* s_ctx;
static ID3D11Texture2D* s_ring[kRing];
static ID3D11Texture2D* s_ringScene[kRing];
static ID3D11Texture2D* s_ringKey[kRing];  // upscaling: the scene image before upscaling, for the HUD key
static ID3D11Query* s_query[kRing];
static bool s_pending[kRing];
static FrameTag s_pendingTag[kRing];
static LONG s_openedGen = -1;
static int s_nextSlot;
static UINT s_reqW, s_reqH;
static UINT s_reqSceneW, s_reqSceneH;  // scene-only images: enlarged when upscaling (upscale.cpp)
static bool s_reqKey;                  // upscaling: the scene images before upscaling too
static DXGI_FORMAT s_reqFmt;
static bool s_loggedNotD3D11, s_loggedOtherDevice;
static unsigned s_presentCount;
static volatile LONG s_budgetOpen, s_budgetNested, s_budgetCopy;

static DXGI_FORMAT ConcreteFormat(DXGI_FORMAT f) {
	switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
	}
}

static void ReleaseRing() {
	for (int i = 0; i < kRing; i++) {
		if (s_ring[i]) s_ring[i]->Release();
		if (s_ringScene[i]) s_ringScene[i]->Release();
		if (s_ringKey[i]) s_ringKey[i]->Release();
		s_ringKey[i] = nullptr;
		if (s_query[i]) s_query[i]->Release();
		s_ring[i] = nullptr;
		s_ringScene[i] = nullptr;
		s_query[i] = nullptr;
		s_pending[i] = false;
	}
}

static void PublishLocked(int i) {
	const FrameTag& t = s_pendingTag[i];
	int kind = (t.eye >= 0 && t.patched) ? t.eye : 2;
	if (g_share.gen == s_openedGen) {
		g_share.latest[kind].seq = ++g_share.seq;
		g_share.latest[kind].slot = i;
		g_share.latest[kind].tag = t;
	}
}

// Publishes ring slots whose copy the GPU has finished. Never blocks. The two images of a Synchronized
// frame (same pairId) are published together, once both are done, so the headset never shows the
// left eye of one game frame with the right eye of another (0.3.26: the right image is finished
// a replay later than the left one, and the right eye lagged).
static void PollPending() {
	bool done[kRing] = {};
	for (int i = 0; i < kRing; i++)
		done[i] = s_pending[i] && s_query[i] &&
				  s_ctx->GetData(s_query[i], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
	EnterCriticalSection(&g_share.cs);
	for (int i = 0; i < kRing; i++) {
		if (!done[i] || !s_pending[i]) continue;
		unsigned pair = s_pendingTag[i].pairId;
		if (!pair) {
			s_pending[i] = false;
			PublishLocked(i);
			continue;
		}
		int j = -1;
		for (int k = 0; k < kRing; k++)
			if (k != i && s_pending[k] && s_pendingTag[k].pairId == pair) j = k;
		if (j < 0) {  // partner already gone (shouldn't happen): publish alone
			s_pending[i] = false;
			PublishLocked(i);
		} else if (done[j]) {
			int a = s_pendingTag[i].eye == 0 ? i : j, b = a == i ? j : i;
			s_pending[a] = s_pending[b] = false;
			PublishLocked(a);
			PublishLocked(b);
		}
	}
	LeaveCriticalSection(&g_share.cs);
}

// From the game's draws (render thread, every 128 draws): frames finished by the GPU are handed to
// the headset right away instead of at the next Present (up to a game frame less latency).
void CapturePollMidFrame() {
	static unsigned n;
	if (++n % 128 || !s_ring[0] || !s_ctx) return;
	PollPending();
}

static int FreeSlots() {
	int n = 0;
	for (int i = 0; i < kRing; i++) n += !s_pending[i];
	return n;
}

// One-time probe of what the game's device supports (logged for diagnostics).
static void ProbeGameDevice(ID3D11Device* dev) {
	Log("game device creation flags 0x%x", dev->GetCreationFlags());
	struct {
		const char* name;
		UINT bind, misc;
	} tests[] = {
		{"plain", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0},
		{"shared", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_RESOURCE_MISC_SHARED},
		{"shared+keyedmutex", D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX},
	};
	for (auto& t : tests) {
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = 64;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = t.bind;
		d.MiscFlags = t.misc;
		ID3D11Texture2D* tex = nullptr;
		HRESULT hr = dev->CreateTexture2D(&d, nullptr, &tex);
		Log("game device probe: %s texture -> 0x%08x", t.name, (unsigned)hr);
		if (tex) tex->Release();
	}
}

static void OnFirstGameDevice(ID3D11Device* dev) {
	s_dev = dev;  // keeps the reference from GetDevice
	s_dev->GetImmediateContext(&s_ctx);
	IDXGIDevice* dxgiDev = nullptr;
	if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
		IDXGIAdapter* adapter = nullptr;
		if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
			DXGI_ADAPTER_DESC ad;
			if (SUCCEEDED(adapter->GetDesc(&ad))) {
				g_share.gameLuid = ad.AdapterLuid;
				g_share.gameLuidValid = true;
				Log("game GPU: %s (LUID %08x:%08x, %u MB VRAM)", Utf8(ad.Description).s, (unsigned)ad.AdapterLuid.HighPart,
					(unsigned)ad.AdapterLuid.LowPart, (unsigned)(ad.DedicatedVideoMemory >> 20));
			}
			IDXGIFactory1* f1 = nullptr;
			HRESULT hr = adapter->GetParent(__uuidof(IDXGIFactory1), (void**)&f1);
			Log("game adapter's factory is DXGI %s", SUCCEEDED(hr) ? "1.1+" : "1.0");
			if (f1) f1->Release();
			adapter->Release();
		}
		dxgiDev->Release();
	}
	Log("game device feature level 0x%x", (unsigned)dev->GetFeatureLevel());
	D3dHooksFollowDevice(dev);
	ProbeGameDevice(dev);
	Log("starting VR thread");
	XrStartThread();
}

// Copies a finished image (and the scene-only image, if any) into the next free ring slot, tagged
// with the eye the stereo code says was just drawn. full = null: no full image (a reprojected eye
// while upscaling: only its scene image is shown). key: upscaling, the scene image before upscaling.
static int PushToRing(ID3D11Texture2D* full, bool msaa, DXGI_FORMAT fmt, ID3D11Texture2D* scene, bool noUI,
					  const FrameTag* tagOverride = nullptr, ID3D11Texture2D* key = nullptr) {
	int slot = -1;
	for (int k = 0; k < kRing && slot < 0; k++)
		if (!s_pending[(s_nextSlot + k) % kRing]) slot = (s_nextSlot + k) % kRing;
	if (slot < 0) {
		if (LogBudget(&s_budgetCopy, 3)) Log("note: capture ring full (GPU behind), skipped a frame");
		return -1;
	}
	if (full && msaa)
		s_ctx->ResolveSubresource(s_ring[slot], 0, full, 0, fmt);
	else if (full)
		s_ctx->CopyResource(s_ring[slot], full);
	if (scene) s_ctx->CopyResource(s_ringScene[slot], scene);
	if (scene && key && s_ringKey[slot]) s_ctx->CopyResource(s_ringKey[slot], key);
	s_ctx->End(s_query[slot]);
	if (tagOverride)
		s_pendingTag[slot] = *tagOverride;
	else
		StereoGetFrameTag(&s_pendingTag[slot]);
	s_pendingTag[slot].hasScene = scene != nullptr;
	s_pendingTag[slot].noUI = noUI;
	s_pendingTag[slot].pairId = 0;
	s_pending[slot] = true;
	s_nextSlot = (slot + 1) % kRing;
	return slot;
}
static unsigned s_pairCounter;
static unsigned s_droppedPairs;
static DWORD s_dropReportTick;

static void CaptureFrame(IDXGISwapChain* sc) {
	ID3D11Device* dev = nullptr;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) {
		if (!s_loggedNotD3D11) Log("note: a non-D3D11 swapchain presented (ignored)");
		s_loggedNotD3D11 = true;
		return;
	}
	if (!s_dev) {
		OnFirstGameDevice(dev);
	} else {
		dev->Release();
		if (dev != s_dev) {
			if (!s_loggedOtherDevice) Log("note: Present from a second D3D11 device (overlay?) ignored");
			s_loggedOtherDevice = true;
			return;
		}
	}

	ID3D11Texture2D* bb = nullptr;
	if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
	D3D11_TEXTURE2D_DESC d;
	bb->GetDesc(&d);
	DXGI_FORMAT fmt = ConcreteFormat(d.Format);
	if (s_presentCount++ == 0) {
		Log("first Present: backbuffer %ux%u format %d msaa %u", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
		ExtraResolutionsOnFirstPresent(sc);
	}
	StereoSetAspect((float)d.Width / (float)d.Height);
	UiSetBackbuffer(bb);
	ReprojectSetBackbufferSize(d.Width, d.Height);
	UINT sw, sh;  // size of this frame's scene-only images
	// upscaling (also DLAA, which keeps the size): the scene images are DLSS's
	const bool up = UpscaleBeginPresent(s_ctx, d.Width, d.Height, &sw, &sh);

	// 1. Ask the VR thread for a ring matching the backbuffer (only when it changes).
	if (d.Width != s_reqW || d.Height != s_reqH || fmt != s_reqFmt || sw != s_reqSceneW || sh != s_reqSceneH ||
		up != s_reqKey) {
		s_reqW = d.Width;
		s_reqH = d.Height;
		s_reqSceneW = sw;
		s_reqSceneH = sh;
		s_reqKey = up;
		s_reqFmt = fmt;
		ReleaseRing();
		EnterCriticalSection(&g_share.cs);
		g_share.reqW = d.Width;
		g_share.reqH = d.Height;
		g_share.reqSceneW = sw;
		g_share.reqSceneH = sh;
		g_share.reqKey = up;
		g_share.reqFmt = fmt;
		g_share.reqGen++;
		LeaveCriticalSection(&g_share.cs);
		if (up)
			Log("requesting %ux%u format %d capture textures, scene images upscaled to %ux%u", d.Width, d.Height, (int)fmt,
				sw, sh);
		else
			Log("requesting %ux%u format %d capture textures", d.Width, d.Height, (int)fmt);
	}

	// 2. Open the ring once the VR thread has published it.
	EnterCriticalSection(&g_share.cs);
	HANDLE handles[kRing], sceneHandles[kRing], keyHandles[kRing];
	memcpy(handles, g_share.ring, sizeof(handles));
	memcpy(sceneHandles, g_share.ringScene, sizeof(sceneHandles));
	memcpy(keyHandles, g_share.ringKey, sizeof(keyHandles));
	LONG gen = g_share.gen;
	bool current = g_share.forReqGen == g_share.reqGen;
	LeaveCriticalSection(&g_share.cs);
	if (handles[0] && current && gen != s_openedGen) {
		ReleaseRing();
		s_openedGen = gen;  // one attempt per published ring
		HRESULT hr = S_OK;
		D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
		for (int i = 0; i < kRing && SUCCEEDED(hr); i++) {
			hr = s_dev->OpenSharedResource(handles[i], __uuidof(ID3D11Texture2D), (void**)&s_ring[i]);
			if (SUCCEEDED(hr))
				hr = s_dev->OpenSharedResource(sceneHandles[i], __uuidof(ID3D11Texture2D), (void**)&s_ringScene[i]);
			if (SUCCEEDED(hr) && keyHandles[i])
				hr = s_dev->OpenSharedResource(keyHandles[i], __uuidof(ID3D11Texture2D), (void**)&s_ringKey[i]);
			if (SUCCEEDED(hr)) hr = s_dev->CreateQuery(&qd, &s_query[i]);
		}
		if (FAILED(hr)) {
			if (LogBudget(&s_budgetOpen, 5))
				Log("ERROR: game device could not open the capture textures (0x%08x)", (unsigned)hr);
			ReleaseRing();
		} else {
			Log("game side: capture ring opened (generation %d)", (int)gen);
		}
	}

	// 3. Publish finished copies, then copy this frame into the next free slot.
	if (s_ring[0]) {
		PollPending();
		ID3D11Texture2D* scene = UiSceneTexture();
		bool synchronized = StereoSynchronizedFrame() && ReplayAvailable();
		if (synchronized && FreeSlots() < 2) {
			// Synchronized and the GPU is behind: drop this game frame for the headset (both eyes keep
			// the previous pair) and skip the replay, which lets the GPU catch up.
			s_droppedPairs++;
		} else if (!synchronized && g_cfg.depthReproject && StereoFrameEye() >= 0 && d.SampleDesc.Count == 1 && FreeSlots() >= 2 &&
				   (!up || scene)) {
			// Alternating with depth reprojection: this frame's eye, plus the other eye's last
			// image moved to this frame's camera, handed to the headset as a pair. Upscaling: the
			// upscaled images are moved.
			ID3D11Texture2D* sc = UpscaleScene(s_ctx, StereoFrameEye(), scene);
			int a = PushToRing(bb, false, fmt, sc, false, nullptr, scene);
			FrameTag tag;
			ID3D11Texture2D* warped = a >= 0 && (!up || sc) ? ReprojectOtherEye(s_ctx, sc, bb, &tag) : nullptr;
			int b = warped ? PushToRing(up ? nullptr : warped, false, fmt, warped, true, &tag) : -1;
			if (b >= 0) {
				unsigned id = ++s_pairCounter ? s_pairCounter : ++s_pairCounter;
				s_pendingTag[a].pairId = s_pendingTag[b].pairId = id;
			}
		} else {
			int left = PushToRing(bb, d.SampleDesc.Count > 1, fmt, UpscaleScene(s_ctx, StereoFrameEye(), scene), false,
								  nullptr, scene);
			// Synchronized: the same frame again for the right eye (replay.cpp), into the next slot.
			if (synchronized && left >= 0) {
				bool inBackbuffer = false;
				int right = -1;
				if (ReplaySecondEye(s_ctx, &inBackbuffer)) {
					if (inBackbuffer)
						right = PushToRing(bb, d.SampleDesc.Count > 1, fmt, nullptr, false);
					else if (scene) {
						UpscaleCaptureDepth(s_ctx, 1);  // the replay left the right eye's depth behind
						right = PushToRing(scene, false, fmt, UpscaleScene(s_ctx, 1, scene), true);
					}
				}
				if (right >= 0) {
					unsigned id = ++s_pairCounter ? s_pairCounter : ++s_pairCounter;
					s_pendingTag[left].pairId = s_pendingTag[right].pairId = id;
				}
			}
		}
		if (s_droppedPairs && GetTickCount() - s_dropReportTick > 30000) {
			s_dropReportTick = GetTickCount();
			Log("both eyes: %u game frames not shown in the last 30 s (GPU behind: lower the game's resolution)",
				s_droppedPairs);
			s_droppedPairs = 0;
		}
	}
	bb->Release();
}

// Right after the game's Present: the copy has been submitted with it.
static void AfterPresent() {
	FrameLatencyTick();
	if (s_ring[0]) PollPending();
	UiBeginFrame();
	GameEffectsBeginFrame();
	StereoBeginFrame();
	if (s_ctx) {
		ReplayFrameStart(s_ctx, StereoSynchronizedFrame());
		GpuTimingPoll(s_ctx);
		GpuTimingFrameStart(s_ctx);
	}
	ReplayReport();
}

static HRESULT PresentCommon(int slot, void* retAddr, IDXGISwapChain* self, UINT syncInterval, UINT flags) {
	size_t depth = (size_t)TlsGetValue(g_tlsDepth);
	TlsSetValue(g_tlsDepth, (void*)(depth + 1));
	if (depth == 0) {
		static bool guaranteed = false;
		if (!guaranteed) {
			// Leave stack room for crash reporting on the render thread.
			ULONG size = 64 * 1024;
			SetThreadStackGuarantee(&size);
			guaranteed = true;
		}
		if (!(flags & DXGI_PRESENT_TEST)) {
			ReplayStop(false);  // Synchronized: the game's frame is complete
			if (s_ctx) GpuTimingMark(s_ctx, "present");
			CaptureBeforePresent();
			CaptureFrame(self);
			if (s_ctx) GpuTimingFrameEnd(s_ctx);
		}
	} else {
		if (LogBudget(&s_budgetNested, 5)) {
			// Present re-entered on the same thread (some overlay/wrapper calls it from inside Present).
			char who[300];
			DescribeAddress(retAddr, who, sizeof(who));
			Log("note: nested Present (depth %u, hook %d) called from %s - not capturing", (unsigned)depth, slot, who);
		}
		if (depth >= 32) {
			// Runaway recursion (the 0.1.0 crash). Break the loop instead of overflowing the stack.
			static bool logged = false;
			if (!logged) Log("ERROR: Present recursion depth %u - breaking the loop", (unsigned)depth);
			logged = true;
			TlsSetValue(g_tlsDepth, (void*)depth);
			return S_OK;
		}
	}
	HRESULT hr = g_origPresent[slot](self, syncInterval, flags);
	if (depth == 0 && !(flags & DXGI_PRESENT_TEST)) {
		AfterPresent();
		CaptureAfterPresent();
	}
	TlsSetValue(g_tlsDepth, (void*)depth);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent0(IDXGISwapChain* self, UINT s, UINT f) {
	return PresentCommon(0, __builtin_return_address(0), self, s, f);
}
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain* self, UINT s, UINT f) {
	return PresentCommon(1, __builtin_return_address(0), self, s, f);
}

static void InstallDetour(void* target, const char* how) {
	char where[300];
	DescribeAddress(target, where, sizeof(where));
	AcquireSRWLockExclusive(&g_hookLock);
	for (LONG i = 0; i < g_hookCount; i++)
		if (g_hookTarget[i] == target) {
			ReleaseSRWLockExclusive(&g_hookLock);
			return;
		}
	if (target == (void*)&HookedPresent0 || target == (void*)&HookedPresent1) {
		ReleaseSRWLockExclusive(&g_hookLock);
		return;
	}
	if (g_hookCount >= kMaxTargets) {
		Log("ERROR: more Present implementations than expected (%s)", where);
		ReleaseSRWLockExclusive(&g_hookLock);
		return;
	}
	if (g_tlsDepth == TLS_OUT_OF_INDEXES) g_tlsDepth = TlsAlloc();
	LONG n = g_hookCount;
	void* detour = n == 0 ? (void*)&HookedPresent0 : (void*)&HookedPresent1;
	if (!HookFunction(target, detour, (void**)&g_origPresent[n], "Present")) {
		ReleaseSRWLockExclusive(&g_hookLock);
		return;
	}
	g_hookTarget[n] = target;
	g_hookCount = n + 1;
	Log("Present hooked (%s) at %s", how, where);
	ReleaseSRWLockExclusive(&g_hookLock);
}

// The backbuffer changes size: every feature frees its textures of the old size first.
static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
													 UINT flags) {
	UiOnResizeBuffers();
	ReplayRelease();
	ReprojectRelease();
	UpscaleRelease();
	HRESULT hr = g_origResizeBuffers(sc, count, w, h, format, flags);
	if (LogBudget(&g_resizeLogBudget, 8))
		Log("swapchain: ResizeBuffers %ux%u format %d -> 0x%08x", w, h, (int)format, (unsigned)hr);
	return hr;
}

void HookFromSwapChain(IDXGISwapChain* sc) { InstallDetour((*(void***)sc)[kPresentIndex], "game swapchain"); }

void HookWithDummySwapChain() {
	static volatile LONG done = 0;
	if (InterlockedCompareExchange(&done, 1, 0) != 0) return;

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = g_hSelf;
	wc.lpszClassName = L"NieRVR_Dummy";
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_hSelf,
								nullptr);
	if (!hwnd) {
		Log("ERROR: dummy window failed (%u)", (unsigned)GetLastError());
		return;
	}
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferDesc.Width = 64;
	sd.BufferDesc.Height = 64;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 1;
	sd.OutputWindow = hwnd;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	IDXGISwapChain* sc = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	HRESULT hr = E_FAIL;
	const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
	for (D3D_DRIVER_TYPE t : types) {
		hr = g_realCreateDeviceAndSwapChain(nullptr, t, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev,
											nullptr, &ctx);
		if (SUCCEEDED(hr)) break;
	}
	if (SUCCEEDED(hr) && sc) {
		InstallDetour((*(void***)sc)[kPresentIndex], "via dummy swapchain");
		HookFunction((*(void***)sc)[kResizeBuffersIndex], (void*)&HookedResizeBuffers, (void**)&g_origResizeBuffers,
					 "IDXGISwapChain::ResizeBuffers");
		ExtraResolutionsHookSwapChain(sc);
		sc->Release();
		if (ctx) ctx->Release();
		if (dev) dev->Release();
	} else {
		Log("ERROR: dummy swapchain creation failed 0x%08x - VR will not start", (unsigned)hr);
	}
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, g_hSelf);
}
