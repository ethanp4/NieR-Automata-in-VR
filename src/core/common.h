// common.h - shared declarations for the NieRVR mod.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>   // snprintf
#include <stdlib.h>  // wcstol, wcstof
#include <string.h>

#include "pure.h"
struct FrameTag;

#ifndef NIERVR_VERSION
#define NIERVR_VERSION "dev"  // set by CMake: -DNIERVR_VERSION=... (the release workflow passes the tag)
#endif

// ---------------------------------------------------------------------------------------------
// Logging (log.cpp)
// ---------------------------------------------------------------------------------------------
void LogOpen(const wchar_t* path);
void Log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));  // GCC checks the arguments
// A wide string (path, GPU name) as UTF-8, for "%s": Log("dir: %s", Utf8(path).s). snprintf's own
// %ls would convert with the C locale and fail on non-ASCII names.
struct Utf8 {
	char s[1024];
	explicit Utf8(const wchar_t* w);
};
// Logs only when [Debug] Verbose=1
void LogV(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

// ---------------------------------------------------------------------------------------------
// Configuration (config.cpp)  - NieRVR.ini next to the DLL
// ---------------------------------------------------------------------------------------------
struct Config {
	// [Loader]
	wchar_t chainLoad[MAX_PATH];  // optional DLL to load instead of System32\d3d11.dll
	// [VR]
	int enabled;
	// [OpenXR]
	wchar_t loaderPath[MAX_PATH];  // optional openxr_loader.dll; empty = auto
	// [Screen]
	float distance;      // metres in front of the head
	float width;         // metres
	float heightOffset;  // metres, relative to eye height
	int statsOverlay;    // live stats in the corner of your view (ui/overlay.cpp)
	float overlayX, overlayY;  // its position: metres right / up of straight ahead (at 0.8 m; scaled with the distance)
	float overlayDistance;     // metres in front of your eyes (it keeps its size in your view)
	float menuDistance;        // the settings menu: metres in front of you when it opens (keeps its size in your view)
	// [Keys]  (Windows virtual-key codes)
	int keyRecenter, keySmaller, keyLarger, keyCloser, keyFarther;
	int keyCapture;  // [Debug] CaptureKey: camera probe frame capture
	int probe;       // [Debug] Probe: install the camera probe hooks
	// [Stereo]
	int stereo;          // start in stereo mode
	float worldScale;    // game units per metre of head movement / IPD
	int levelCamera;     // remove the game camera's pitch/roll
	float steepFrom, steepFull;  // camera pitch (deg) where levelling starts to fade out / is fully off
	float fovScale;      // multiplies the rendered frustum (>1 = margin for reprojection)
	int keyStereo;       // [Keys] ToggleStereo
	int keyMenu;         // [Keys] Menu: opens/closes the settings menu (0 = no key)
	float verticalFill;  // 1 = render the headset's full vertical FOV; 0 = fit width (sharper, crops top/bottom)
	int uiSplit;         // HUD/menus on a separate flat layer
	int anamorphic;      // fill each eye's own FOV shape (non-square pixels) instead of a 16:9 cover
	float cameraDolly;   // metres to move the camera along its line of sight (towards the character)
	int firstPerson;     // prototype: eyes at the game camera's look-at target (around 2B), not behind her
	float fpHeight, fpForward;  // first person: game units above / ahead of that target
	float fpWorldScale;  // first person: WorldScale (1 = life size)
	int keyFirstPerson;  // [Keys] FirstPerson
	// [Render]
	int extraResolutions;
	int ambientOcclusion;
	int shadows;           // 0 = NieR's screen-space shadow mask cleared to "lit" (diagnostic)
	int vignette;
	int lowHealthFx;       // 0 = low-health grey/distortion (screen filter effects) removed in stereo
	int viewFix;
	int distortionFx;      // 0 = remove the screen-copy distortion/glitch effects (hits, low health) in stereo
	float cullFov;         // vertical field of view (deg) the game culls with during stereo; 0 = game default
	int lodFix;            // 1 = model detail keeps the game's own field of view while culling is widened
	float cullFovAsked;    // CullingFov from the ini when above the 170 maximum (for the log), else 0
	unsigned fovEasingCaller;  // exe offset of the camera-easing fov read, once found (0 = unknown)
	unsigned fovRealCallers[8];  // exe offsets of fov getter callers that always get the real fov
	int fovRealCount;
	int headCulling;       // turn the culling frustum the way the headset looks (0.3.22)
	float headCullMargin;  // degrees added around the headset's view for the head-culling frustum (0.3.24)
	int bothEyes;          // render both eyes every frame (record + replay, 0.3.26)
	int reuseShadows;      // BothEyes: the replay reuses the left eye's shadow map (0.3.29)
	int depthReproject;    // alternate eyes: move the older eye's image to the new camera with its depth (0.3.30)
	int reprojectEdges;    // fill the gaps it opens with background instead of stretching edges (0.4.1)
	int posePrediction;    // [Stereo] PosePrediction: predict the game's head pose for when its frame is shown (0.4.4)
	int lateWarp;          // [Stereo] LateWarp: submit the headset's pose/fov, eye images re-aimed to it (0.5.1)
	int maxFrameLatency;   // [Render] MaxFrameLatency: frames the game's CPU may queue ahead of the GPU; 0 = game's (0.4.5)
	int giVolumes;         // [Render] GIVolumes: GI volumes the game's GI pass reads (nearest first); >= 128 = all (0.4.2)
	// [Upscaler] (render/upscale.cpp)
	int upscaler;          // 0 off, 1 DLSS
	float upscaleOutput;   // the upscaled image's size: % of the detail the headset recommends
	int dlssModel;         // 0 CNN (preset E, fast), 1 transformer (preset K)
	int upscaleJitterFlip; // [Debug] UpscaleJitterFlip: bit 0 flips the jitter handed to DLSS across, bit 1 up-down
	// [Input]
	int vrControllers;     // VR controllers act as an XInput gamepad (0.4.0)
	int vrRumble;          // the game's rumble goes to the VR controllers
	int motionControls;    // swing a hand to attack (vrinput.cpp)
	float swingSpeed;      // m/s a hand must move (relative to the head) to count as a swing
	int ignoreUpSwings;    // no swings from a hand moving mostly upward or at the headset
	int eyeSync;           // turn the older eye's image by the game camera's turn since (0.3.25)
	int wideProjection;    // 1 = the camera's own projection gets the wide fov too (0.3.22; Pod aims up)
	int iniAdded;          // settings added to an older ini at startup
	int flatBelowDraws;    // frames with fewer draws than this render mono (title screen); 0 = off
	int topDownFlat;       // 1 = steep top-down views are shown on the flat screen
	float topDownFrom, topDownUntil;  // TopDownFlat: camera pitch (deg) to go flat / back to stereo
	// [Debug]
	int verbose;
};
extern Config g_cfg;
extern wchar_t g_modDir[MAX_PATH];  // directory of this DLL, with trailing backslash
extern wchar_t g_iniPath[MAX_PATH];
void ConfigLoad();
bool ConfigSave(const void* field);  // writes one setting to NieRVR.ini: ConfigSave(&g_cfg.worldScale)
bool ConfigDefault(const void* field, float* value);  // a setting's default (ints as floats); false if not in the table
void ConfigSaveEasingCaller(unsigned exeOffset);
// menu.cpp - the in-headset settings menu
// gpuprof.cpp - GPU timing of one frame every 5 s (diagnostic)
void GpuProfFrameStart(ID3D11DeviceContext* c);
void GpuProfMark(ID3D11DeviceContext* c, const char* label);
void GpuProfTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
					const char* prefix);
void GpuProfCompute(ID3D11DeviceContext* c, const char* prefix, int kind);
void GpuProfDraw();
void GpuProfFrameEnd(ID3D11DeviceContext* c);
void GpuProfPoll(ID3D11DeviceContext* c);
void CapturePollMidFrame();
void FrameLatencyTick();  // probe.cpp: applies [Render] MaxFrameLatency to the game's device  // present_hook.cpp: publish finished frames without waiting for Present
void MenuTick(bool foreground);
void MenuSetInfo(int line, const char* text);
bool MenuIsOpen();
bool MenuTakeJustOpened();
bool MenuTakeRecenter();
// vrinput.cpp - VR controllers as a gamepad (0.4.0)
struct _XINPUT_GAMEPAD;
bool VrInputPad(struct _XINPUT_GAMEPAD* out, bool evenIfOff = false);  // false = no VR gamepad; evenIfOff: for the menu
void VrInputRumble(unsigned short left, unsigned short right);
void VrInputDetach();
void VrInputShutdown();
void VrInputSync(bool focused, bool foreground);
void VrInputProfileChanged();  // interaction profile changed event
// ui/overlay.cpp - the stats overlay, drawn with Dear ImGui on the VR thread ([Screen] StatsOverlay)
struct OverlayStats {
	const char* mode;              // "alternate eyes", "flat screen", ...
	float headsetFps;
	float gameFps[3];              // game images per second: left, right, flat
	unsigned gameW, gameH;         // the game's render size
	unsigned sceneW, sceneH;       // the 3D image's size in the headset (larger when upscaling)
	const char* upscaler;          // "DLSS 310.9.16 Quality" or why not; "" = off
	float sharpX, sharpY;          // % of the headset's recommended detail, across / up-down; 0 = unknown
	float poseDelayMs, predictMs;  // poseDelayMs < 0 = unknown
	const float* frameMs;          // headset frame times, a ring buffer for the graph
	int frameCount, frameOffset;   // its size, and the index of the oldest entry
};
bool OverlayRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h,
				   const OverlayStats& s);
bool MenuRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h);

// ---------------------------------------------------------------------------------------------
// Startup (main.cpp)
// ---------------------------------------------------------------------------------------------
void EnsureInit();  // idempotent, thread safe; loads config, log, real d3d11
extern HMODULE g_hSelf;

// ---------------------------------------------------------------------------------------------
// Proxy (proxy.cpp)
// ---------------------------------------------------------------------------------------------
bool ProxyLoadReal();  // called from EnsureInit
extern PFN_D3D11_CREATE_DEVICE g_realCreateDevice;
extern PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain;

// ---------------------------------------------------------------------------------------------
// Present hook + game-side frame capture (present_hook.cpp)
// ---------------------------------------------------------------------------------------------
void HookFromSwapChain(IDXGISwapChain* sc);
bool HookFunction(void* target, void* detour, void** orig, const char* name);  // MinHook wrapper

// Arguments of a draw call, for replaying it (kind: 0 Draw, 1 DrawIndexed, 2 DrawInstanced,
// 3 DrawIndexedInstanced; fields in the API's parameter order).
struct DrawArgs {
	int kind;
	UINT a, b;
	INT c;
	UINT d, e;
};

// UI split (uisplit.cpp): scene-only copy of each frame, so HUD/menus can go on a flat layer.
void UiSetBackbuffer(ID3D11Texture2D* bb);
void UiBeginFrame();
void UiOnSetVS(bool isUI);
bool UiBeforeDraw(ID3D11DeviceContext* c);  // true = skip this draw
void UiOnMap(ID3D11Resource* res, void* data, UINT size);
void UiBeforeUnmap(ID3D11Resource* res);
void UiOnUpdate(ID3D11Resource* res, const void* data, UINT size);
void UiOnSetVSConstantBuffers(UINT start, UINT n, ID3D11Buffer* const* bufs);
void UiAfterDraw(ID3D11DeviceContext* c, const DrawArgs& args);
ID3D11Texture2D* UiSceneTexture();  // this frame's scene without UI, or null
void ScreenFxBeginFrame();  // screenfx.cpp: low-health grey / distortion
// An effect-system draw (particles, screen filters) that samples the texture registers in `sampled`:
// true = skip it (a low-health screen filter while in stereo, LowHealthEffect=0).
bool ScreenFxSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash);
void ProbeReplayDraw(ID3D11DeviceContext* c, const DrawArgs& d);
void ProbeSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
						   ID3D11DepthStencilView* dsv);

// Camera probe (probe.cpp): records one frame of D3D11 activity on a hotkey, for analysis.
void ProbeInstall(ID3D11Device* gameDevice);
void ProbeSetTarget(ID3D11Device* presentingDevice);
void ProbeBeforePresent();  // end of a frame
void ProbeAfterPresent();   // start of the next frame
void HookWithDummySwapChain();
// Render resolution (display.cpp)
void DisplayInstallHooks();
void DisplayEarlyHooks();  // from DllMain: patches code only
extern volatile bool g_initDone;  // EnsureInit has finished
void DisplayHookSwapChain(IDXGISwapChain* sc);
void DisplayOnFirstPresent(IDXGISwapChain* sc);
void UiOnResizeBuffers();  // the swapchain's buffers are about to be replaced
void StereoCountDraw();
void CullingUpdate(bool wide);
void CullingObserveGameFov(float deg);
float CullingGameFov();  // the field of view (deg) the game last asked for, before widening
// First person: the game camera's last look-at (eye, target), from the frame whose eye is at camPos
// if there is one (*matched), else the newest. False if none in the last 250 ms.
bool CullingLookAt(const float camPos[3], float eye[3], float target[3], bool* matched);
void CullingInstallHook();  // from EnsureInit: hooks the game camera's fov setter
void CullingWatch();  // diagnostic: hardware breakpoints on the camera fov for 2 s (Ctrl+F5)
void CullingToggleHeadCulling();  // Ctrl+Shift+F5: head-direction culling on/off (saved)
void CullingCycleTest();          // Shift+F5: which reads get the wide fov (test, not saved)
bool StereoGetViewFix(float T[16], float Tinv[16]);  // game view -> eye view (and back), this frame
void ViewFixBeforeDispatch(ID3D11DeviceContext* c, int kind);
void ViewFixAfterDispatch(ID3D11DeviceContext* c);
void ProbeRawDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
// Second-eye playback (replay.cpp): a recorded constant-buffer upload, camera patched for this eye.
const void* ProbePatchForReplay(ID3D11Resource* res, const void* data, UINT size, float* scratch /* 64 floats */);

// replay.cpp - BothEyes: record the left eye's frame, play it back for the right eye at Present.
extern volatile bool g_replaying;      // playback running: hooks pass calls straight through
extern volatile LONG g_recOn;
extern ID3D11DeviceContext* g_recCtx;
extern volatile LONG g_recSuspend;     // mod-internal calls (not recorded)
inline bool RecOn(ID3D11DeviceContext* c) { return g_recOn && c == g_recCtx && !g_recSuspend && !g_replaying; }
enum RecStage { RS_VS = 0, RS_HS, RS_DS, RS_GS, RS_PS, RS_CS };
void RecSetCB(int stage, UINT start, UINT n, ID3D11Buffer* const* b);
void RecSetSRV(int stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v);
void RecSetShader(int stage, void* sh, ID3D11ClassInstance* const* ci, UINT n);
void RecCSSetUAV(UINT start, UINT n, ID3D11UnorderedAccessView* const* v, const UINT* counts);
void RecDraw(int kind, UINT a, UINT b, INT c, UINT d, UINT e);
void RecDispatch(UINT x, UINT y, UINT z, int viewFixKind);
void RecOMSetRT(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void RecViewports(UINT n, const D3D11_VIEWPORT* vp);
void RecBeforeMap(ID3D11Resource* res, D3D11_MAP type);
void RecMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, const D3D11_MAPPED_SUBRESOURCE* m);
void RecUnmap(ID3D11Resource* res, UINT sub);
void RecUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp);
void RecUnsupported(const char* what);
void ReplayInstall(ID3D11DeviceContext* ctx);
bool ReplayAvailable();
void ReplayFrameStart(ID3D11DeviceContext* c, bool bothEyes);
void ReplayRelease();  // before the swapchain is resized: hold nothing of the game's
void ReplayStop(bool fromUi);
void ReplayCaptureFinalPassState(ID3D11DeviceContext* c);
void ReplayApplyFinalPassState(ID3D11DeviceContext* c);
bool ReplaySecondEye(ID3D11DeviceContext* c, bool* inBackbuffer);
void ReplayReport();
bool UiSecondEyeScene(ID3D11DeviceContext* c);  // right eye's scene image from the replayed UI target
bool StereoBothEyesFrame();      // this frame renders the left eye and replays the right one
void StereoBeginSecondEye();     // switch the camera patching to the right eye (same head pose)
bool StereoSecondEyePatched();
bool StereoReprojectParams(int eye, float T[16], float proj[4], FrameTag* tag);  // depth reprojection
int StereoFrameEye();  // the eye this frame renders (-1 = mono)
bool StereoUpscaleCams(int eye, float T[16], float cur[4], float prev[4], float jitter[2], bool* hasPrev);
// reproject.cpp - depth-based correction of the older eye (alternate-eye mode, 0.3.30)
void ReprojectNoteDsv(ID3D11DepthStencilView* dsv);   // a depth buffer the game draws with
void ReprojectAtUiStart(ID3D11DeviceContext* c);      // the scene is finished: keep its depth
void ReprojectRelease();                              // before a resize
// at Present after this frame's eye was captured: returns the warped other eye (or null)
ID3D11Texture2D* ReprojectOtherEye(ID3D11DeviceContext* c, ID3D11Texture2D* scene, ID3D11Texture2D* bb, FrameTag* tag);
ID3D11Resource* ReprojectSceneDepth();                // the game's scene depth buffer (raw), or null
void ReprojectSetBackbufferSize(UINT w, UINT h);      // at Present: the size the scene depth must have
// upscale.cpp - temporal upscaling of the 3D image for the headset ([Upscaler])
// At Present, before the frame goes to the headset: decides this frame's scene-image size (*outW,
// *outH: enlarged when upscaling, else w x h). True while upscaling (also DLAA: same size).
bool UpscaleBeginPresent(ID3D11DeviceContext* c, UINT w, UINT h, UINT* outW, UINT* outH);
// The HUD-free scene image of `eye` (-1 = mono) at the output size: upscaled, or stretched when it
// can't be. Returns `scene` itself when not upscaling, null on failure.
ID3D11Texture2D* UpscaleScene(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* scene);
void UpscaleAtUiStart(ID3D11DeviceContext* c);               // the scene is finished: keep this eye's depth
void UpscaleCaptureDepth(ID3D11DeviceContext* c, int eye);   // BothEyes: the replayed right eye's depth
bool UpscaleJitter(int eye, float ndc[2], float pixels[2]);  // stereo.cpp: this frame's sub-pixel offset
bool UpscaleWantsDepth();                                    // the scene depth buffer has to be tracked
void UpscaleRelease();                                       // before a resize
void UpscaleStatus(char* out, size_t cap);                   // for the stats overlay / Info page
// dlss.cpp: NVIDIA's DLSS file (nvngx_dlss.dll in the game's folder), downloaded from the menu
enum { DF_MISSING, DF_DOWNLOADING, DF_INSTALLED, DF_DELETING };
struct DlssFileState {
	int state;
	long long got, total;  // bytes, while downloading
	char text[96];         // installed: its version; missing: why the last try failed ("" = none)
};
void DlssFileGet(DlssFileState* s);
void DlssFileDownload();  // starts a background download (licence, then the DLL, checked by SHA256)
void DlssFileCancel();
void DlssFileDelete();    // removes it (DLSS is shut down first if it runs)
// download.cpp: HTTPS download to `path` (via path.part), only kept when its SHA256 matches. got/total:
// progress in bytes (total 0 = unknown); *cancel stops it. err: why it failed.
bool HttpDownload(const wchar_t* url, const wchar_t* path, const char* sha256, volatile LONG64* got,
				  volatile LONG64* total, volatile LONG* cancel, char* err, size_t errCap);
void StateSaveTemp(ID3D11DeviceContext* c);           // replay.cpp: pipeline state around our own drawing
void StateRestoreTemp(ID3D11DeviceContext* c);   // the replay found and patched the camera

// ---------------------------------------------------------------------------------------------
// Stereo (stereo.cpp game side, xr_main.cpp VR side)
// ---------------------------------------------------------------------------------------------
struct StereoShared {  // written by the VR thread, read by the game thread
	SRWLOCK lock;
	bool enabled;      // stereo toggled on (hotkey)
	bool valid;        // eye poses below are current
	float eyePos[2][3], eyeRot[2][4];  // LOCAL space
	float eyeTan[2][4];                // tan of angleLeft, angleRight, angleUp, angleDown
	bool menuMode;                     // the UI covers the screen: render flat (mono)
	bool refValid;                     // recentred reference (yaw-only head pose)
	float refPos[3], refRot[4];
	bool singleEye;                    // diagnostic: render only the left eye, every frame (Ctrl+F12)
	long long poseBase;                // VR frame display time (XrTime) the poses were predicted from;
									   // they are predicted for poseBase + the measured lag (0.4.4)
};
extern StereoShared g_stereo;

struct FrameTag {  // what a captured game frame shows
	int eye;       // 0 left, 1 right, -1 mono
	bool patched;  // the camera was rewritten for that eye
	bool hasScene; // a scene-only (no UI) image was captured too
	float pos[3], rot[4];  // eye pose (LOCAL) it was rendered from
	float tanHalfW, tanHalfH;
	unsigned pairId;       // BothEyes: left and right image of one game frame share it (0 = single)
	bool noUI;             // drawn without UI (the replayed second eye): don't key the HUD from it
	bool hasWorld;         // worldRot is valid (the scene camera was patched)
	float worldRot[4];     // game-world orientation of LOCAL space for this frame (eye sync, 0.3.25)
	long long poseBase;    // StereoShared::poseBase of the pose it was rendered with (0 = unknown)
};

void StereoSetAspect(float aspect);
void StereoBeginFrame();
void StereoGetFrameTag(FrameTag* tag);
bool StereoActive();
bool StereoGameCamera(float pos[3], float fwd[3], float up[3]);
bool StereoWatching();
bool StereoHeadRelative(pure::M4* rel);
float StereoHeadCullFov(float marginDeg);  // radians; 0 = not known yet  // eye pose relative to the game camera (head culling)
void StereoOnMap(ID3D11Resource* res, void* data, UINT size);
void StereoBeforeUnmap(ID3D11Resource* res);
const void* StereoPatchUpdate(UINT size, const void* data, float* scratch);
void StereoMemoryScan();

// Frame hand-off from the game's device to the VR thread's device.
// The VR thread owns the shared texture (created on its own device); the game thread asks for a
// size/format and opens whatever the VR thread publishes.
// Capture ring slots (8 since 0.3.27: BothEyes pushes two images per game frame).
const int kRingSize = 8;
struct FrameShare {
	CRITICAL_SECTION cs;
	// request: written by the game thread (under cs)
	LONG reqGen;  // bumps whenever the request changes
	UINT reqW, reqH;
	UINT reqSceneW, reqSceneH;  // the scene-only images: larger than the frame when upscaling
	bool reqKey;                // upscaling: also the scene images before upscaling (ringKey)
	// written by the VR thread: the game-image size with the detail the headset recommends (0 = not
	// known yet): the upscaler's output follows it
	UINT nativeW, nativeH;
	DXGI_FORMAT reqFmt;
	// reply: written by the VR thread (under cs): a ring of shared textures
	HANDLE ring[kRingSize];       // legacy shared handles: full frames
	HANDLE ringScene[kRingSize];  // and scene-only (no UI) frames
	HANDLE ringKey[kRingSize];    // upscaling: the scene-only frames before upscaling (the HUD is cut out
								  // against these, which match the full frames pixel for pixel); else null
	LONG gen;        // bumps whenever a new ring is published
	LONG forReqGen;  // the request this ring answers
	UINT w, h;
	UINT sceneW, sceneH;
	DXGI_FORMAT fmt;
	// completed frames, published by the game thread (under cs) once the GPU finished the copy
	struct Published {
		LONG seq;  // 0 = none yet
		int slot;  // ring index
		FrameTag tag;
	} latest[3];   // [0] left eye, [1] right eye, [2] mono
	LONG seq;      // last sequence number handed out
	// GPU that runs the game
	LUID gameLuid;
	bool gameLuidValid;
};
extern FrameShare g_share;

// main.cpp helpers
void DescribeAddress(void* addr, char* out, size_t cap);  // "module.dll+0x1234"
// Returns true while fewer than `max` messages were logged through this counter.
bool LogBudget(volatile LONG* counter, LONG max);

// ---------------------------------------------------------------------------------------------
// VR thread (xr_main.cpp)
// ---------------------------------------------------------------------------------------------
void XrStartThread();
extern volatile DWORD g_xrThreadId;
