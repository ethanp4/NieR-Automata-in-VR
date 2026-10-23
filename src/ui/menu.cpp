// menu.cpp - the in-headset settings menu.
//
// Opened with the menu key ([Keys] Menu, default Insert) or, on a gamepad or the VR controllers, by
// holding B and pressing A; B, Start or the menu key close it. X (or Delete) puts the highlighted setting
// back to its default. While it is open the game doesn't see
// the gamepad (XInputGetState is hooked and returns a neutral pad). Every change applies at once and
// is written to NieRVR.ini; a few settings only take effect after a restart and say so.
//
// Drawn with Dear ImGui (ui/imgui_panel.h) on the VR thread and used with ImGui's own gamepad and
// keyboard navigation; xr_main.cpp shows it on a quad in front of you.
#include "core/common.h"
#include "input/gamepad.h"
#include "ui/imgui_panel.h"
#include "imgui.h"
#include "imgui_internal.h"  // FocusItem

namespace {
// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
enum Type { T_BOOL, T_INT, T_FLOAT, T_ACTION, T_TEXT, T_CHOICE };  // T_CHOICE: fmt holds the names, "Off|On"
enum Flags { F_RESTART = 1, F_LOG = 2, F_RECENTER = 4, F_ALTERNATE = 8, F_BOTHEYES = 16 };  // ALTERNATE/BOTHEYES: only used in that mode
enum Action { A_RECENTER = 1, A_BOTHEYES_STATS, A_DLSS_FILE };
struct Item {
	const char* label;
	Type type;
	void* ptr;  // the g_cfg field (its ini section and key come from config.cpp)
	float min, max;  // slider range
	const char* fmt;
	int flags;
	const char* help;
};

Item kView[] = {
	{"Both eyes every frame", T_BOOL, &g_cfg.bothEyes, 0, 1, nullptr, 0,
	 "Draws the game's frame a second time for the right eye: both eyes show the same moment, 60 times a second. "
	 "About twice the graphics work. Off: the eyes take turns (30 images a second each). Also F5."},
	{"World scale", T_FLOAT, &g_cfg.worldScale, 0.2f, 3.0f, "%.3f", F_LOG,
	 "Eye distance in the game world. Lower = the world looks bigger and the 3D flatter; higher = smaller, "
	 "deeper. 0.75 is the default. Also Ctrl+F9 / Ctrl+F10."},
	{"Camera dolly", T_FLOAT, &g_cfg.cameraDolly, -3.0f, 3.0f, "%.2f m", 0,
	 "Moves the camera along its line of sight towards the character (positive) for stronger 3D. Also Ctrl+F6 / Ctrl+F7."},
	{"First person (prototype)", T_BOOL, &g_cfg.firstPerson, 0, 1, nullptr, 0,
	 "Your eyes go to the point the game camera follows (around 2B) instead of behind her; the right stick still "
	 "turns you. 2B's head isn't hidden yet: move out of it with the settings below. Also F3."},
	{"First person: height", T_FLOAT, &g_cfg.fpHeight, -1.5f, 1.5f, "%.2f m", 0,
	 "Moves your eyes up (positive) or down from the point the camera follows."},
	{"First person: forward", T_FLOAT, &g_cfg.fpForward, -1.5f, 1.5f, "%.2f m", 0,
	 "Moves your eyes forward (positive), the way the camera faces, e.g. out of 2B's head."},
	{"First person: world scale", T_FLOAT, &g_cfg.fpWorldScale, 0.2f, 3.0f, "%.2f", F_LOG,
	 "Like World scale, while in first person. 1 = life size."},
	{"Level the horizon", T_BOOL, &g_cfg.levelCamera, 0, 1, nullptr, 0,
	 "Removes the game camera's up/down tilt so the horizon stays level (comfort). Steep top-down views keep "
	 "their tilt (see below)."},
	{"Keep tilt from", T_FLOAT, &g_cfg.steepFrom, 10, 89, "%.0f deg", 0,
	 "Camera pitch where the game's tilt starts to be kept (top-down sections)."},
	{"Keep full tilt at", T_FLOAT, &g_cfg.steepFull, 10, 90, "%.0f deg", 0,
	 "Camera pitch from which the game's tilt is kept completely."},
	{"Top-down views flat", T_BOOL, &g_cfg.topDownFlat, 0, 1, nullptr, 0,
	 "Shows steep top-down views (flight unit shooting sections) on the flat screen instead of in 3D. Also Shift+F12."},
	{"Fill eye shape", T_BOOL, &g_cfg.anamorphic, 0, 1, nullptr, 0,
	 "Stretches the image to each eye's own field of view (sharper). Off: a 16:9 picture that covers it."},
	{"Rendered field of view", T_FLOAT, &g_cfg.fovScale, 0.8f, 1.4f, "%.2fx", 0,
	 "More than 1 draws a margin around your view: fewer black edges when you turn your head quickly or the "
	 "game falls behind, at some cost in sharpness. 1.10 is the default; 1.00 is the sharpest."},
	{"Vertical fill", T_FLOAT, &g_cfg.verticalFill, 0.5f, 1.0f, "%.2f", 0,
	 "Fraction of your vertical view that is drawn. Lower is sharper with black bars at the top and bottom."},
	{"Head pose prediction", T_BOOL, &g_cfg.posePrediction, 0, 1, nullptr, 0,
	 "Renders each game frame from where your head will be when it is shown (measured), not where it was: fewer "
	 "black edges when you turn your head. The Info page shows the delay it predicts for."},
	{"Late warp", T_BOOL, &g_cfg.lateWarp, 0, 1, nullptr, 0,
	 "Each headset frame, both eye images are re-aimed to where your head is now and cut to each eye's own field of "
	 "view. Needed by headsets that stream (Quest, Pico): without it alternate eyes stutter and a Quest looks "
	 "cross-eyed. Off: images are sent as rendered (0.5.0)."},
	{"Depth reprojection (experimental)", T_BOOL, &g_cfg.depthReproject, 0, 1, nullptr,
	 F_ALTERNATE,
	 "Experimental. Alternate eyes only: the other eye's last image is moved to where the camera is now, using its depth, so both "
	 "eyes follow camera motion 60 times a second. Off: each eye updates 30 times a second. No effect with both eyes "
	 "every frame."},
	{"Reprojection gap fill (exp.)", T_BOOL, &g_cfg.reprojectEdges, 0, 1, nullptr, F_ALTERNATE,
	 "Depth reprojection: where moving the image uncovers the background (around 2B), fill it with the background "
	 "instead of stretching 2B's edge over it. Off: the 0.4.0 look, to compare."},
	{"Recenter", T_ACTION, (void*)A_RECENTER, 0, 0, nullptr, 0,
	 "Places the screen and the view straight ahead of you again (also F8)."},
};
Item kRender[] = {
	{"Upscaler (experimental)", T_CHOICE, &g_cfg.upscaler, 0, 1, "Off|DLSS", 0,
	 "DLSS turns the game's image into a sharp one at the size your headset wants, without jagged, shimmering edges. "
	 "As in other games, the game's resolution picks the mode: lower it for more speed (Quality, Balanced, "
	 "Performance). The Info page shows which resolution gives which mode. Needs an NVIDIA RTX "
	 "card and nvngx_dlss.dll in the game's folder."},
	{"DLSS file", T_ACTION, (void*)A_DLSS_FILE, 0, 0, nullptr, 0,
	 "NVIDIA's DLSS isn't included with the mod. Download gets nvngx_dlss.dll (59 MB) from NVIDIA's GitHub "
	 "(github.com/NVIDIA/DLSS) into the game's folder, with NVIDIA's licence next to it (nvngx_dlss_LICENSE.txt), "
	 "which you accept by downloading. DLSS starts as soon as it is there. Press again to cancel a download; once "
	 "installed, press twice to delete it."},
	{"Upscaler output", T_FLOAT, &g_cfg.upscaleOutput, 50, 150, "%.0f%% of the headset's detail", 0,
	 "Size of the image DLSS makes, compared with what your headset recommends (100% = its own). It stays put while "
	 "you change the game's resolution. Less is a little faster, more is sharper (supersampling)."},
	{"DLSS model", T_CHOICE, &g_cfg.dlssModel, 0, 1, "Fast (CNN)|Best (transformer)", 0,
	 "Fast: about 1 ms per eye on an RTX 2070. Best: sharper in motion, about 3 ms per eye on an RTX 2070 (much "
	 "less on RTX 30 cards and newer)."},
	{"Ambient occlusion", T_BOOL, &g_cfg.ambientOcclusion, 0, 1, nullptr, 0,
	 "NieR's ambient occlusion (soft contact shadows). Off removes it (it is half resolution and can look blotchy "
	 "up close). Also Ctrl+F11."},
	{"Shadows", T_BOOL, &g_cfg.shadows, 0, 1, nullptr, 0,
	 "Off skips the game's screen-space shadow pass (no shadows from the sun and lights): faster. Also Ctrl+Shift+F11."},
	{"Vignette", T_BOOL, &g_cfg.vignette, 0, 1, nullptr, 0,
	 "The game's darkened screen edges. In VR they look like a dark frame, so off by default."},
	{"Low-health effect", T_BOOL, &g_cfg.lowHealthFx, 0, 1, nullptr, 0,
	 "The grey filter and ripples at low health while in stereo."},
	{"Screen distortions", T_BOOL, &g_cfg.distortionFx, 0, 1, nullptr, 0,
	 "The glitch/distortion effects when hit. Off removes them in stereo (they redraw a flat copy of the screen)."},
	{"Fix lighting", T_BOOL, &g_cfg.viewFix, 0, 1, nullptr, 0,
	 "Moves the game's lights and reflections into each eye's view. Leave on (off only to compare)."},
	{"Frame queue", T_INT, &g_cfg.maxFrameLatency, 0, 3, "%d", 0,
	 "How many frames the game may prepare ahead of the graphics card. Each one delays what you see after a head "
	 "turn by a frame. 1 = lowest delay (default); 2-3 may run a little faster; 0 = the game's own (3)."},
	{"Global illumination volumes", T_INT, &g_cfg.giVolumes, 8, 128, "%d", 0,
	 "How many GI volumes (the boxes that give places their reflections and ambient light) stay on, nearest to you "
	 "first. Fewer = faster. 128 = all: no visual change, only empty slots are skipped. The Info page shows how many "
	 "the area uses and the GI pass time. Idea from the FAR mod."},
	{"Reuse shadows (both eyes)", T_BOOL, &g_cfg.reuseShadows, 0, 1, nullptr, F_BOTHEYES,
	 "Both-eye mode: the right eye reuses the left eye's shadow map instead of drawing it again (saves graphics work)."},
	{"Flat below draws", T_INT, &g_cfg.flatBelowDraws, 0, 1000, "%d", 0,
	 "Frames with fewer draws than this are shown on the flat screen (title and loading screens)."},
	{"Extra resolutions", T_INT, &g_cfg.extraResolutions, 0, 2, "%d", F_RESTART,
	 "Adds resolutions above your monitor's to the game's list (1), and also patches its table (2)."},
};
Item kCull[] = {
	{"Head culling", T_BOOL, &g_cfg.headCulling, 0, 1, nullptr, 0,
	 "Turns the game's culling view the way your headset looks, so nothing vanishes when you look around. Also Ctrl+Shift+F5."},
	{"Head culling margin", T_FLOAT, &g_cfg.headCullMargin, 0, 60, "%.0f deg", 0,
	 "Extra degrees around your view that are still drawn. More is safer for fast head turns, less is faster."},
	{"Culling field of view", T_FLOAT, &g_cfg.cullFov, 0, 170, "%.0f deg", 0,
	 "Height of the game's culling view while in stereo (used as is when head culling is off). 0 = the game's own."},
	{"Wide projection", T_BOOL, &g_cfg.wideProjection, 0, 1, nullptr, 0,
	 "Also gives the game camera's own projection the wide view (0.3.22 behaviour). Makes Pod 042 aim at the sky."},
	{"Model detail fix", T_BOOL, &g_cfg.lodFix, 0, 1, nullptr, F_RESTART,
	 "Keeps model and texture detail at the game's own level while culling is widened."},
};
Item kScreen[] = {
	{"Screen distance", T_FLOAT, &g_cfg.distance, 0.3f, 10, "%.2f m", F_RECENTER,
	 "Distance of the flat screen and the HUD (also F6 / F7)."},
	{"Screen width", T_FLOAT, &g_cfg.width, 0.2f, 10, "%.2f m", 0,
	 "Width of the flat screen and the HUD (also F9 / F10)."},
	{"Screen height", T_FLOAT, &g_cfg.heightOffset, -2, 2, "%.2f m", F_RECENTER,
	 "Moves the flat screen and the HUD up or down."},
	{"HUD on its own layer", T_BOOL, &g_cfg.uiSplit, 0, 1, nullptr, 0,
	 "Shows the HUD and menus on a flat layer in front of you instead of inside the 3D image."},
	{"Stats overlay", T_BOOL, &g_cfg.statsOverlay, 0, 1, nullptr, 0,
	 "Live numbers in the corner of your view (placed with the two settings below): frame rates, the game's resolution and how sharp it is "
	 "compared with what the headset recommends, and the head-pose delay."},
	{"Overlay left / right", T_FLOAT, &g_cfg.overlayX, -0.6f, 0.6f, "%.2f m", 0,
	 "Moves the stats overlay sideways: negative = left (metres at 0.8 m away). It moves with your head."},
	{"Overlay up / down", T_FLOAT, &g_cfg.overlayY, -0.6f, 0.6f, "%.2f m", 0,
	 "Moves the stats overlay up (positive) or down."},
	{"Overlay distance", T_FLOAT, &g_cfg.overlayDistance, 0.3f, 3.0f, "%.2f m", 0,
	 "How far in front of your eyes the stats overlay floats. It keeps its size and place in your view."},
	{"Menu distance", T_FLOAT, &g_cfg.menuDistance, 0.4f, 3.0f, "%.2f m", 0,
	 "How far in front of you this menu opens. It keeps its size in your view, and moves as you change this."},
	{"Recenter", T_ACTION, (void*)A_RECENTER, 0, 0, nullptr, 0, "Places the screen straight ahead of you again (also F8)."},
};
Item kInput[] = {
	{"VR controllers as gamepad", T_BOOL, &g_cfg.vrControllers, 0, 1, nullptr, 0,
	 "Your VR controllers work as an Xbox gamepad: sticks, triggers, grips = LB/RB, A/B/X/Y, stick clicks. Left menu "
	 "button: tap = Start, hold = Back, hold + left stick = D-pad. A real gamepad and the keyboard keep working. "
	 "Turn off to play with only those."},
	{"Controller rumble", T_BOOL, &g_cfg.vrRumble, 0, 1, nullptr, 0,
	 "The game's rumble is played on the VR controllers."},
	{"Motion controls", T_BOOL, &g_cfg.motionControls, 0, 1, nullptr, 0,
	 "Swing a controller to attack: right hand = light attack, left hand = heavy attack (2B's two weapons). "
	 "A short rumble confirms each swing. The buttons keep working too."},
	{"Swing speed needed", T_FLOAT, &g_cfg.swingSpeed, 0.8f, 5.0f, "%.1f m/s", 0,
	 "How fast a hand must move (relative to your head) to count as a swing. Lower = easier; higher = only big "
	 "swings. The Info page shows how fast your swings are."},
	{"Ignore hand-to-headset movements", T_BOOL, &g_cfg.ignoreUpSwings, 0, 1, nullptr, 0,
	 "Reaching for your headset doesn't attack: a hand moving mostly upward doesn't count, nor one near your head "
	 "or just lowered from it. Raising your arm for a swing then only attacks on the way down. Off: every fast "
	 "movement attacks."},
};
const int kInfoLines = 18;

struct Page {
	const char* name;
	Item* items;
	int count;
};
Page g_pages[] = {{"View", kView, sizeof(kView) / sizeof(Item)},
				  {"Rendering", kRender, sizeof(kRender) / sizeof(Item)},
				  {"Culling", kCull, sizeof(kCull) / sizeof(Item)},
				  {"Screen", kScreen, sizeof(kScreen) / sizeof(Item)},
				  {"Controls", kInput, sizeof(kInput) / sizeof(Item)},
				  {"Info", nullptr, 0}};
const int kPages = sizeof(g_pages) / sizeof(Page);

char g_infoText[kInfoLines][160];
SRWLOCK g_infoLock = SRWLOCK_INIT;

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
UiPanel g_panel = {"menu", 30.0f, true};
const float kSmall = 22.0f;  // font size of tags and hints

volatile bool g_open, g_justOpened, g_wantRecenter;
bool g_waitRelease;   // after closing: keep blocking the game's gamepad until every button is released
int g_page;
bool g_pageChanged;   // select g_page's tab on the next frames (paging buttons, opening)
bool g_focusFirst;    // put the navigation cursor on the page's first setting
bool g_focusWindow;   // the menu window takes keyboard/gamepad focus (when it opens)
bool g_uiActive;      // a slider is being adjusted: B (finish) and LB/RB (finer/coarser) go to ImGui
XINPUT_GAMEPAD g_pad; // this frame's gamepad (a real one and the VR controllers), fed to ImGui
bool g_padOk, g_foreground;
bool g_holdA;         // the A press that opened the menu: kept from ImGui until A is let go
WORD g_prevButtons;
bool g_comboLocked;   // B closed the menu: B + A can't open it again until B is let go
bool g_keyPrev[256];
const char* g_help;   // help text of the focused setting
char g_helpDefault[96];  // and its default value, "" for actions
bool g_resetPending;  // X / Delete was pressed: the focused setting goes back to its default

// ---------------------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------------------
// A key's name as the keyboard layout calls it ("Insert", "F1", ...).
void KeyName(int vk, char* out, size_t cap) {
	UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
	switch (vk) {  // keys on the extended part of the keyboard (else the name of the numpad key)
		case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
		case VK_RCONTROL: case VK_RMENU: case VK_SNAPSHOT:
			sc |= 0x100;
			break;
	}
	wchar_t w[64];
	if (vk <= 0 || !GetKeyNameTextW((LONG)(sc << 16), w, 64)) {
		snprintf(out, cap, vk > 0 ? "key 0x%X" : "(no key)", (unsigned)vk);
		return;
	}
	snprintf(out, cap, "%s", Utf8(w).s);
}

bool KeyEdge(int vk, bool fg) {
	if (vk <= 0 || vk > 255) return false;
	bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool e = d && !g_keyPrev[vk];
	g_keyPrev[vk] = d;
	return e && fg;
}
bool KeyDown(int vk) { return g_foreground && (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Open(bool on) {
	if (g_open == on) return;
	g_open = on;
	if (on) {
		g_justOpened = true;
		g_blockPad = 1;
		g_pageChanged = g_focusFirst = g_focusWindow = true;
		g_holdA = true;
		g_uiActive = false;
		Log("menu: opened");
	} else {
		g_waitRelease = true;
		Log("menu: closed");
	}
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
// After a setting was changed in the menu: log it and save it to NieRVR.ini.
void Saved(const Item& it) {
	char v[48];
	if (it.type == T_BOOL)
		snprintf(v, sizeof(v), "%s", *(int*)it.ptr ? "on" : "off");
	else if (it.type == T_INT || it.type == T_CHOICE)
		snprintf(v, sizeof(v), "%d", *(int*)it.ptr);
	else
		snprintf(v, sizeof(v), "%.3f", (double)*(float*)it.ptr);
	Log("menu: %s = %s%s", it.label, v, it.flags & F_RESTART ? " (after a restart)" : "");
	ConfigSave(it.ptr);
}

// Settings that do nothing in the current eye mode say so.
const char* Tag(const Item& it) {
	if (it.type == T_ACTION && (intptr_t)it.ptr == A_DLSS_FILE) {  // why the last download failed
		static DlssFileState f;
		DlssFileGet(&f);
		return f.state == DF_MISSING && f.text[0] ? f.text : nullptr;
	}
	if ((it.flags & F_ALTERNATE) && g_cfg.bothEyes) return "(not used: both eyes is on)";
	if ((it.flags & F_BOTHEYES) && !g_cfg.bothEyes) return "(not used: alternate eyes)";
	if (it.flags & F_RESTART) return "(restart)";
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Drawing (ImGui; runs on the VR thread every headset frame while the menu is open)
// ---------------------------------------------------------------------------------------------
ImVec4 Col(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

// The v-th name of a T_CHOICE item ("Off|DLSS").
void ChoiceName(const Item& it, int v, char* out, size_t cap) {
	snprintf(out, cap, "?");
	const char* n = it.fmt;
	for (int i = 0; n && i < v; i++) n = (n = strchr(n, '|')) ? n + 1 : nullptr;
	if (n && v >= 0) snprintf(out, cap, "%.*s", (int)strcspn(n, "|"), n);
}

// A value of a setting as the menu shows it.
void ValueText(const Item& it, float v, char* out, size_t cap) {
	switch (it.type) {
		case T_BOOL: snprintf(out, cap, "%s", v != 0 ? "On" : "Off"); return;
		case T_CHOICE: ChoiceName(it, (int)v, out, cap); return;
		case T_INT: snprintf(out, cap, it.fmt ? it.fmt : "%d", (int)v); return;
		case T_FLOAT: snprintf(out, cap, it.fmt ? it.fmt : "%.2f", (double)v); return;
		default: out[0] = 0;
	}
}

// Puts a setting back to its default (NieRVR.ini's table) and saves it.
void ResetToDefault(Item& it) {
	float d;
	if ((it.type != T_BOOL && it.type != T_INT && it.type != T_FLOAT && it.type != T_CHOICE) || !ConfigDefault(it.ptr, &d))
		return;
	if (it.type == T_FLOAT) {
		if (*(float*)it.ptr == d) return;
		*(float*)it.ptr = d;
	} else {
		if (*(int*)it.ptr == (int)d) return;
		*(int*)it.ptr = (int)d;
	}
	if (it.flags & F_RECENTER) g_wantRecenter = true;
	Saved(it);
}

// The DLSS file row: one button that shows the state and turns into the progress bar while downloading.
// Missing: download. Downloading: cancel. Installed: press twice (within 3 s) to delete.
void DlssFileButton() {
	static DWORD armedUntil;
	DlssFileState f;
	DlssFileGet(&f);
	bool armed = armedUntil && GetTickCount() - (armedUntil - 3000) < 3000;
	if (f.state != DF_INSTALLED) armedUntil = 0, armed = false;
	char label[96];
	float frac = -1;
	switch (f.state) {
		case DF_DOWNLOADING:
			frac = f.total > 0 ? (float)((double)f.got / (double)f.total) : 0;
			snprintf(label, sizeof(label), "%.0f / %.0f MB  (%.0f%%)", f.got / 1e6, f.total / 1e6,
					 (double)(frac * 100));
			break;
		case DF_DELETING: snprintf(label, sizeof(label), "Deleting..."); break;
		case DF_INSTALLED:
			snprintf(label, sizeof(label), armed ? "Press again to delete" : "%s installed: delete", f.text);
			break;
		default: snprintf(label, sizeof(label), f.text[0] ? "Try again (59 MB)" : "Download (59 MB)"); break;
	}
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
	bool pressed = ImGui::Button("##dlss", ImVec2(w, h));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (frac >= 0) {  // the button fills up as the file arrives
		const float* a = ui::Colors::accent;
		dl->AddRectFilled(pos, ImVec2(pos.x + w * frac, pos.y + h), ImGui::GetColorU32(ImVec4(a[0], a[1], a[2], 0.55f)),
						  ImGui::GetStyle().FrameRounding);
	}
	ImVec2 ts = ImGui::CalcTextSize(label);
	dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
				ImGui::GetColorU32(armed ? Col(ui::Colors::warn) : ImGui::GetStyleColorVec4(ImGuiCol_Text)), label);
	if (!pressed) return;
	switch (f.state) {
		case DF_MISSING: DlssFileDownload(); break;
		case DF_DOWNLOADING: DlssFileCancel(); break;
		case DF_INSTALLED:
			if (armed) {
				armedUntil = 0;
				DlssFileDelete();
			} else {
				armedUntil = GetTickCount() + 3000;
			}
			break;
	}
}

// The widget of one setting; true if it changed.
bool Widget(Item& it) {
	ImGui::SetNextItemWidth(-FLT_MIN);
	const ImGuiSliderFlags sf = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput |
								(it.flags & F_LOG ? ImGuiSliderFlags_Logarithmic : 0);
	switch (it.type) {
		case T_BOOL: {
			bool v = *(int*)it.ptr != 0;
			if (!ImGui::Checkbox("##v", &v)) return false;
			*(int*)it.ptr = v;
			return true;
		}
		case T_INT: {
			int v = *(int*)it.ptr;
			const char* f = it.fmt ? it.fmt : "%d";
			if (it.ptr == &g_cfg.giVolumes && v >= 128) f = "128 (all)";
			if (!ImGui::SliderInt("##v", &v, (int)it.min, (int)it.max, f, sf)) return false;
			*(int*)it.ptr = v;
			return true;
		}
		case T_FLOAT: {
			float v = *(float*)it.ptr;
			if (!ImGui::SliderFloat("##v", &v, it.min, it.max, it.fmt ? it.fmt : "%.2f", sf)) return false;
			*(float*)it.ptr = v;
			return true;
		}
		case T_CHOICE: {  // a slider over the names
			int v = *(int*)it.ptr;
			char name[64];
			ChoiceName(it, v, name, sizeof(name));
			if (!ImGui::SliderInt("##v", &v, (int)it.min, (int)it.max, name, sf)) return false;
			*(int*)it.ptr = v;
			return true;
		}
		case T_ACTION:
			if ((intptr_t)it.ptr == A_DLSS_FILE) {
				DlssFileButton();
				return false;
			}
			if (ImGui::Button("Do it", ImVec2(-FLT_MIN, 0)) && (intptr_t)it.ptr == A_RECENTER) g_wantRecenter = true;
			return false;
		default:
			return false;
	}
}

void Row(Item& it, int index) {
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(it.label);
	if (const char* tag = Tag(it)) {
		ImGui::SameLine();
		ImGui::PushFont(nullptr, kSmall);
		ImGui::TextColored(Col(ui::Colors::warn), "%s", tag);
		ImGui::PopFont();
	}
	ImGui::TableSetColumnIndex(1);
	ImGui::PushID(index);
	bool changed = Widget(it);
	if (g_focusFirst) {  // the page was just opened: start on its first setting
		ImGui::FocusItem();
		ImGui::SetNavCursorVisible(true);
		g_focusFirst = false;
	}
	bool focused = ImGui::IsItemFocused();
	bool done = it.type == T_BOOL ? changed : ImGui::IsItemDeactivatedAfterEdit();  // sliders: when let go
	if (it.type == T_BOOL) {
		ImGui::SameLine();
		ImGui::TextUnformatted(*(int*)it.ptr ? "On" : "Off");
	}
	ImGui::PopID();
	if (changed && (it.flags & F_RECENTER)) g_wantRecenter = true;
	if (done) Saved(it);
	if (focused) {
		if (g_resetPending) {
			g_resetPending = false;
			ResetToDefault(it);
		}
		g_help = it.help;
		float d;
		char v[64];
		g_helpDefault[0] = 0;
		if (it.type != T_ACTION && ConfigDefault(it.ptr, &d)) {
			ValueText(it, d, v, sizeof(v));
			snprintf(g_helpDefault, sizeof(g_helpDefault), "Default: %s  (X or Delete: back to it)", v);
		}
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(Col(ui::Colors::bar)));
	}
}

void PageContents(const Page& p, bool info) {
	if (info) {  // the Info page: lines of live text set by the other parts of the mod
		AcquireSRWLockShared(&g_infoLock);
		for (int i = 0; i < kInfoLines; i++)
			if (g_infoText[i][0]) ImGui::TextWrapped("%s", g_infoText[i]);
		ReleaseSRWLockShared(&g_infoLock);
		g_focusFirst = false;
		return;
	}
	if (!ImGui::BeginTable("items", 2, ImGuiTableFlags_None)) return;
	ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, 440);
	for (int i = 0; i < p.count; i++) Row(p.items[i], i);
	ImGui::EndTable();
}

void Draw() {
	// title and version
	ImGui::PushFont(nullptr, 42.0f);
	ImGui::TextColored(Col(ui::Colors::accent), "NieRVR settings");
	ImGui::PopFont();
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(NIERVR_VERSION).x);
	ImGui::TextDisabled(NIERVR_VERSION);

	// footer size, so the page gets the rest
	const float smallLine = kSmall * 1.35f;
	const float footer = ImGui::GetFontSize() * 1.3f * 3 + smallLine * 3 + ImGui::GetStyle().ItemSpacing.y * 6;

	g_help = nullptr;
	g_helpDefault[0] = 0;
	int shown = -1;
	if (ImGui::BeginTabBar("pages")) {
		for (int i = 0; i < kPages; i++) {
			ImGuiTabItemFlags f = g_pageChanged && i == g_page ? ImGuiTabItemFlags_SetSelected : 0;
			if (!ImGui::BeginTabItem(g_pages[i].name, nullptr, f)) continue;
			shown = i;
			ImGui::BeginChild("page", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_NavFlattened);
			if (!g_pageChanged || i == g_page) PageContents(g_pages[i], i == kPages - 1);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	// the shown tab: ours once a page change has taken effect, else the one ImGui navigation chose
	if (shown >= 0) {
		if (g_pageChanged) {
			if (shown == g_page) g_pageChanged = false;
		} else if (shown != g_page) {
			g_page = shown;
		}
	}

	// help for the focused setting, then the controls
	ImGui::Separator();
	ImGui::BeginChild("help", ImVec2(0, ImGui::GetFontSize() * 1.3f * 3 + smallLine), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
	if (g_help) ImGui::TextWrapped("%s", g_help);
	if (g_help && g_helpDefault[0]) {
		ImGui::PushFont(nullptr, kSmall);
		ImGui::TextDisabled("%s", g_helpDefault);
		ImGui::PopFont();
	}
	ImGui::EndChild();
	g_resetPending = false;  // a reset press with nothing to reset
	ImGui::Separator();
	ImGui::PushFont(nullptr, kSmall);
	ImGui::TextDisabled("Gamepad:  left stick / D-pad move   right stick scroll   A switch / adjust a slider (A again: done)");
	ImGui::TextDisabled("X default   LB/RB or triggers page   B close");
	char key[64];
	KeyName(g_cfg.keyMenu, key, sizeof(key));
	ImGui::SameLine();
	ImGui::TextDisabled("      Keyboard:  arrows   Enter   Delete   PgUp/PgDn   %s close", key);
	ImGui::PopFont();
}

// The menu's input for ImGui's navigation: the gamepad (a real one and the VR controllers) and
// the keyboard. B and LB/RB only while a slider is being adjusted (else they close the menu and
// change pages, MenuTick).
void FeedInput() {
	ImGuiIO& io = ImGui::GetIO();
	WORD b = g_padOk ? g_pad.wButtons : 0;
	if (!(b & XINPUT_GAMEPAD_A)) g_holdA = false;
	// The left stick works like the D-pad (ImGui would scroll with it; on the VR controllers the
	// thumbstick is what you move with). On past half way, off again below a third.
	static bool stUp, stDown, stLeft, stRight;
	auto dir = [](bool& on, float v) { on = on ? v > 0.33f : v > 0.5f; return on; };
	float sx = g_padOk ? (float)g_pad.sThumbLX / 32767.0f : 0.0f, sy = g_padOk ? (float)g_pad.sThumbLY / 32767.0f : 0.0f;
	dir(stUp, sy);
	dir(stDown, -sy);
	dir(stLeft, -sx);
	dir(stRight, sx);
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (b & XINPUT_GAMEPAD_DPAD_UP) || stUp);
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (b & XINPUT_GAMEPAD_DPAD_DOWN) || stDown);
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (b & XINPUT_GAMEPAD_DPAD_LEFT) || stLeft);
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & XINPUT_GAMEPAD_DPAD_RIGHT) || stRight);
	// The right stick scrolls the page (ImGui's stick scrolling, which it reads from its "left stick").
	float rx = g_padOk ? (float)g_pad.sThumbRX / 32767.0f : 0.0f, ry = g_padOk ? (float)g_pad.sThumbRY / 32767.0f : 0.0f;
	auto scroll = [&](ImGuiKey neg, ImGuiKey pos, float v) {
		io.AddKeyAnalogEvent(neg, v < -0.25f, v < -0.25f ? -v : 0.0f);
		io.AddKeyAnalogEvent(pos, v > 0.25f, v > 0.25f ? v : 0.0f);
	};
	scroll(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, rx);
	scroll(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, ry);
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (b & XINPUT_GAMEPAD_A) && !g_holdA);
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & XINPUT_GAMEPAD_B) && g_uiActive);
	io.AddKeyEvent(ImGuiKey_GamepadL1, (b & XINPUT_GAMEPAD_LEFT_SHOULDER) && g_uiActive);
	io.AddKeyEvent(ImGuiKey_GamepadR1, (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) && g_uiActive);
	io.AddKeyEvent(ImGuiKey_UpArrow, KeyDown(VK_UP));
	io.AddKeyEvent(ImGuiKey_DownArrow, KeyDown(VK_DOWN));
	io.AddKeyEvent(ImGuiKey_LeftArrow, KeyDown(VK_LEFT));
	io.AddKeyEvent(ImGuiKey_RightArrow, KeyDown(VK_RIGHT));
	io.AddKeyEvent(ImGuiKey_Enter, KeyDown(VK_RETURN));
	io.AddKeyEvent(ImGuiKey_Space, KeyDown(VK_SPACE));
	io.AddKeyEvent(ImGuiKey_Escape, KeyDown(VK_ESCAPE));
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// API (VR thread unless noted)
// ---------------------------------------------------------------------------------------------
// Any thread: one line of the Info page.
void MenuSetInfo(int line, const char* text) {
	if (line < 0 || line >= kInfoLines) return;
	AcquireSRWLockExclusive(&g_infoLock);
	snprintf(g_infoText[line], sizeof(g_infoText[line]), "%s", text);
	ReleaseSRWLockExclusive(&g_infoLock);
}

// Every headset frame: opening and closing, page buttons, and the gamepad state for ImGui.
void MenuTick(bool foreground) {
	DWORD t = GetTickCount();
	static DWORD lastHook;
	if (t - lastHook > 3000) {  // the game may load XInput late
		lastHook = t;
		InstallGamepad();
	}
	XINPUT_GAMEPAD g = {};
	bool pad = ReadGamepad(&g);
	g_pad = g;
	g_padOk = pad;
	g_foreground = foreground;
	WORD b = pad ? g.wButtons : 0;
	WORD pressed = b & ~g_prevButtons;  // newly pressed buttons
	g_prevButtons = b;
	// Triggers also change pages (VR controllers: the grips are LB/RB, which is easy to miss).
	// A pull counts once: above 160, and it must drop below 64 before it counts again.
	static bool ltHeld, rtHeld;
	bool ltPull = pad && !ltHeld && g.bLeftTrigger > 160;
	bool rtPull = pad && !rtHeld && g.bRightTrigger > 160;
	if (pad && g.bLeftTrigger > 160) ltHeld = true;
	else if (!pad || g.bLeftTrigger < 64) ltHeld = false;
	if (pad && g.bRightTrigger > 160) rtHeld = true;
	else if (!pad || g.bRightTrigger < 64) rtHeld = false;

	// open / close. Gamepad: hold B, then press A. Closing: B, Start or the menu key.
	if (!(b & XINPUT_GAMEPAD_B)) g_comboLocked = false;
	bool comboFire = !g_comboLocked && (b & XINPUT_GAMEPAD_B) && (pressed & XINPUT_GAMEPAD_A);
	bool keyToggle = KeyEdge(g_cfg.keyMenu, foreground);
	bool pgKeyL = KeyEdge(VK_PRIOR, foreground), pgKeyR = KeyEdge(VK_NEXT, foreground);
	bool resetKey = KeyEdge(VK_DELETE, foreground) | KeyEdge(VK_BACK, foreground);
	if (!g_open) {
		if (keyToggle || comboFire) Open(true);
	} else if (keyToggle || (!g_uiActive && (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)))) {
		Open(false);
		if (b & XINPUT_GAMEPAD_B) g_comboLocked = true;  // else A with B still down reopens it at once
	}
	if (!g_open) {
		if (g_waitRelease && !b) {
			g_waitRelease = false;
			g_blockPad = 0;
		}
		return;
	}
	// X / Delete: the highlighted setting back to its default (not while a slider is being adjusted)
	if (!g_uiActive && ((pressed & XINPUT_GAMEPAD_X) || resetKey)) g_resetPending = true;
	// pages (while a slider is being adjusted, LB/RB make it finer/coarser instead)
	bool pgL = pgKeyL || (!g_uiActive && ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || ltPull));
	bool pgR = pgKeyR || (!g_uiActive && ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || rtPull));
	if (pgL || pgR) {
		g_page = (g_page + (pgR ? 1 : kPages - 1)) % kPages;
		g_pageChanged = g_focusFirst = true;
	}
}

bool MenuIsOpen() { return g_open; }
// Returns whether the menu was just opened, and clears it.
bool MenuTakeJustOpened() {
	bool j = g_justOpened;
	g_justOpened = false;
	return j;
}
bool MenuTakeRecenter() {
	bool r = g_wantRecenter;
	g_wantRecenter = false;
	return r;
}

// Draws the menu into rtv (w x h pixels). False if ImGui couldn't start.
bool MenuRender(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv, int w, int h) {
	if (!UiPrepare(g_panel, dev, dc, w, h)) return false;
	FeedInput();
	UiNewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	if (g_focusWindow) {
		ImGui::SetNextWindowFocus();
		g_focusWindow = false;
	}
	ImGui::Begin("NieRVR settings", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
	Draw();
	ImGui::End();
	g_uiActive = ImGui::IsAnyItemActive();
	UiRender(dc, rtv);
	return true;
}
