#pragma once
#include <windows.h>

struct Config {
	// [Loader]
	wchar_t chainLoad[MAX_PATH];  // optional DLL to load instead of System32\d3d11.dll
	// [VR]
	int enabled;
	// [OpenXR]
	wchar_t loaderPath[MAX_PATH];  // optional openxr_loader.dll; empty = auto
	// [Screen]
	float distance;            // metres in front of the head
	float width;               // metres
	float heightOffset;        // metres, relative to eye height
	int statsOverlay;          // live stats in the corner of your view (ui/stats_overlay.cpp)
	float overlayX, overlayY;  // its position: metres right / up of straight ahead (at 0.8 m; scaled with the distance)
	float overlayDistance;     // metres in front of your eyes (it keeps its size in your view)
	float menuDistance;        // the settings menu: metres in front of you when it opens (keeps its size in your view)
	// [Keys]  (Windows virtual-key codes)
	int keyRecenter, keySmaller, keyLarger, keyCloser, keyFarther;
	int keyCapture;  // [Debug] CaptureKey: frame capture
	int probe;       // [Debug] Probe: the frame capture key is on
	// [Stereo]
	int stereo;                  // start in stereo mode
	float worldScale;            // game units per metre of head movement / IPD
	int levelCamera;             // remove the game camera's pitch/roll
	float steepFrom, steepFull;  // camera pitch (deg) where levelling starts to fade out / is fully off
	float fovScale;              // multiplies the rendered frustum (>1 = margin for reprojection)
	int keyStereo;               // [Keys] ToggleStereo
	int keyMenu;                 // [Keys] Menu: opens/closes the settings menu (0 = no key)
	float verticalFill;          // 1 = render the headset's full vertical FOV; 0 = fit width (sharper, crops top/bottom)
	int anamorphic;              // fill each eye's own FOV shape (non-square pixels) instead of a 16:9 cover
	float cameraDolly;           // metres to move the camera along its line of sight (towards the character)
	int firstPerson;             // prototype: eyes at the game camera's look-at target (around 2B), not behind her
	float fpHeight, fpForward;   // first person: game units above / ahead of that target
	float fpWorldScale;          // first person: WorldScale (1 = life size)
	int keyFirstPerson;          // [Keys] FirstPerson
	// [Render]
	int extraResolutions;
	int ambientOcclusion;
	int shadows;      // 0 = NieR's screen-space shadow mask cleared to "lit" (diagnostic)
	int vignette;
	int lowHealthFx;  // 0 = low-health grey/distortion (screen filter effects) removed in stereo
	int distortionFx;            // 0 = remove the screen-copy distortion/glitch effects (hits, low health) in stereo
	float cullFov;               // vertical field of view (deg) the game culls with during stereo; 0 = game default
	float cullFovAsked;          // CullingFov from the ini when above the 170 maximum (for the log), else 0
	unsigned fovEasingCaller;    // exe offset of the camera-easing fov read, once found (0 = unknown)
	unsigned fovRealCallers[8];  // exe offsets of fov getter callers that always get the real fov
	int fovRealCount;
	int headCulling;       // turn the culling frustum the way the headset looks
	float headCullMargin;  // degrees added around the headset's view for the head-culling frustum
	int synchronized;      // both eyes rendered every frame (record + replay)
	int reuseShadows;      // Synchronized: the replay reuses the left eye's shadow map
	int depthReproject;    // alternating mode: move the older eye's image to the new camera with its depth
	int reprojectEdges;    // fill the gaps it opens with background instead of stretching edges
	int posePrediction;    // [Stereo] PosePrediction: predict the game's head pose for when its frame is shown
	int lateWarp;          // [Stereo] LateWarp: submit the headset's pose/fov, eye images re-aimed to it
	int maxFrameLatency;   // [Render] MaxFrameLatency: frames the game's CPU may queue ahead of the GPU; 0 = game's
	int giVolumes;         // [Render] GIVolumes: GI volumes the game's GI pass reads (nearest first); >= 128 = all
	// [Upscaler] (render/upscale.cpp)
	int upscaler;           // 0 off, 1 DLSS
	float upscaleOutput;    // the upscaled image's size: % of the detail the headset recommends
	int dlssModel;          // 0 CNN (preset E, fast), 1 transformer (preset K)
	// [Input]
	int vrControllers;   // VR controllers act as an XInput gamepad
	int vrRumble;        // the game's rumble goes to the VR controllers
	int motionControls;  // swing a hand to attack (vr_controllers.cpp)
	float swingSpeed;    // m/s a hand must move (relative to the head) to count as a swing
	int iniAdded;        // settings added to an older ini at startup
	int flatBelowDraws;  // frames with fewer draws than this render mono (title screen); 0 = off
	int topDownFlat;     // 1 = steep top-down views are shown on the flat screen
	float topDownFrom, topDownUntil;  // TopDownFlat: camera pitch (deg) to go flat / back to stereo
	// [Debug]
	int verbose;
};
extern Config g_cfg;

void ConfigLoad();
bool ConfigSave(const void* field);  // writes one setting to NieRVR.ini: ConfigSave(&g_cfg.worldScale)
bool ConfigDefault(const void* field, float* value);  // a setting's default (ints as floats); false if not in the table
void ConfigSaveEasingCaller(unsigned exeOffset);
