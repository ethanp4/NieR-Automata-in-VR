#include "core/config.h"
#include "core/main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config g_cfg;

namespace {
enum Kind {
	K_INT,
	K_HEX,
	K_FLOAT,
	K_PATH,
	K_TEXT,
};

struct Setting {
	const char* section;
	const char* key;
	Kind kind;
	void* ptr;            // the field in g_cfg
	float def, min, max;  // K_INT, K_HEX, K_FLOAT
	const char* comment;  // written above the key, "; " before each line ('\n' separates lines)
	const char* text;     // K_PATH, K_TEXT: the default
};

#define S_INT(sec, key, field, def, lo, hi, c) {sec, key, K_INT, &g_cfg.field, def, lo, hi, c, nullptr}
#define S_HEX(sec, key, field, def, c) {sec, key, K_HEX, &g_cfg.field, def, 0, 255, c, nullptr}
#define S_FLT(sec, key, field, def, lo, hi, c) {sec, key, K_FLOAT, &g_cfg.field, def, lo, hi, c, nullptr}
#define S_PATH(sec, key, field, c) {sec, key, K_PATH, g_cfg.field, 0, 0, 0, c, ""}
#define S_TEXT(sec, key, def, c) {sec, key, K_TEXT, nullptr, 0, 0, 0, c, def}

const Setting kSettings[] = {
	S_INT(
		"VR",
		"Enabled",
		enabled,
		1,
		0,
		1,
		"0 = mod stays passive (game runs normally, nothing is sent to the headset)"),

	S_INT(
		"Input",
		"VRControllers",
		vrControllers,
		1,
		0,
		1,
		"1 = your VR controllers work as a gamepad (Quest/Touch, Index, Pico, Vive, WMR...). A real\n"
		"gamepad and the keyboard keep working too. 0 = off (also in the in-headset menu)"),
	S_INT(
		"Input",
		"ControllerRumble",
		vrRumble,
		1,
		0,
		1,
		"1 = the game's rumble is played on the VR controllers"),
	S_INT(
		"Input",
		"MotionControls",
		motionControls,
		0,
		0,
		1,
		"1 = swing a VR controller to attack: right hand = light attack (X), left hand = heavy attack (Y).\n"
		"The buttons keep working too"),
	S_FLT(
		"Input",
		"SwingSpeed",
		swingSpeed,
		1.5f,
		0.8f,
		5.0f,
		"How fast (metres per second, relative to your head) a hand must move to count as a swing.\n"
		"Lower = easier to trigger; higher = only big swings. The Info page shows your swing speeds"),
	S_FLT(
		"Screen",
		"Distance",
		distance,
		2.5f,
		0.3f,
		50,
		"Virtual screen placement, in metres. Also adjustable in game with the hotkeys below."),
	S_FLT(
		"Screen",
		"Width",
		width,
		3.2f,
		0.2f,
		100,
		nullptr),
	S_FLT(
		"Screen",
		"HeightOffset",
		heightOffset,
		0,
		-10,
		10,
		nullptr),
	S_INT(
		"Screen",
		"StatsOverlay",
		statsOverlay,
		1,
		0,
		1,
		"1 = live stats (frame rates, resolution, head-pose delay) in the corner of your view"),
	S_FLT(
		"Screen",
		"OverlayX",
		overlayX,
		-0.22f,
		-0.6f,
		0.6f,
		"Stats overlay position: metres to the right (negative: left) and up (negative: down) of\n"
		"straight ahead, 0.8 m in front of your eyes; it moves with your head"),
	S_FLT(
		"Screen",
		"OverlayY",
		overlayY,
		0.14f,
		-0.6f,
		0.6f,
		nullptr),
	S_FLT(
		"Screen",
		"OverlayDistance",
		overlayDistance,
		0.8f,
		0.3f,
		3.0f,
		"Stats overlay distance in metres; it keeps its size and place in your view (OverlayX/Y scale with it)"),
	S_FLT(
		"Screen",
		"MenuDistance",
		menuDistance,
		1.1f,
		0.4f,
		3.0f,
		"Settings menu distance in metres (placed when it opens); it keeps its size in your view"),

	S_INT(
		"Stereo",
		"Enabled",
		stereo,
		1,
		0,
		1,
		"1 = start in stereo 3D (F12 toggles between stereo and the flat screen)"),
	S_FLT(
		"Stereo",
		"WorldScale",
		worldScale,
		0.75f,
		0.002f,
		50,
		"Game units per metre: scales eye separation and head movement (bigger = world looks smaller)"),
	S_INT(
		"Stereo",
		"LevelCamera",
		levelCamera,
		1,
		0,
		1,
		"1 = keep the horizon level (removes the game camera's up/down tilt; more comfortable)"),
	S_FLT(
		"Stereo",
		"SteepViewFrom",
		steepFrom,
		45,
		10,
		89,
		"...except in steep views (top-down sections): from this camera pitch (degrees) the tilt is\n"
		"kept more and more, fully from SteepViewFull, so the arena is in front of you"),
	S_FLT(
		"Stereo",
		"SteepViewFull",
		steepFull,
		60,
		10,
		90,
		nullptr),  // and at least SteepViewFrom (below)
	S_INT(
		"Stereo",
		"DepthReprojection",
		depthReproject,
		1,
		0,
		1,
		"EXPERIMENTAL, alternating mode only: the eye drawn in the previous game frame is moved to the new\n"
		"camera using its depth"),
	S_INT(
		"Stereo",
		"ReprojectionEdges",
		reprojectEdges,
		0,
		0,
		1,
		"Depth reprojection: 1 = fill the gaps it opens behind 2B and other near things with the\n"
		"background; 0 = stretch their edges over the gap"),
	S_INT(
		"Stereo",
		"Synchronized",
		synchronized,
		0,
		0,
		1,
		"Stereo mode: 0 = alternating (one eye per game frame), 1 = synchronized (both eyes every game\n"
		"frame: they always match, for about twice the GPU work). F5 switches it in game"),
	S_INT(
		"Stereo",
		"PosePrediction",
		posePrediction,
		1,
		0,
		1,
		"1 = predict the head pose each game frame is rendered with for the moment it will actually be\n"
		"shown (measured), so fast head turns show fewer black edges. 0 = off"),
	S_INT(
		"Stereo",
		"LateWarp",
		lateWarp,
		1,
		0,
		1,
		"1 = each headset frame, both eye images are re-aimed to the headset's own pose and cut to each\n"
		"eye's own field of view before they are sent (needed by streaming headsets: Quest, Pico, ...).\n"
		"0 = sent as rendered"),
	S_INT(
		"Stereo",
		"TopDownFlat",
		topDownFlat,
		0,
		0,
		1,
		"1 = show steep top-down views (e.g. the flight-unit shooting sections) on the flat screen\n"
		"instead of in 3D. Shift+F12 in game. Goes flat after 1 s at TopDownFlatFrom degrees or more,\n"
		"back to 3D after 2 s below TopDownFlatUntil."),
	S_FLT(
		"Stereo",
		"TopDownFlatFrom",
		topDownFrom,
		50,
		10,
		89,
		nullptr),
	S_FLT(
		"Stereo",
		"TopDownFlatUntil",
		topDownUntil,
		35,
		0,
		89,
		nullptr),  // and at most TopDownFlatFrom (below)
	S_FLT(
		"Stereo",
		"FovScale",
		fovScale,
		1.1f,
		0.5f,
		2,
		"Rendered field of view relative to the headset's. 1.10 (default) leaves a little picture beyond\n"
		"your view so fast head turns don't show black edges; 1.0 = exact (sharpest)"),
	S_FLT(
		"Stereo",
		"CameraDolly",
		cameraDolly,
		0,
		-5,
		5,
		"Metres to move the camera towards the character along its line of sight: closer = stronger 3D\n"
		"without shrinking the world (Ctrl+F6 / Ctrl+F7 in game)"),
	S_INT(
		"Stereo",
		"FirstPerson",
		firstPerson,
		0,
		0,
		1,
		"Prototype: 1 = first person: your eyes are at the point the game camera follows (around 2B)\n"
		"instead of behind her; the right stick still turns you. F3 switches it. 2B's head isn't hidden\n"
		"yet: move out of it with FirstPersonHeight / FirstPersonForward"),
	S_FLT(
		"Stereo",
		"FirstPersonHeight",
		fpHeight,
		0,
		-1.5f,
		1.5f,
		"First person: metres (game units) above the point the camera follows"),
	S_FLT(
		"Stereo",
		"FirstPersonForward",
		fpForward,
		0,
		-1.5f,
		1.5f,
		"First person: metres ahead of it, the way the camera faces"),
	S_FLT(
		"Stereo",
		"FirstPersonWorldScale",
		fpWorldScale,
		1,
		0.2f,
		3,
		"First person: WorldScale (see above) while in first person. 1 = life size"),
	S_INT(
		"Stereo",
		"Anamorphic",
		anamorphic,
		1,
		0,
		1,
		"1 = stretch the game image to each eye's own field of view (sharper, no cropping); 0 = off"),
	S_FLT(
		"Stereo",
		"VerticalFill",
		verticalFill,
		1,
		0,
		1,
		"Fraction of your vertical field of view to render (1 = all; lower = sharper, black top/bottom edges)"),
	S_INT(
		"Stereo",
		"FlatBelowDraws",
		flatBelowDraws,
		250,
		0,
		100000,
		"Frames with fewer draw calls than this are shown flat (title and New Game screens have up to ~190,\n"
		"gameplay 500+). 0 = never"),

	S_INT(
		"Render",
		"ExtraResolutions",
		extraResolutions,
		1,
		0,
		2,
		"EXPERIMENTAL supersampling: 1 = offer resolutions above your monitor's (125%-300%) in the game's\n"
		"settings; 2 = also rewrite the game's built-in resolution list (try if 1 shows nothing new).\n"
		"Use the game's windowed/borderless mode with them. See the README."),
	S_INT(
		"Render",
		"AmbientOcclusion",
		ambientOcclusion,
		1,
		0,
		1,
		"0 = turn off ambient occlusion (NieR's half-resolution SAO: blotchy/pixelated on things close to\n"
		"the camera, e.g. 2B with CameraDolly)"),
	S_INT(
		"Render",
		"Shadows",
		shadows,
		1,
		0,
		1,
		"0 = skip the game's screen-space shadow pass (no shadows from the sun and lights): faster"),
	S_INT(
		"Render",
		"Vignette",
		vignette,
		0,
		0,
		1,
		"0 = remove the game's vignette (darker screen edges: in VR they show as a dark frame)"),
	S_INT(
		"Render",
		"LowHealthEffect",
		lowHealthFx,
		0,
		0,
		1,
		"0 = no grey colour filter at low health while in stereo (1 = keep the game's effect)"),
	S_INT(
		"Render",
		"DistortionEffects",
		distortionFx,
		0,
		0,
		1,
		"0 = no screen distortion/glitch effects while in stereo (when hit, at low health): they redraw\n"
		"a flat copy of the whole screen. 1 = keep them (drawn into the 3D image)"),
	S_INT(
		"Render",
		"FixLighting",
		viewFix,
		1,
		0,
		1,
		"1 = fix NieR's tiled lighting and reflections for the VR camera (0 only for comparison)"),
	S_INT(
		"Render",
		"GIVolumes",
		giVolumes,
		128,
		8,
		128,
		"Global illumination volumes that stay on, nearest to the camera first: fewer = faster. 128 = all\n"
		"(no visual change; empty slots are still skipped). Idea from the FAR mod (DrDaxxy, Kaldaien)"),
	S_INT(
		"Render",
		"MaxFrameLatency",
		maxFrameLatency,
		1,
		0,
		3,
		"Frames the game may prepare ahead of the graphics card. Each one delays what you see after a\n"
		"head turn by a frame. 1 = lowest delay (default), 2-3 = a little smoother, 0 = the game's own"),
	S_FLT(
		"Render",
		"CullingFov",
		cullFov,
		170,
		0,
		170,
		"Vertical field of view (degrees) the game uses to decide what to draw while in stereo, so things\n"
		"at the edges of your view and where you look don't vanish. 0 = the game's own (~50).\n"
		"At most 170: a field of view of 180 or more doesn't exist (170 covers ~88 deg to each side)."),
	S_INT(
		"Render",
		"LodFix",
		lodFix,
		1,
		0,
		1,
		"1 = only the camera's projection and culling use the wider view above; the camera itself and\n"
		"model/texture detail keep the game's own (0 = old way: low-detail models in stereo)"),
	S_INT(
		"Render",
		"HeadCulling",
		headCulling,
		1,
		0,
		1,
		"1 = the culling view (above) is turned the way your headset looks, not where the game camera\n"
		"points, so turning your head doesn't make things vanish. Ctrl+Shift+F5 switches it in game"),
	S_TEXT(
		"Render",
		"FovRealCallers",
		"0x729b49",
		"Places in the game (exe offsets, comma separated) that always get the game's own field of view:\n"
		"0x729b49 = the camera code that also aims Pod 042 (the wide view made it shoot at the sky)"),
	S_INT(
		"Render",
		"WideProjection",
		wideProjection,
		0,
		0,
		1,
		"1 = the game camera's own projection also gets the wide view (makes Pod 042 aim at the sky)"),
	S_INT(
		"Render",
		"ReuseShadows",
		reuseShadows,
		1,
		0,
		1,
		"Synchronized mode: the right eye reuses the left eye's shadow map (saves graphics work; 0 = draw it again)"),
	S_FLT(
		"Render",
		"HeadCullingMargin",
		headCullMargin,
		20,
		0,
		60,
		"With HeadCulling=1 the culling view only needs to hold your headset's view: this many degrees\n"
		"are added on every side (more = safer when turning your head fast, less = faster).\n"
		"0 = use CullingFov as before."),

	S_INT(
		"Upscaler",
		"Mode",
		upscaler,
		0,
		0,
		1,
		"Experimental. Sharper, smoother 3D image at a lower game resolution: the game renders smaller and\n"
		"each eye's image is enlarged with detail gathered over several frames (anti-aliasing included).\n"
		"0 = off, 1 = NVIDIA DLSS (RTX graphics cards; nvngx_dlss.dll must be in the game's folder).\n"
		"The game's resolution picks the DLSS mode (see Output below); the stats overlay shows both sizes"),
	S_FLT(
		"Upscaler",
		"Output",
		upscaleOutput,
		100,
		50,
		150,
		"Size of the upscaled image, in % of the detail your headset recommends (100 = its own). It stays\n"
		"the same; as in other games, the game's resolution decides the DLSS mode: the same size = DLAA,\n"
		"1.5x smaller = Quality, 1.7x = Balanced, 2x = Performance, 3x = Ultra performance. The Info page\n"
		"lists the game resolutions for each mode"),
	S_INT(
		"Upscaler",
		"Model",
		dlssModel,
		0,
		0,
		1,
		"DLSS model: 0 = fast (CNN, preset E: about 1 ms per eye on an RTX 2070), 1 = best (transformer,\n"
		"preset K: about 3 ms per eye on an RTX 2070, cheaper on RTX 30/40/50 cards)"),

	S_HEX(
		"Keys",
		"Recenter",
		keyRecenter,
		VK_F8,
		"Windows virtual-key codes (https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)\n"
		"Defaults: F8 recenter, F9 smaller, F10 larger, F6 closer, F7 farther, F12 stereo on/off\n"
		"Ctrl+F9 / Ctrl+F10: world bigger (flatter) / smaller (deeper) - WorldScale, saved automatically\n"
		"Ctrl+F6 / Ctrl+F7: camera closer to / farther from the character - CameraDolly, saved automatically"),
	S_HEX(
		"Keys",
		"Smaller",
		keySmaller,
		VK_F9,
		nullptr),
	S_HEX(
		"Keys",
		"Larger",
		keyLarger,
		VK_F10,
		nullptr),
	S_HEX(
		"Keys",
		"Closer",
		keyCloser,
		VK_F6,
		nullptr),
	S_HEX(
		"Keys",
		"Farther",
		keyFarther,
		VK_F7,
		nullptr),
	S_HEX(
		"Keys",
		"ToggleStereo",
		keyStereo,
		VK_F12,
		nullptr),
	S_HEX(
		"Keys",
		"FirstPerson",
		keyFirstPerson,
		VK_F3,
		"First person on/off (prototype, F3)"),
	S_HEX(
		"Keys",
		"Menu",
		keyMenu,
		VK_INSERT,
		"Opens and closes the in-headset settings menu (default Insert = 0x2D; 0 = no key)"),

	S_PATH(
		"OpenXR",
		"LoaderPath",
		loaderPath,
		"Leave empty for automatic: uses openxr_loader.dll if it sits next to this mod\n"
		"(enables API layers such as OpenXR Toolkit), otherwise talks to the active runtime directly."),

	S_PATH(
		"Loader",
		"ChainLoad",
		chainLoad,
		"Another d3d11.dll-based mod (e.g. ReShade renamed to d3d11_reshade.dll) to load in a chain.\n"
		"Leave empty to use Windows' own d3d11.dll."),

	S_INT(
		"Debug",
		"Verbose",
		verbose,
		0,
		0,
		1,
		"1 = more detail in NieRVR.log"),
	S_HEX(
		"Debug",
		"CaptureKey",
		keyCapture,
		VK_F11,
		"Camera probe: press this key in game to record one frame for analysis (default F11)"),
	S_INT(
		"Debug",
		"Probe",
		probe,
		1,
		0,
		1,
		"0 = disable the capture key"),
};
#undef S_INT
#undef S_HEX
#undef S_FLT
#undef S_PATH
#undef S_TEXT

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
void Widen(const char* s, wchar_t* out, int cap) {
	int i = 0;
	for (; s[i] && i < cap - 1; i++) out[i] = (wchar_t)(unsigned char)s[i];
	out[i] = 0;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// "2.500" -> "2.5", "50.000" -> "50"
void FormatFloat(char* buf, size_t cap, float v) {
	snprintf(buf, cap, "%.3f", (double)v);
	size_t n = strlen(buf);
	while (n > 0 && buf[n - 1] == '0') buf[--n] = 0;
	if (n > 0 && buf[n - 1] == '.') buf[--n] = 0;
	if (!strcmp(buf, "-0")) snprintf(buf, cap, "0");
}

// A number's text as written to the ini.
void ValueText(const Setting& s, float v, char* buf, size_t cap) {
	switch (s.kind) {
		case K_INT: snprintf(buf, cap, "%d", (int)v); break;
		case K_HEX: snprintf(buf, cap, "0x%X", (unsigned)(int)v); break;
		case K_FLOAT: FormatFloat(buf, cap, v); break;
		default: snprintf(buf, cap, "%s", s.text ? s.text : ""); break;
	}
}

bool ReadRaw(const char* section, const char* key, wchar_t* buf, int cap) {
	wchar_t sec[64], k[64];
	Widen(section, sec, 64);
	Widen(key, k, 64);
	GetPrivateProfileStringW(sec, k, L"\x01", buf, cap, g_iniPath);
	return !(buf[0] == 1 && buf[1] == 0);  // false: the key isn't in the file
}

void WriteRaw(const char* section, const char* key, const char* value) {
	wchar_t sec[64], k[64], v[256];
	Widen(section, sec, 64);
	Widen(key, k, 64);
	Widen(value, v, 256);
	WritePrivateProfileStringW(sec, k, v, g_iniPath);
}

// A number from the ini: whole numbers (also hex, 0x..) or decimals ("0.8" for a switch rounds to 1).
bool ReadNumber(const char* section, const char* key, float* out) {
	wchar_t buf[64];
	if (!ReadRaw(section, key, buf, 64)) return false;
	wchar_t* end = nullptr;
	if (wcschr(buf, L'.')) {
		float v = wcstof(buf, &end);  // the C locale: always a '.' decimal point
		if (end == buf) return false;
		*out = v;
	} else {
		long v = wcstol(buf, &end, 0);  // base 0: "0x77" is hex
		if (end == buf) return false;
		*out = (float)v;
	}
	return true;
}

void WriteNumber(const char* section, const char* key, Kind kind, float v) {
	Setting s = {section, key, kind, nullptr, 0, 0, 0, nullptr, nullptr};
	char buf[32];
	ValueText(s, v, buf, sizeof(buf));
	WriteRaw(section, key, buf);
}

// ---------------------------------------------------------------------------------------------
// The ini file
// ---------------------------------------------------------------------------------------------
// Writes NieRVR.ini with every setting at its default, grouped by section, comments included.
void WriteDefaultIni() {
	static char text[16384];
	size_t at = 0;
	auto put = [&](const char* s) {
		size_t n = strlen(s);
		if (at + n < sizeof(text)) memcpy(text + at, s, n), at += n;
	};
	put("; NieRVR configuration. Delete this file to restore defaults.\r\n");
	const char* section = "";
	for (const Setting& s : kSettings) {
		if (strcmp(section, s.section)) {
			section = s.section;
			put("\r\n[");
			put(section);
			put("]\r\n");
		}
		for (const char* c = s.comment; c && *c;) {
			const char* e = c;
			while (*e && *e != '\n') e++;
			char line[200];
			size_t n = (size_t)(e - c) < sizeof(line) - 1 ? (size_t)(e - c) : sizeof(line) - 1;
			memcpy(line, c, n);
			line[n] = 0;
			put("; ");
			put(line);
			put("\r\n");
			c = *e ? e + 1 : e;
		}
		char val[64];
		ValueText(s, s.def, val, sizeof(val));
		put(s.key);
		put("=");
		put(val);
		put("\r\n");
	}
	HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD w;
	WriteFile(f, text, (DWORD)at, &w, nullptr);
	CloseHandle(f);
}

// An ini from an older version keeps working, but lacks the settings added since: add each missing
// key with its default (without the comments). Returns how many were added.
int AddMissingSettings() {
	int added = 0;
	for (const Setting& s : kSettings) {
		wchar_t have[8];
		if (ReadRaw(s.section, s.key, have, 8)) continue;
		char val[64];
		ValueText(s, s.def, val, sizeof(val));
		WriteRaw(s.section, s.key, val);
		added++;
	}
	return added;
}

// "0x729b49, 0x123" -> offsets
void ParseHexList(const wchar_t* buf, unsigned* out, int* count, int max) {
	*count = 0;
	for (int i = 0; buf[i] && *count < max;) {
		while (buf[i] == ' ' || buf[i] == ',' || buf[i] == ';') i++;
		if (buf[i] == '0' && (buf[i + 1] == 'x' || buf[i + 1] == 'X')) i += 2;
		unsigned v = 0;
		int digits = 0;
		for (;; i++, digits++) {
			wchar_t c = buf[i];
			int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
			if (d < 0) break;
			v = v * 16 + (unsigned)d;
		}
		if (digits && v) out[(*count)++] = v;
		if (!digits && buf[i]) i++;  // skip anything else
	}
}

const Setting* Find(const void* field) {
	for (const Setting& s : kSettings)
		if (s.ptr && s.ptr == field) return &s;
	return nullptr;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------------
void ConfigLoad() {
	if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) WriteDefaultIni();
	g_cfg.iniAdded = AddMissingSettings();

	for (const Setting& s : kSettings) {
		if (s.kind == K_PATH) {
			wchar_t sec[64], k[64];
			Widen(s.section, sec, 64);
			Widen(s.key, k, 64);
			GetPrivateProfileStringW(sec, k, L"", (wchar_t*)s.ptr, MAX_PATH, g_iniPath);
			continue;
		}
		if (s.kind == K_TEXT) continue;
		float v;
		if (!ReadNumber(s.section, s.key, &v)) v = s.def;
		v = Clamp(v, s.min, s.max);
		if (s.kind == K_FLOAT)
			*(float*)s.ptr = v;
		else
			*(int*)s.ptr = (int)(v + (v >= 0 ? 0.5f : -0.5f));
	}

	// limits that depend on another setting
	g_cfg.steepFull = Clamp(g_cfg.steepFull, g_cfg.steepFrom, 90);
	g_cfg.topDownUntil = Clamp(g_cfg.topDownUntil, 0, g_cfg.topDownFrom);

	// settings with their own format
	wchar_t buf[200];
	if (!ReadRaw("Render", "FovRealCallers", buf, 200)) Widen("0x729b49", buf, 200);
	ParseHexList(buf, g_cfg.fovRealCallers, &g_cfg.fovRealCount, 8);
	// the camera-easing fov reader found in game; defaults to the Steam build's (checked in game:
	// a wrong one is dropped and searched again)
	unsigned easing[1];
	int n = 0;
	if (ReadRaw("Render", "FovEasingCaller", buf, 200)) ParseHexList(buf, easing, &n, 1);
	g_cfg.fovEasingCaller = n ? easing[0] : 0x7397de;
}

// Writes one setting of g_cfg (pass its field: &g_cfg.worldScale) to NieRVR.ini.
bool ConfigSave(const void* field) {
	const Setting* s = Find(field);
	if (!s || s->kind == K_PATH || s->kind == K_TEXT) return false;
	float v = s->kind == K_FLOAT ? *(const float*)field : (float)*(const int*)field;
	WriteNumber(s->section, s->key, s->kind, v);
	return true;
}

bool ConfigDefault(const void* field, float* value) {
	const Setting* s = Find(field);
	if (!s || s->kind == K_PATH || s->kind == K_TEXT) return false;
	*value = s->def;
	return true;
}

// The camera-easing fov reader found in game (offset in the exe), so the next start skips the search.
void ConfigSaveEasingCaller(unsigned offset) {
	char a[24];
	snprintf(a, sizeof(a), "0x%x", offset);
	WriteRaw("Render", "FovEasingCaller", a);
}
