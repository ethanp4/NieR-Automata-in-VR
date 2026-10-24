#include "input/vr_controllers.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "ui/menu.h"
#include "xr/openxr.h"
#include <stdio.h>
#include <string.h>
#include <xinput.h>

namespace {
#define VR_FUNCS(X)                        \
	X(xrStringToPath)                      \
	X(xrCreateActionSet)                   \
	X(xrDestroyActionSet)                  \
	X(xrCreateAction)                      \
	X(xrSuggestInteractionProfileBindings) \
	X(xrAttachSessionActionSets)           \
	X(xrSyncActions)                       \
	X(xrGetActionStateBoolean)             \
	X(xrGetActionStateFloat)               \
	X(xrGetActionStateVector2f)            \
	X(xrApplyHapticFeedback)               \
	X(xrStopHapticFeedback)                \
	X(xrGetCurrentInteractionProfile)      \
	X(xrPathToString)                      \
	X(xrCreateActionSpace)                 \
	X(xrLocateSpace)                       \
	X(xrDestroySpace)
struct {
#define DECL(n) PFN_##n n;
	VR_FUNCS(DECL)
#undef DECL
} F;

enum Act { A_STICK,
		   A_CLICK,
		   A_TRIGGER,
		   A_GRIP,
		   A_A,
		   A_B,
		   A_MENU,
		   A_HAPTIC,
		   A_POSE,
		   A_COUNT };
XrInstance s_inst;
XrActionSet s_set;
XrAction s_act[A_COUNT];
XrPath s_hand[2];
XrSession s_session;
bool s_ready, s_attached;
XrSpace s_handSpace[2];  // the grip pose of each hand (motion controls)

// shared with the game thread
SRWLOCK s_lock = SRWLOCK_INIT;
XINPUT_GAMEPAD s_pad;
DWORD s_padTick;       // when s_pad was last updated with an active controller
bool s_anyActive, s_everActive;  // everActive: for the log
volatile LONG s_hapL, s_hapR;  // 0..65535 from XInputSetState
float s_lastHap[2];
DWORD s_hapTick[2];

pure::MenuButton s_menuBtn;  // Start / Back / D-pad from the left menu button

// motion controls
pure::SwingDetector s_swing[2];
// pure::HeadZone g_headZone[2];
DWORD s_swingUntil[2];        // the attack button is held until then (100 ms per swing)
XrVector3f s_lastPos[2], s_lastHead;  // for speeds when the runtime gives no velocity
XrTime s_lastTime;
bool s_lastValid;
unsigned s_swings[2];
float s_peak[2];
DWORD s_peakTick;

// Keyboard / mouse handoff (0.4.1): a key or mouse button pressed in the game pauses the VR
// controllers (the game sees a neutral pad, no rumble) until a button, trigger or stick on them is
// used on purpose. Controllers strapped to your hands while you type (Index) send nothing then.
bool s_kbmMode;
bool s_keyWas[256];
WORD s_prevVrButtons;
float s_prevTrig[2];
unsigned s_kbmSwitches, s_vrSwitches;

bool HandoffKey(int vk) {
	if (vk >= VK_F1 && vk <= VK_F24) return false;  // the mod's hotkeys
	switch (vk) {
		case VK_INSERT:  // settings menu
		case VK_SHIFT:
		case VK_CONTROL:
		case VK_MENU:
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_LMENU:
		case VK_RMENU:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
		case VK_SNAPSHOT:
		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			return false;
	}
	return true;
}
// true when a key or mouse button was newly pressed while the game has focus
bool KeyboardMouseUsed(bool foreground) {
	bool used = false;
	for (int vk = 1; vk < 256; vk++) {
		if (vk == VK_CANCEL || vk == 0x07) continue;
		bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (d && !s_keyWas[vk] && foreground && HandoffKey(vk)) used = true;
		s_keyWas[vk] = d;
	}
	return used;
}
char s_profile[2][128];

XrPath Path(const char* s) {
	XrPath p = XR_NULL_PATH;
	F.xrStringToPath(s_inst, s, &p);
	return p;
}

bool MakeAction(Act a, XrActionType type, const char* name, const char* loc) {
	XrActionCreateInfo ci = {};
	ci.type = XR_TYPE_ACTION_CREATE_INFO;
	ci.actionType = type;
	lstrcpynA(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE);
	lstrcpynA(ci.localizedActionName, loc, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
	ci.countSubactionPaths = 2;
	ci.subactionPaths = s_hand;
	return XR_SUCCEEDED(F.xrCreateAction(s_set, &ci, &s_act[a]));
}

struct Bind {
	Act a;
	const char* path;
};
void Suggest(const char* profile, const Bind* b, int n) {
	XrActionSuggestedBinding sb[40];
	int k = 0;
	for (int i = 0; i < n && k < 40; i++) {
		XrPath p = Path(b[i].path);
		if (p != XR_NULL_PATH) sb[k++] = {s_act[b[i].a], p};
	}
	XrInteractionProfileSuggestedBinding s = {};
	s.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	s.interactionProfile = Path(profile);
	s.countSuggestedBindings = (uint32_t)k;
	s.suggestedBindings = sb;
	XrResult r = F.xrSuggestInteractionProfileBindings(s_inst, &s);
	if (XR_FAILED(r)) Log("vr controllers: bindings for %s refused (%d)", profile, (int)r);
}

#define L "/user/hand/left/"
#define R "/user/hand/right/"
const Bind kTouch[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"}, {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"}, {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"}, {A_GRIP, L "input/squeeze/value"}, {A_GRIP, R "input/squeeze/value"}, {A_A, L "input/x/click"}, {A_A, R "input/a/click"}, {A_B, L "input/y/click"}, {A_B, R "input/b/click"}, {A_MENU, L "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}, {A_POSE, L "input/grip/pose"}, {A_POSE, R "input/grip/pose"}};
const Bind kIndex[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"}, {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"}, {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"}, {A_GRIP, L "input/squeeze/value"}, {A_GRIP, R "input/squeeze/value"}, {A_A, L "input/a/click"}, {A_A, R "input/a/click"}, {A_B, L "input/b/click"}, {A_B, R "input/b/click"}, {A_MENU, L "input/trackpad/force"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}, {A_POSE, L "input/grip/pose"}, {A_POSE, R "input/grip/pose"}};
const Bind kPico[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"}, {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"}, {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"}, {A_GRIP, L "input/squeeze/value"}, {A_GRIP, R "input/squeeze/value"}, {A_A, L "input/x/click"}, {A_A, R "input/a/click"}, {A_B, L "input/y/click"}, {A_B, R "input/b/click"}, {A_MENU, L "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}, {A_POSE, L "input/grip/pose"}, {A_POSE, R "input/grip/pose"}};
// Vive wands have few buttons: right trackpad click = A, right menu = B, left trackpad click = L3.
const Bind kVive[] = {{A_STICK, L "input/trackpad"}, {A_STICK, R "input/trackpad"}, {A_CLICK, L "input/trackpad/click"}, {A_A, R "input/trackpad/click"}, {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"}, {A_GRIP, L "input/squeeze/click"}, {A_GRIP, R "input/squeeze/click"}, {A_MENU, L "input/menu/click"}, {A_B, R "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}, {A_POSE, L "input/grip/pose"}, {A_POSE, R "input/grip/pose"}};
const Bind kWmr[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"}, {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"}, {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"}, {A_GRIP, L "input/squeeze/click"}, {A_GRIP, R "input/squeeze/click"}, {A_A, L "input/trackpad/click"}, {A_A, R "input/trackpad/click"}, {A_MENU, L "input/menu/click"}, {A_B, R "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}, {A_POSE, L "input/grip/pose"}, {A_POSE, R "input/grip/pose"}};
const Bind kSimple[] = {{A_TRIGGER, L "input/select/click"}, {A_TRIGGER, R "input/select/click"}, {A_MENU, L "input/menu/click"}, {A_B, R "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}, {A_POSE, L "input/grip/pose"}, {A_POSE, R "input/grip/pose"}};
#undef L
#undef R

bool GetBool(Act a, int hand) {
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, s_act[a], s_hand[hand]};
	XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(F.xrGetActionStateBoolean(s_session, &gi, &s)) && s.isActive && s.currentState;
}
float GetFloat(Act a, int hand, bool* active = nullptr) {
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, s_act[a], s_hand[hand]};
	XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
	bool ok = XR_SUCCEEDED(F.xrGetActionStateFloat(s_session, &gi, &s)) && s.isActive;
	if (active) *active = ok;
	return ok ? s.currentState : 0;
}
XrVector2f GetStick(int hand, bool* active) {
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, s_act[A_STICK], s_hand[hand]};
	XrActionStateVector2f s = {XR_TYPE_ACTION_STATE_VECTOR2F};
	bool ok = XR_SUCCEEDED(F.xrGetActionStateVector2f(s_session, &gi, &s)) && s.isActive;
	*active = ok;
	return ok ? s.currentState : XrVector2f{0, 0};
}
SHORT Axis(float v) {
	if (v > 1) v = 1;
	if (v < -1) v = -1;
	return (SHORT)(v * 32767.0f);
}
// A resting controller can report a little stick or trigger; that alone shouldn't make the game
// switch its button prompts away from the keyboard.
XrVector2f StickDead(XrVector2f v) {
	if (v.x * v.x + v.y * v.y < 0.06f * 0.06f) return {0, 0};
	return v;
}
float TriggerDead(float v) { return v < 0.04f ? 0 : v; }

void Haptics() {
	float amp[2] = {(float)s_hapL / 65535.0f, (float)s_hapR / 65535.0f};
	for (int h = 0; h < 2; h++) {
		XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO, nullptr, s_act[A_HAPTIC], s_hand[h]};
		DWORD t = GetTickCount();
		if (amp[h] > 0.02f) {
			// renew a 100 ms pulse every 50 ms, or at once when the strength changes
			float d = amp[h] - s_lastHap[h];
			if (d < 0.05f && d > -0.05f && t - s_hapTick[h] < 50) continue;
			s_hapTick[h] = t;
			XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
			v.amplitude = amp[h];
			v.duration = 100 * 1000 * 1000;  // 100 ms (ns)
			v.frequency = XR_FREQUENCY_UNSPECIFIED;
			F.xrApplyHapticFeedback(s_session, &hi, (const XrHapticBaseHeader*)&v);
		} else if (s_lastHap[h] > 0.02f) {
			F.xrStopHapticFeedback(s_session, &hi);
		}
		s_lastHap[h] = amp[h];
	}
}

// Which controllers the runtime bound: read once after the actions are attached and again whenever
// the runtime reports XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED (0.5.0; was polled every 2 s).
bool s_profileDirty = true;
void LogProfiles() {
	if (!s_profileDirty) return;
	s_profileDirty = false;
	for (int h = 0; h < 2; h++) {
		XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
		if (XR_FAILED(F.xrGetCurrentInteractionProfile(s_session, s_hand[h], &st))) continue;
		char name[128] = "(none)";
		uint32_t n = 0;
		if (st.interactionProfile != XR_NULL_PATH)
			F.xrPathToString(s_inst, st.interactionProfile, sizeof(name), &n, name);
		if (lstrcmpA(name, s_profile[h])) {
			lstrcpynA(s_profile[h], name, sizeof(s_profile[h]));
			Log("vr controllers: %s hand uses %s", h ? "right" : "left", name);
			char info[160];
			const char* a = s_profile[0][0] ? s_profile[0] : "(none)";
			const char* b = s_profile[1][0] ? s_profile[1] : "(none)";
			if (strstr(a, "/interaction_profiles/") == a) a += 22;
			if (strstr(b, "/interaction_profiles/") == b) b += 22;
			snprintf(info, sizeof(info), "VR controllers: left %s, right %s", a, b);
			MenuSetInfo(11, info);
		}
	}
}
}  // namespace

// After xrCreateInstance: actions and suggested bindings (must come before the session attaches).
void VrInputInit(PFN_xrGetInstanceProcAddr gipa, XrInstance inst, bool pico) {
	s_ready = false;
	s_inst = inst;
	bool ok = true;
#define LOAD(n) \
	if (XR_FAILED(gipa(inst, #n, (PFN_xrVoidFunction*)&F.n)) || !F.n) ok = false;
	VR_FUNCS(LOAD)
#undef LOAD
	if (!ok) {
		Log("vr controllers: OpenXR input functions missing; controllers not used");
		return;
	}
	s_hand[0] = Path("/user/hand/left");
	s_hand[1] = Path("/user/hand/right");
	XrActionSetCreateInfo si = {};
	si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	lstrcpynA(si.actionSetName, "gamepad", XR_MAX_ACTION_SET_NAME_SIZE);
	lstrcpynA(si.localizedActionSetName, "Gamepad", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
	if (XR_FAILED(F.xrCreateActionSet(inst, &si, &s_set))) {
		Log("vr controllers: could not create the action set");
		return;
	}
	ok = MakeAction(A_STICK, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Stick") &&
		 MakeAction(A_CLICK, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick click") &&
		 MakeAction(A_TRIGGER, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") &&
		 MakeAction(A_GRIP, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip") &&
		 MakeAction(A_A, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_lower", "A / X") &&
		 MakeAction(A_B, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_upper", "B / Y") &&
		 MakeAction(A_MENU, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu (Start / Back / D-pad)") &&
		 MakeAction(A_HAPTIC, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble") &&
		 MakeAction(A_POSE, XR_ACTION_TYPE_POSE_INPUT, "hand", "Hand position (motion controls)");
	if (!ok) {
		Log("vr controllers: could not create the actions");
		return;
	}
	Suggest("/interaction_profiles/oculus/touch_controller", kTouch, sizeof(kTouch) / sizeof(Bind));
	Suggest("/interaction_profiles/valve/index_controller", kIndex, sizeof(kIndex) / sizeof(Bind));
	Suggest("/interaction_profiles/htc/vive_controller", kVive, sizeof(kVive) / sizeof(Bind));
	Suggest("/interaction_profiles/microsoft/motion_controller", kWmr, sizeof(kWmr) / sizeof(Bind));
	if (pico) Suggest("/interaction_profiles/bytedance/pico4_controller", kPico, sizeof(kPico) / sizeof(Bind));
	Suggest("/interaction_profiles/khr/simple_controller", kSimple, sizeof(kSimple) / sizeof(Bind));
	s_ready = true;
	Log("vr controllers: ready (VRControllers=%d)", g_cfg.vrControllers);
}

void VrInputAttach(XrSession s) {
	s_session = s;
	s_attached = false;
	if (!s_ready) return;
	XrSessionActionSetsAttachInfo ai = {};
	ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	ai.countActionSets = 1;
	ai.actionSets = &s_set;
	s_attached = XR_SUCCEEDED(F.xrAttachSessionActionSets(s, &ai));
	s_profileDirty = true;
	if (!s_attached) {
		Log("vr controllers: could not attach the actions to the session");
		return;
	}
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo ci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		ci.action = s_act[A_POSE];
		ci.subactionPath = s_hand[h];
		ci.poseInActionSpace.orientation.w = 1;
		if (XR_FAILED(F.xrCreateActionSpace(s, &ci, &s_handSpace[h]))) {
			s_handSpace[h] = XR_NULL_HANDLE;
			Log("vr controllers: no %s hand position (motion controls won't work)", h ? "right" : "left");
		}
	}
	s_lastValid = false;
}

void VrInputDetach() {
	for (int h = 0; h < 2; h++) {
		if (s_handSpace[h]) F.xrDestroySpace(s_handSpace[h]);
		s_handSpace[h] = XR_NULL_HANDLE;
		s_swingUntil[h] = 0;
	}
	s_attached = false;
	s_session = XR_NULL_HANDLE;
	s_lastHap[0] = s_lastHap[1] = 0;
	s_profile[0][0] = s_profile[1][0] = 0;
	AcquireSRWLockExclusive(&s_lock);
	s_anyActive = s_everActive = false;
	ReleaseSRWLockExclusive(&s_lock);
}

void VrInputShutdown() {
	VrInputDetach();
	if (s_set) F.xrDestroyActionSet(s_set);  // destroys its actions too
	s_set = XR_NULL_HANDLE;
	s_ready = false;
}

// VR thread, from the OpenXR event loop: the controllers in use changed.
void VrInputProfileChanged() { s_profileDirty = true; }

// Each VR frame while the session is focused.
void VrInputSync(bool focused, bool foreground) {
	bool kbm = KeyboardMouseUsed(foreground);  // polled every frame so key edges aren't missed
	if (!s_attached) return;
	XrActiveActionSet as = {s_set, XR_NULL_PATH};
	XrActionsSyncInfo si = {};
	si.type = XR_TYPE_ACTIONS_SYNC_INFO;
	si.countActiveActionSets = 1;
	si.activeActionSets = &as;
	if (XR_FAILED(F.xrSyncActions(s_session, &si))) return;
	LogProfiles();
	bool la = false, ra = false;
	XrVector2f ls = StickDead(GetStick(0, &la)), rs = StickDead(GetStick(1, &ra));
	bool lt = false, rt = false;
	float ltv = TriggerDead(GetFloat(A_TRIGGER, 0, &lt)), rtv = TriggerDead(GetFloat(A_TRIGGER, 1, &rt));
	bool active = la || ra || lt || rt;
	XINPUT_GAMEPAD p = {};
	WORD b = 0;
	if (GetBool(A_A, 1)) b |= XINPUT_GAMEPAD_A;
	if (GetBool(A_B, 1)) b |= XINPUT_GAMEPAD_B;
	if (GetBool(A_A, 0)) b |= XINPUT_GAMEPAD_X;
	if (GetBool(A_B, 0)) b |= XINPUT_GAMEPAD_Y;
	if (GetBool(A_CLICK, 0)) b |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (GetBool(A_CLICK, 1)) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
	if (GetFloat(A_GRIP, 0) > 0.5f) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (GetFloat(A_GRIP, 1) > 0.5f) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
	p.bLeftTrigger = (BYTE)(ltv * 255.0f + 0.5f);
	p.bRightTrigger = (BYTE)(rtv * 255.0f + 0.5f);
	// left menu button: tap = Start, hold = Back, + left stick = D-pad (pure.h, test_vr_controllers)
	DWORD t = GetTickCount();
	b |= s_menuBtn.update(GetBool(A_MENU, 0), &ls.x, &ls.y, t);
	if ((LONG)(s_swingUntil[1] - t) > 0) b |= XINPUT_GAMEPAD_X;  // motion controls (VrInputMotion)
	if ((LONG)(s_swingUntil[0] - t) > 0) b |= XINPUT_GAMEPAD_Y;
	// handoff between keyboard/mouse and the VR controllers
	{
		WORD pressed = b & ~s_prevVrButtons & ~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
		bool trig = (ltv > 0.6f && s_prevTrig[0] <= 0.6f) || (rtv > 0.6f && s_prevTrig[1] <= 0.6f);
		bool stick = ls.x * ls.x + ls.y * ls.y > 0.7f * 0.7f || rs.x * rs.x + rs.y * rs.y > 0.7f * 0.7f;
		s_prevVrButtons = b;
		s_prevTrig[0] = ltv;
		s_prevTrig[1] = rtv;
		if (kbm && !s_kbmMode) {
			s_kbmMode = true;
			s_kbmSwitches++;
			if (s_kbmSwitches <= 5 && g_cfg.vrControllers)
				Log("vr controllers: keyboard/mouse in use - VR controllers paused until you use them again%s",
					s_kbmSwitches == 5 ? " (not logged again)" : "");
		} else if (s_kbmMode && !kbm && (pressed || trig || stick)) {
			s_kbmMode = false;
			s_vrSwitches++;
			if (s_vrSwitches <= 5 && g_cfg.vrControllers) Log("vr controllers: in use again");
		}
	}
	p.wButtons = b;
	p.sThumbLX = Axis(ls.x);
	p.sThumbLY = Axis(ls.y);
	p.sThumbRX = Axis(rs.x);
	p.sThumbRY = Axis(rs.y);
	AcquireSRWLockExclusive(&s_lock);
	s_pad = p;
	s_anyActive = active;
	if (active) {
		s_padTick = t;
		if (!s_everActive) Log("vr controllers: in use as gamepad %s", g_cfg.vrControllers ? "(on)" : "(off in the settings)");
		s_everActive = true;
	}
	ReleaseSRWLockExclusive(&s_lock);
	if (g_cfg.vrControllers && g_cfg.vrRumble && !s_kbmMode) Haptics();
	else if (s_lastHap[0] > 0.02f || s_lastHap[1] > 0.02f) {
		InterlockedExchange(&s_hapL, 0);
		InterlockedExchange(&s_hapR, 0);
		Haptics();  // stops them
	}
}

// Game thread (XInput hook): the controllers' gamepad state. Returns false when there is no VR
// gamepad (setting off, or the mod's VR off). Otherwise pad 0 is connected from the start, even
// before the headset session runs or while the controllers sleep (a neutral pad then): the game
// never sees a pad come and go, and finds it even if it only looks for pads at startup.
// The settings menu reads them even when they are off for the game, so they can be turned back on.
bool VrInputPad(XINPUT_GAMEPAD* out, bool evenIfOff) {
	memset(out, 0, sizeof(*out));
	if ((!g_cfg.vrControllers && !evenIfOff) || !g_cfg.enabled) return false;
	AcquireSRWLockShared(&s_lock);
	if (s_attached && s_anyActive && GetTickCount() - s_padTick < 300 && (evenIfOff || !s_kbmMode)) *out = s_pad;
	ReleaseSRWLockShared(&s_lock);
	return true;
}

// Game thread (XInputSetState hook): rumble.
void VrInputRumble(WORD left, WORD right) {
	InterlockedExchange(&s_hapL, left);
	InterlockedExchange(&s_hapR, right);
}

namespace {
bool Locate(XrSpace space, XrSpace base, XrTime time, XrVector3f* pos, XrVector3f* vel, bool* velValid) {
	XrSpaceVelocity sv = {XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION, &sv};
	if (XR_FAILED(F.xrLocateSpace(space, base, time, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		return false;
	*pos = loc.pose.position;
	*vel = sv.linearVelocity;
	*velValid = (sv.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
	return true;
}
float Length(XrVector3f v) { return pure::fsqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
XrVector3f Sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// A short pulse on one hand: the swing registered.
void Tick(int hand) {
	XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO, nullptr, s_act[A_HAPTIC], s_hand[hand]};
	XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
	v.amplitude = 0.6f;
	v.duration = 30 * 1000 * 1000;  // 30 ms (ns)
	v.frequency = XR_FREQUENCY_UNSPECIFIED;
	F.xrApplyHapticFeedback(s_session, &hi, (const XrHapticBaseHeader*)&v);
}
}  // namespace

// Each VR frame after VrInputSync: motion controls. Hand speeds relative to the head (both located
// in `local` at the frame's display time); a swing holds the attack button for 100 ms.
void VrInputMotion(XrSpace local, XrSpace view, XrTime time, bool focused, bool flipped) {
	if (!s_attached || !focused || !s_handSpace[0] || !s_handSpace[1] || !local || !view) return;
	XrVector3f head, headVel, pos[2], vel[2];
	bool headVelOk, velOk[2];
	if (!Locate(view, local, time, &head, &headVel, &headVelOk)) return;
	bool have[2];
	for (int h = 0; h < 2; h++) have[h] = Locate(s_handSpace[h], local, time, &pos[h], &vel[h], &velOk[h]);
	// no velocity from the runtime: from the positions since the last frame
	double dt = s_lastValid ? (double)(time - s_lastTime) / 1e9 : 0;
	bool diffOk = dt > 0.001 && dt < 0.1;
	if (!headVelOk) headVel = diffOk ? XrVector3f{(float)((head.x - s_lastHead.x) / dt), (float)((head.y - s_lastHead.y) / dt),
												  (float)((head.z - s_lastHead.z) / dt)}
									 : XrVector3f{0, 0, 0};
	DWORD t = GetTickCount();
	for (int h = 0; h < 2; h++) {
		if (!have[h]) continue;
		if (!velOk[h]) {
			if (!diffOk) continue;
			vel[h] = {(float)((pos[h].x - s_lastPos[h].x) / dt), (float)((pos[h].y - s_lastPos[h].y) / dt),
					  (float)((pos[h].z - s_lastPos[h].z) / dt)};
		}
		XrVector3f rel = Sub(vel[h], headVel);
		float speed = Length(rel);
		if (speed > s_peak[h]) s_peak[h] = speed;
		bool waggleEnable = g_cfg.motionControls && g_cfg.vrControllers && !MenuIsOpen() && !s_kbmMode;
		if (s_swing[h].update(waggleEnable ? speed : 0, t, g_cfg.swingSpeed) && waggleEnable) {
			s_swingUntil[h] = t + 100;
			s_swings[h]++;
			if (s_swings[0] + s_swings[1] == 1) Log("motion controls: first swing (%s hand, %.1f m/s)", h ? "right" : "left", (double)speed);
			LogV("motion controls: %s swing %.1f m/s", h ? "right" : "left", (double)speed);
			Tick(h);
			AcquireSRWLockExclusive(&s_lock);  // into the pad at once (VrInputSync adds it from then on)
			s_pad.wButtons |= h ? XINPUT_GAMEPAD_X : XINPUT_GAMEPAD_Y;
			s_padTick = t;
			ReleaseSRWLockExclusive(&s_lock);
		}
	}
	for (int h = 0; h < 2; h++) s_lastPos[h] = pos[h];
	s_lastHead = head;
	s_lastTime = time;
	s_lastValid = have[0] && have[1];
	// the Info page: swings so far and the fastest hand speeds of the last second
	if (t - s_peakTick >= 1000) {
		s_peakTick = t;
		char info[160];
		snprintf(info, sizeof(info), "Motion controls %s: swings left %u, right %u; fastest last second %.1f / %.1f m/s (needs %.1f)",
				 g_cfg.motionControls ? "on" : "off", s_swings[0], s_swings[1], (double)s_peak[0], (double)s_peak[1],
				 (double)g_cfg.swingSpeed);
		MenuSetInfo(15, info);
		s_peak[0] = s_peak[1] = 0;
	}
}
