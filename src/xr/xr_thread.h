// xr_thread.h - state shared by the VR thread's files: xr_main.cpp (OpenXR setup, session,
// swapchains, the thread's loop), xr_device.cpp (its D3D11 device, the game's frames, menu detection)
// and xr_frame.cpp (each headset frame). Only the VR thread uses any of it.
#pragma once
#include "xr/xr.h"

#define XR_INSTANCE_FUNCS(X)                                                                                       \
	X(xrGetInstanceProperties)                                                                                     \
	X(xrGetSystem)                                                                                                 \
	X(xrGetSystemProperties) X(xrDestroyInstance)                                                                  \
		X(xrResultToString) X(xrPollEvent) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession)                \
			X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrCreateReferenceSpace)                 \
				X(xrDestroySpace) X(xrLocateSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)             \
					X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage)                 \
						X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrEnumerateEnvironmentBlendModes)     \
							X(xrGetD3D11GraphicsRequirementsKHR) X(xrLocateViews) X(xrEnumerateViewConfigurations) \
								X(xrEnumerateViewConfigurationViews)

const uint32_t kMaxImages = 8;

struct Swapchain {
	XrSwapchain sc;
	uint32_t w, h, count;
	ID3D11RenderTargetView* rtv[kMaxImages];
	DWORD retryAt;
};

// A flat ImGui panel on its own quad layer: drawn into tex through rtv, then copied into the swapchain.
struct Panel {
	Swapchain swap;
	ID3D11Texture2D* tex;
	ID3D11RenderTargetView* rtv;
	bool shown;  // the swapchain has a released image
};

// The newest game frame of one kind ([0] left eye, [1] right eye, [2] mono), copied onto our device.
struct GameView {
	ID3D11Texture2D* tex;
	ID3D11ShaderResourceView* srv;
	ID3D11Texture2D* sceneTex;
	ID3D11ShaderResourceView* sceneSrv;
	ID3D11Texture2D* keyTex;  // upscaling: the scene image the HUD is cut out against (else null)
	ID3D11ShaderResourceView* keySrv;
	LONG seq;
	FrameTag tag;
	bool has;
};

struct VrState {
	PFN_xrGetInstanceProcAddr gipa;
#define DECL_FN(n) PFN_##n n;
	XR_INSTANCE_FUNCS(DECL_FN)
#undef DECL_FN

	XrInstance instance;
	XrSystemId system;
	LUID xrLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
	uint32_t maxSwapW, maxSwapH;

	// VR thread's own D3D11 device (on the game's GPU)
	LUID devLuid;
	D3D_FEATURE_LEVEL devLevel;
	ID3D11Device* dev;
	ID3D11DeviceContext* ctx;
	ID3D11VertexShader* vs;
	ID3D11PixelShader* ps;
	ID3D11PixelShader* psKey;   // HUD/menu extraction: alpha from |full - scene|
	ID3D11PixelShader* psWarp;  // late warp: an eye image re-aimed to the headset's pose of this frame
	ID3D11Buffer* cbWarp;
	ID3D11SamplerState* sampler;
	ID3D11Buffer* cbuf;
	int cbufMode;

	XrSession session;
	XrSessionState state;
	bool running;
	bool exitRequested;  // runtime ended the session (EXITING); don't restart it
	bool sessionLost;    // destroy & recreate the session
	bool instanceLost;   // destroy & recreate everything
	XrSpace local, view;
	XrEnvironmentBlendMode blend;

	int64_t swapFormat;
	Swapchain quadSwap, eyeSwap[2], hudSwap;
	Panel menu, overlay;  // the settings menu (ui/menu.cpp), the stats overlay (ui/overlay.cpp)
	XrPosef menuPose;
	pure::Q menuYaw;      // the menu faces where you looked when it opened...
	XrVector3f menuHead;  // ...from where your head was; MenuDistance away (live)
	bool menuPlaced;

	// game frames, on our device
	LONG servedReqGen;  // last game request we answered
	LONG publishGen;
	ID3D11Texture2D* ring[kRingSize];       // shared with the game device: full frames
	ID3D11Texture2D* ringScene[kRingSize];  // scene-only frames (UI removed)
	ID3D11Texture2D* ringKey[kRingSize];    // upscaling: scene-only frames before upscaling (HUD key)
	GameView views[3];                      // [0] left, [1] right, [2] mono
	UINT contentW, contentH;                // the game's frames
	UINT sceneW, sceneH;                    // its scene-only images (larger when upscaling, upscale.cpp)
	bool srcLinear;

	// UI coverage measurement (decides menu mode)
	ID3D11Texture2D* covTex;
	ID3D11RenderTargetView* covRtv;
	ID3D11Texture2D* covStaging[3];
	bool covPending[3];
	int covNext;
	LONG covMeasuredSeq;
	int menuVotes;   // >0 towards menu, <0 towards gameplay
	int noSceneRun;  // consecutive eye frames without a scene-only image
	unsigned statSceneFrames;
	float statCovSum, statCovMax;
	unsigned statCovN;
	bool menuMode;

	// screen placement
	bool placed;
	bool recenterRequested;
	XrPosef quadPose;
	pure::Q lastYaw;
	bool spaceFlipped;  // the runtime's LOCAL space has its +Y pointing down (Virtual Desktop)
	int upsideFrames;   // consecutive headset frames the head's up points against the reference's up
	float lastHead[3];  // head position last frame (silent space changes show as jumps)
	bool lastHeadValid;
	bool keyDown[256];

	// stats
	unsigned statFrames, statQuadFrames, statStereoFrames, statGameFrames[3];
	// the same counts over half a second, for the stats overlay
	unsigned ovFrames, ovGameFrames[3];
	LARGE_INTEGER ovStart, ovDrawn, ovLastFrame;
	OverlayStats ovStats;
	char ovMode[32];
	char ovUpscaler[128];
	float ovFrameMs[180];  // headset frame times (two seconds at 90 Hz), ring buffer
	int ovFrameNext;
	LARGE_INTEGER statStart, qpcFreq;
	unsigned errorCount;

	// head pose prediction: how long after the VR frame whose display time the game's pose was
	// predicted from (poseBase) its images are actually on screen, averaged
	double lagMs;  // running average
	bool lagHave;
	long long predictNs;  // extra prediction handed to the game thread
	double statLagSum;
	unsigned statLagN;
	float dispRot[2][4];    // eye orientations at this VR frame's display time
	bool dispValid;
	XrView disp[2];         // both eyes at this VR frame's display time (late warp submits these)
	uint32_t recW, recH;    // runtime's recommended per-eye image size
	float resPct, resPctH;  // game picture's sharpness relative to that, horizontally / vertically (0 = unknown)
	UINT resSuggestW, resSuggestH;
	bool dispHave;  // disp holds a valid pair (kept from the last good one while tracking drops)
	// how far the shown image's pose is from the head at display (what the margin must cover)
	double statTurnSum;
	float statTurnMax;
	unsigned statTurnN, statTurnOver;  // over: frames where it exceeded the rendered margin
	float marginDeg;
};
extern VrState g_vr;

// Logs failures; per-frame calls are rate-limited so a persistent error can't flood the log.
bool XrCheck(XrResult r, const char* what, bool perFrame = false);
#define XR_CHECK(call) XrCheck((call), #call)
#define XR_CHECK_FRAME(call) XrCheck((call), #call, true)

template <typename T>
void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

inline bool IsSrgb(int64_t f) {
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
		   f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

struct WarpParams {
	float outTan[4];  // shown eye: tangents of left, right, up, down
	float srcTan[4];  // rendered image: tangent half-width, half-height
	float m[3][4];    // rows of R_render^T * R_shown
};

// xr_main.cpp: swapchains
bool EnsureSwap(Swapchain& s, uint32_t w, uint32_t h, const char* what, bool copyTarget = false);
// Draws `srv` (or, with `scene`, the keyed-out UI of `srv`; with `warp`, the late-warped eye image)
// into the next image of `s`. Returns false if nothing could be submitted.
bool RenderInto(Swapchain& s, ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* scene = nullptr,
				const WarpParams* warp = nullptr);

// xr_device.cpp
bool InitDevice();     // created on the game's GPU as soon as the VR thread starts
void ServiceShare();   // answers the game thread's request for capture textures
void UpdateContent();  // copies newly finished game frames into views[]
void MeasureUICoverage();
void Blit(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11ShaderResourceView* srv,
		  ID3D11ShaderResourceView* scene, bool dstSrgb, const WarpParams* warp = nullptr);
ID3D11ShaderResourceView* KeySrv(const GameView& v);  // the scene image the HUD is cut out against

// xr_frame.cpp
void Frame();  // one headset frame
bool GameIsForeground();
bool KeyPressed(int vk);  // edge-triggered
