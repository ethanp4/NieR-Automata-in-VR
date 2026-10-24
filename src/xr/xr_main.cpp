// xr_main.cpp - the VR thread: OpenXR instance, system and session, swapchains, events, and the
// thread's loop. Each headset frame is in xr_frame.cpp, the thread's D3D11 device in xr_device.cpp.
//
// The VR thread runs at the headset's refresh rate, independent of the game's frame rate.
// Stereo: the game renders left and right eyes on alternate frames (camera.cpp); each headset
// frame submits the newest left and right images in a projection layer, each with the eye pose it
// was rendered from, and the runtime reprojects them to the current head pose.
// Flat: the newest game frame on a quad layer floating in front of the user (menus, F12, startup).
#include "xr/xr_main.h"
#include "xr/xr_thread.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "input/vr_controllers.h"
#include "render/camera.h"
#include "xr/runtime_loader.h"
#include <stdio.h>

VrState g_vr;
volatile DWORD g_xrThreadId;

// ---------------------------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------------------------
static const char* XrStr(XrResult r) {
	static char buf[XR_MAX_RESULT_STRING_SIZE + 16];
	if (g_vr.instance && g_vr.xrResultToString && XR_SUCCEEDED(g_vr.xrResultToString(g_vr.instance, r, buf))) return buf;
	snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
	return buf;
}

bool XrCheck(XrResult r, const char* what, bool perFrame) {
	if (XR_SUCCEEDED(r)) return true;
	if (!perFrame || g_vr.errorCount < 20 || (g_vr.errorCount % 1000) == 0)
		Log("OpenXR error: %s -> %s%s", what, XrStr(r), perFrame && g_vr.errorCount == 19 ? " (further per-frame errors throttled)" : "");
	if (perFrame) g_vr.errorCount++;
	if (r == XR_ERROR_SESSION_LOST) g_vr.sessionLost = true;
	if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_RUNTIME_FAILURE) g_vr.instanceLost = true;
	return false;
}
// ---------------------------------------------------------------------------------------------
// Instance & system
// ---------------------------------------------------------------------------------------------
static bool InitInstance() {
	g_vr.gipa = XrLoadEntry();
	if (!g_vr.gipa) return false;

	PFN_xrEnumerateInstanceExtensionProperties enumExt = nullptr;
	PFN_xrCreateInstance createInstance = nullptr;
	g_vr.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumExt);
	g_vr.gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
	if (!enumExt || !createInstance) {
		Log("ERROR: OpenXR: runtime is missing core functions");
		return false;
	}
	uint32_t n = 0;
	if (!XR_CHECK(enumExt(nullptr, 0, &n, nullptr))) return false;
	static XrExtensionProperties exts[256];
	if (n > 256) n = 256;
	for (uint32_t i = 0; i < n; i++) {
		exts[i] = {};
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	if (!XR_CHECK(enumExt(nullptr, n, &n, exts))) return false;
	bool hasD3D11 = false, hasPico = false;
	for (uint32_t i = 0; i < n; i++) {
		LogV("OpenXR extension: %s v%u", exts[i].extensionName, exts[i].extensionVersion);
		if (!lstrcmpA(exts[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
		if (!lstrcmpA(exts[i].extensionName, "XR_BD_controller_interaction")) hasPico = true;  // Pico 4 / Neo3 controllers
	}
	if (!hasD3D11) {
		Log("ERROR: OpenXR runtime does not support Direct3D 11 (XR_KHR_D3D11_enable)");
		return false;
	}

	const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, "XR_BD_controller_interaction"};
	XrInstanceCreateInfo ci = {};
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	lstrcpynA(ci.applicationInfo.applicationName, "NieRVR", XR_MAX_APPLICATION_NAME_SIZE);
	ci.applicationInfo.applicationVersion = 1;
	lstrcpynA(ci.applicationInfo.engineName, "NieRVR", XR_MAX_ENGINE_NAME_SIZE);
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = hasPico ? 2 : 1;
	ci.enabledExtensionNames = enabled;
	XrResult r = createInstance(&ci, &g_vr.instance);
	if (r == XR_ERROR_API_VERSION_UNSUPPORTED) {
		Log("OpenXR: runtime rejected API 1.0, retrying with 1.1");
		ci.applicationInfo.apiVersion = XR_API_VERSION_1_1;
		r = createInstance(&ci, &g_vr.instance);
	}
	if (!XrCheck(r, "xrCreateInstance")) {
		g_vr.instance = XR_NULL_HANDLE;
		return false;
	}

	bool ok = true;
#define LOAD_FN(n)                                                                          \
	if (XR_FAILED(g_vr.gipa(g_vr.instance, #n, (PFN_xrVoidFunction*)&g_vr.n)) || !g_vr.n) { \
		Log("ERROR: OpenXR: missing function " #n);                                         \
		ok = false;                                                                         \
	}
	XR_INSTANCE_FUNCS(LOAD_FN)
#undef LOAD_FN
	if (!ok) return false;
	VrInputInit(g_vr.gipa, g_vr.instance, hasPico);

	XrInstanceProperties ip = {};
	ip.type = XR_TYPE_INSTANCE_PROPERTIES;
	if (XR_SUCCEEDED(g_vr.xrGetInstanceProperties(g_vr.instance, &ip)))
		Log("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
			(unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));
	return true;
}

static bool InitSystem() {
	static bool loggedWaiting = false;
	XrSystemGetInfo gi = {};
	gi.type = XR_TYPE_SYSTEM_GET_INFO;
	gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult r = g_vr.xrGetSystem(g_vr.instance, &gi, &g_vr.system);
	if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
		if (!loggedWaiting) Log("OpenXR: headset not available yet (is it connected and awake?) - waiting");
		loggedWaiting = true;
		return false;
	}
	if (!XR_CHECK(r)) return false;
	loggedWaiting = false;

	XrSystemProperties sp = {};
	sp.type = XR_TYPE_SYSTEM_PROPERTIES;
	if (XR_SUCCEEDED(g_vr.xrGetSystemProperties(g_vr.instance, g_vr.system, &sp))) {
		Log("headset: %s (vendor 0x%x), max swapchain %ux%u, max layers %u, position tracking %d", sp.systemName,
			sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
			sp.graphicsProperties.maxLayerCount, (int)sp.trackingProperties.positionTracking);
		g_vr.maxSwapW = sp.graphicsProperties.maxSwapchainImageWidth;
		g_vr.maxSwapH = sp.graphicsProperties.maxSwapchainImageHeight;
	}

	// the mod renders a stereo pair: the system must offer the primary stereo view configuration
	{
		XrViewConfigurationType types[16];
		uint32_t nt = 0;
		bool stereo = false;
		if (XR_SUCCEEDED(g_vr.xrEnumerateViewConfigurations(g_vr.instance, g_vr.system, 16, &nt, types)))
			for (uint32_t i = 0; i < nt; i++) stereo |= types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (!stereo) {
			Log("ERROR: this OpenXR system has no stereo view configuration (%u others)", nt);
			return false;
		}
	}

	// the per-eye image size the runtime recommends (for the resolution hint)
	{
		XrViewConfigurationView vv[2] = {};
		vv[0].type = vv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		uint32_t nv = 0;
		if (XR_SUCCEEDED(g_vr.xrEnumerateViewConfigurationViews(g_vr.instance, g_vr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
																&nv, vv)) &&
			nv == 2) {
			g_vr.recW = vv[0].recommendedImageRectWidth;
			g_vr.recH = vv[0].recommendedImageRectHeight;
			Log("headset recommends %ux%u per eye (max %ux%u)", g_vr.recW, g_vr.recH, vv[0].maxImageRectWidth,
				vv[0].maxImageRectHeight);
		}
	}

	XrGraphicsRequirementsD3D11KHR req = {};
	req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
	if (!XR_CHECK(g_vr.xrGetD3D11GraphicsRequirementsKHR(g_vr.instance, g_vr.system, &req))) return false;
	g_vr.xrLuid = req.adapterLuid;
	g_vr.minFeatureLevel = req.minFeatureLevel;
	Log("headset GPU LUID %08x:%08x, min feature level 0x%x", (unsigned)req.adapterLuid.HighPart,
		(unsigned)req.adapterLuid.LowPart, (unsigned)req.minFeatureLevel);
	return true;
}

// After OpenXR tells us which GPU drives the headset: it must be the game's GPU.
static bool CheckHeadsetGpu() {
	if (g_vr.xrLuid.LowPart != g_vr.devLuid.LowPart || g_vr.xrLuid.HighPart != g_vr.devLuid.HighPart) {
		Log("ERROR: the headset is driven by a different GPU than the game. On laptops/multi-GPU systems, set "
			"NieRAutomata.exe to the high-performance GPU in Windows Settings > Display > Graphics.");
		return false;
	}
	if (g_vr.devLevel < g_vr.minFeatureLevel) {
		Log("ERROR: GPU feature level 0x%x is below the runtime's minimum 0x%x", (unsigned)g_vr.devLevel,
			(unsigned)g_vr.minFeatureLevel);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------
static void DestroySwap(Swapchain& s) {
	for (uint32_t i = 0; i < kMaxImages; i++) SafeRelease(s.rtv[i]);
	if (s.sc) g_vr.xrDestroySwapchain(s.sc);
	s.sc = XR_NULL_HANDLE;
	s.count = s.w = s.h = 0;
}

static void DestroyPanel(Panel& p) {
	DestroySwap(p.swap);
	SafeRelease(p.rtv);
	SafeRelease(p.tex);
	p.shown = false;
}

static void DestroyAllSwaps() {
	DestroySwap(g_vr.quadSwap);
	DestroySwap(g_vr.eyeSwap[0]);
	DestroySwap(g_vr.eyeSwap[1]);
	DestroySwap(g_vr.hudSwap);
	DestroyPanel(g_vr.menu);
	DestroyPanel(g_vr.overlay);
}

static void DestroySession() {
	VrInputDetach();
	DestroyAllSwaps();
	AcquireSRWLockExclusive(&g_stereo.lock);
	g_stereo.valid = false;
	ReleaseSRWLockExclusive(&g_stereo.lock);
	if (g_vr.local) g_vr.xrDestroySpace(g_vr.local);
	if (g_vr.view) g_vr.xrDestroySpace(g_vr.view);
	if (g_vr.session) g_vr.xrDestroySession(g_vr.session);
	g_vr.local = g_vr.view = XR_NULL_HANDLE;
	g_vr.session = XR_NULL_HANDLE;
	g_vr.running = false;
	g_vr.sessionLost = false;
	g_vr.placed = false;
	g_vr.state = XR_SESSION_STATE_UNKNOWN;
}

static void DestroyInstance() {
	DestroySession();
	VrInputShutdown();
	if (g_vr.instance) g_vr.xrDestroyInstance(g_vr.instance);
	g_vr.instance = XR_NULL_HANDLE;
	g_vr.system = XR_NULL_SYSTEM_ID;
	g_vr.instanceLost = false;
}

static bool InitSession() {
	XrGraphicsBindingD3D11KHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
	binding.device = g_vr.dev;
	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_vr.system;
	if (!XR_CHECK(g_vr.xrCreateSession(g_vr.instance, &ci, &g_vr.session))) {
		g_vr.session = XR_NULL_HANDLE;
		return false;
	}
	XrReferenceSpaceCreateInfo rs = {};
	rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	rs.poseInReferenceSpace.orientation.w = 1;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!XR_CHECK(g_vr.xrCreateReferenceSpace(g_vr.session, &rs, &g_vr.local))) return false;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!XR_CHECK(g_vr.xrCreateReferenceSpace(g_vr.session, &rs, &g_vr.view))) return false;
	VrInputAttach(g_vr.session);

	uint32_t n = 0;
	XrEnvironmentBlendMode modes[8];
	g_vr.blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	if (XR_SUCCEEDED(g_vr.xrEnumerateEnvironmentBlendModes(g_vr.instance, g_vr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
														   8, &n, modes)) &&
		n > 0)
		g_vr.blend = modes[0];

	// Pick a swapchain format: prefer sRGB so the runtime displays the game's colours exactly.
	int64_t formats[64];
	n = 0;
	if (!XR_CHECK(g_vr.xrEnumerateSwapchainFormats(g_vr.session, 64, &n, formats))) return false;
	const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
								 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
	g_vr.swapFormat = 0;
	for (int64_t p : preferred) {
		for (uint32_t i = 0; i < n && !g_vr.swapFormat; i++)
			if (formats[i] == p) g_vr.swapFormat = p;
		if (g_vr.swapFormat) break;
	}
	if (!g_vr.swapFormat) {
		Log("ERROR: runtime offers no usable 8-bit swapchain format (%u formats)", n);
		return false;
	}
	Log("OpenXR session created (blend mode %d, swapchain format %d)", (int)g_vr.blend, (int)g_vr.swapFormat);
	g_vr.errorCount = 0;
	return true;
}

bool EnsureSwap(Swapchain& s, uint32_t w, uint32_t h, const char* what, bool copyTarget) {
	// Respect runtime limits, keep aspect ratio
	uint32_t maxW = g_vr.maxSwapW ? g_vr.maxSwapW : 4096, maxH = g_vr.maxSwapH ? g_vr.maxSwapH : 4096;
	if (w > maxW) {
		h = (uint32_t)((uint64_t)h * maxW / w);
		w = maxW;
	}
	if (h > maxH) {
		w = (uint32_t)((uint64_t)w * maxH / h);
		h = maxH;
	}
	if (s.sc && s.w == w && s.h == h) return true;
	if ((LONG)(GetTickCount() - s.retryAt) < 0) return false;
	s.retryAt = GetTickCount() + 2000;  // after a failure, retry at most every 2 s
	DestroySwap(s);

	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
					(copyTarget ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0);
	ci.format = g_vr.swapFormat;
	ci.sampleCount = 1;
	ci.width = w;
	ci.height = h;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	if (!XR_CHECK(g_vr.xrCreateSwapchain(g_vr.session, &ci, &s.sc))) {
		s.sc = XR_NULL_HANDLE;
		return false;
	}
	XrSwapchainImageD3D11KHR images[kMaxImages];
	for (uint32_t i = 0; i < kMaxImages; i++) {
		images[i] = {};
		images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
	}
	uint32_t n = 0;
	if (!XR_CHECK(g_vr.xrEnumerateSwapchainImages(s.sc, kMaxImages, &n, (XrSwapchainImageBaseHeader*)images))) {
		DestroySwap(s);
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = (DXGI_FORMAT)g_vr.swapFormat;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		HRESULT hr = g_vr.dev->CreateRenderTargetView(images[i].texture, &rd, &s.rtv[i]);
		if (FAILED(hr)) {
			Log("ERROR: CreateRenderTargetView on swapchain image %u failed 0x%08x", i, (unsigned)hr);
			DestroySwap(s);
			return false;
		}
	}
	s.count = n;
	s.w = w;
	s.h = h;
	Log("%s swapchain %ux%u, %u images", what, w, h, n);
	return true;
}

bool RenderInto(Swapchain& s, ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* scene, const WarpParams* warp) {
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {};
	ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrAcquireSwapchainImage(s.sc, &ai, &idx))) return false;
	XrSwapchainImageWaitInfo wi = {};
	wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	wi.timeout = XR_INFINITE_DURATION;
	bool waited = XR_CHECK_FRAME(g_vr.xrWaitSwapchainImage(s.sc, &wi));
	if (!waited) return false;  // spec: an image may only be released after a successful wait
	if (idx < s.count) Blit(s.rtv[idx], s.w, s.h, srv, scene, IsSrgb(g_vr.swapFormat), warp);
	XrSwapchainImageReleaseInfo ri = {};
	ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK_FRAME(g_vr.xrReleaseSwapchainImage(s.sc, &ri));
	return true;
}

// ---------------------------------------------------------------------------------------------
// Events & frame loop
// ---------------------------------------------------------------------------------------------
static const char* StateName(XrSessionState s) {
	switch (s) {
		case XR_SESSION_STATE_IDLE: return "IDLE";
		case XR_SESSION_STATE_READY: return "READY";
		case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
		case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
		case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
		case XR_SESSION_STATE_STOPPING: return "STOPPING";
		case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
		case XR_SESSION_STATE_EXITING: return "EXITING";
		default: return "UNKNOWN";
	}
}

static void PollEvents() {
	XrEventDataBuffer ev;
	while (true) {
		// the runtime overwrites the type with the event's own: it must be reset before every call
		// (0.5.0; earlier versions passed the previous event's type back in)
		ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		if (g_vr.xrPollEvent(g_vr.instance, &ev) != XR_SUCCESS) break;
		switch (ev.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
				g_vr.state = e->state;
				Log("session state -> %s", StateName(e->state));
				if (e->state == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo bi = {};
					bi.type = XR_TYPE_SESSION_BEGIN_INFO;
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (XR_CHECK(g_vr.xrBeginSession(g_vr.session, &bi))) g_vr.running = true;
				} else if (e->state == XR_SESSION_STATE_STOPPING) {
					XR_CHECK(g_vr.xrEndSession(g_vr.session));
					g_vr.running = false;
				} else if (e->state == XR_SESSION_STATE_EXITING) {
					Log("the VR runtime closed our session; restart the game to re-enter VR");
					g_vr.exitRequested = true;
					g_vr.sessionLost = true;
				} else if (e->state == XR_SESSION_STATE_LOSS_PENDING) {
					g_vr.sessionLost = true;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				Log("OpenXR instance loss pending (runtime shutting down?)");
				g_vr.instanceLost = true;
				break;
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				const XrEventDataReferenceSpaceChangePending* e = (const XrEventDataReferenceSpaceChangePending*)&ev;
				if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
					Log("runtime recentered the view; re-placing the screen");
					g_vr.recenterRequested = true;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_EVENTS_LOST:
				Log("OpenXR: events lost");
				VrInputProfileChanged();  // one of them may have been a controller change
				break;
			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
				VrInputProfileChanged();
				break;
			default:
				break;
		}
		ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

static bool TimeReached(DWORD at) { return (LONG)(GetTickCount() - at) >= 0; }

static DWORD WINAPI XrThreadMain(void*) {
	g_xrThreadId = GetCurrentThreadId();
	InstallCrashHandler();  // in case the game replaced it during startup
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	QueryPerformanceFrequency(&g_vr.qpcFreq);
	QueryPerformanceCounter(&g_vr.statStart);
	Log("VR thread started");
	g_stereo.enabled = g_cfg.stereo != 0;
	Log("stereo is %s at start (toggle key 0x%x)", g_stereo.enabled ? "ON" : "OFF", (unsigned)g_cfg.keyStereo);

	if (!InitDevice()) {
		Log("VR disabled for this session (see errors above). The game keeps running normally.");
		return 0;
	}

	// Every wait in this loop is short so the capture texture is always served promptly.
	DWORD instanceRetryAt = 0, systemRetryAt = 0, sessionRetryAt = 0, instanceRetryDelay = 10000;
	bool xrDisabled = false;
	while (true) {
		ServiceShare();
		if (xrDisabled) {
			Sleep(50);
			continue;
		}
		if (!g_vr.instance) {
			if (!TimeReached(instanceRetryAt)) {
				if (KeyPressed(g_cfg.keyRecenter) && GameIsForeground()) {
					Log("retrying OpenXR now (recenter key)");
					instanceRetryAt = GetTickCount();
				}
				Sleep(50);
				continue;
			}
			if (!InitInstance()) {
				if (g_vr.instance) DestroyInstance();
				// Back off (10 s, 20 s, 40 s ... up to 5 min): each attempt may (re)start the VR
				// runtime, which is unwelcome when playing without a headset. F8 retries at once.
				DWORD wait = instanceRetryDelay;
				instanceRetryDelay = instanceRetryDelay >= 150000 ? 300000 : instanceRetryDelay * 2;
				Log("OpenXR not available - retrying in %u s (or press the recenter key to retry now)",
					(unsigned)(wait / 1000));
				instanceRetryAt = GetTickCount() + wait;
				continue;
			}
			instanceRetryDelay = 10000;
		}
		if (g_vr.system == XR_NULL_SYSTEM_ID) {
			if (!TimeReached(systemRetryAt)) {
				Sleep(50);
				continue;
			}
			if (!InitSystem()) {
				g_vr.system = XR_NULL_SYSTEM_ID;
				systemRetryAt = GetTickCount() + 2000;
				continue;
			}
			if (!CheckHeadsetGpu()) {
				xrDisabled = true;
				Log("VR disabled for this session. The game keeps running normally.");
				continue;
			}
		}
		if (!g_vr.session) {
			if (g_vr.exitRequested || !TimeReached(sessionRetryAt)) {
				Sleep(50);
				continue;
			}
			if (!InitSession()) {
				DestroySession();
				if (g_vr.instanceLost) DestroyInstance();
				sessionRetryAt = GetTickCount() + 3000;
				continue;
			}
		}

		PollEvents();
		if (g_vr.instanceLost) {
			DestroyInstance();
			instanceRetryAt = GetTickCount() + 3000;
			continue;
		}
		if (g_vr.sessionLost) {
			DestroySession();
			// spec: after a lost session the system may have changed (headset unplugged, another one):
			// get it (and its graphics requirements) again before creating a new session
			g_vr.system = XR_NULL_SYSTEM_ID;
			sessionRetryAt = GetTickCount() + 1000;
			systemRetryAt = GetTickCount() + 1000;
			continue;
		}
		if (!g_vr.running) {
			Sleep(10);
			continue;
		}
		Frame();
	}
}

void XrStartThread() {
	static volatile LONG started = 0;
	if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
	HANDLE t = CreateThread(nullptr, 0, XrThreadMain, nullptr, 0, nullptr);
	if (t) CloseHandle(t);
	else Log("ERROR: could not create VR thread (%u)", (unsigned)GetLastError());
}
