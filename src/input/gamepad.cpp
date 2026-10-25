// gamepad.cpp - XInput hooks: the VR controllers as pad 0, the game's rumble to them, and the pad blocked while the menu is open.
#include "core/hook.h"
#include "core/log.h"
#include "input/vr_controllers.h"
#include <atomic>
#include <string.h>
#include "input/gamepad.h"

namespace {
// ---------------------------------------------------------------------------------------------
// XInput
// ---------------------------------------------------------------------------------------------
// The same hooks also hand the VR controllers to the game as the first gamepad (vr_controllers.cpp):
// their state is merged into what pad 0 reports (a real pad keeps working), pad 0 reads as
// connected while the VR controllers are in use, and the game's rumble goes to them.
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
struct XInputHook {
	const wchar_t* dll;
	PFN_XInputGetState getState, getStateEx;  // the originals
	PFN_XInputGetCapabilities getCapabilities;
	PFN_XInputSetState setState;
	bool hooked;
};
XInputHook s_xinputDlls[] = {{L"xinput1_4.dll"}, {L"xinput1_3.dll"}, {L"xinput9_1_0.dll"}, {L"xinput1_2.dll"}, {L"xinput1_1.dll"}};
PFN_XInputGetState s_readPadState;  // for our own reading (an original, never blocked)

// Adds the VR controllers' state to a gamepad state: buttons combined, the stronger trigger and
// the stick that is pushed further.
void MergeVrPad(XINPUT_GAMEPAD& pad, const XINPUT_GAMEPAD& vr) {
	pad.wButtons |= vr.wButtons;
	if (vr.bLeftTrigger > pad.bLeftTrigger) pad.bLeftTrigger = vr.bLeftTrigger;
	if (vr.bRightTrigger > pad.bRightTrigger) pad.bRightTrigger = vr.bRightTrigger;
	auto lengthSq = [](int x, int y) { return (int64_t)x * x + (int64_t)y * y; };
	if (lengthSq(vr.sThumbLX, vr.sThumbLY) > lengthSq(pad.sThumbLX, pad.sThumbLY)) pad.sThumbLX = vr.sThumbLX, pad.sThumbLY = vr.sThumbLY;
	if (lengthSq(vr.sThumbRX, vr.sThumbRY) > lengthSq(pad.sThumbRX, pad.sThumbRY)) pad.sThumbRX = vr.sThumbRX, pad.sThumbRY = vr.sThumbRY;
}

DWORD FilterState(DWORD user, DWORD result, XINPUT_STATE* state) {
	if (!state) return result;
	XINPUT_GAMEPAD vr;
	if (user == 0 && VrInputPad(&vr)) {
		if (result != ERROR_SUCCESS) {  // no real pad: the VR controllers are pad 0
			*state = {};
			result = ERROR_SUCCESS;
		}
		MergeVrPad(state->Gamepad, vr);
		// games skip a state whose packet number didn't change: count changes of the merged state
		static XINPUT_GAMEPAD lastMerged;
		static DWORD packetNumber = 1;
		if (memcmp(&lastMerged, &state->Gamepad, sizeof(lastMerged))) {
			lastMerged = state->Gamepad;
			packetNumber++;
		}
		state->dwPacketNumber = packetNumber;
	}
	// Block input when mod menu is open
	if (result == ERROR_SUCCESS && g_blockPad) state->Gamepad = {};
	return result;
}
template <int I>
DWORD WINAPI H_XInputGetState(DWORD user, XINPUT_STATE* state) { return FilterState(user, s_xinputDlls[I].getState(user, state), state); }
template <int I>
DWORD WINAPI H_XInputGetStateEx(DWORD user, XINPUT_STATE* state) { return FilterState(user, s_xinputDlls[I].getStateEx(user, state), state); }
template <int I>
DWORD WINAPI H_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
	DWORD result = s_xinputDlls[I].getCapabilities(user, flags, caps);
	XINPUT_GAMEPAD vr;
	if (result != ERROR_SUCCESS && user == 0 && caps && VrInputPad(&vr)) {
		*caps = {};  // describe an ordinary Xbox controller
		caps->Type = XINPUT_DEVTYPE_GAMEPAD;
		caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
		caps->Gamepad.wButtons = 0xF3FF;
		caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
		caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
		caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFFFF;
		result = ERROR_SUCCESS;
	}
	return result;
}
template <int I>
DWORD WINAPI H_XInputSetState(DWORD user, XINPUT_VIBRATION* vib) {
	DWORD result = s_xinputDlls[I].setState(user, vib);
	if (user == 0 && vib) {
		VrInputRumble(vib->wLeftMotorSpeed, vib->wRightMotorSpeed);
		XINPUT_GAMEPAD vr;
		if (result != ERROR_SUCCESS && VrInputPad(&vr)) result = ERROR_SUCCESS;
	}
	return result;
}

// Hook XInput calls to add VR controller input and block input while in mod menu
template <int I>
void HookXInputDLL() {
	XInputHook& entry = s_xinputDlls[I];
	if (entry.hooked) return;
	HMODULE module = GetModuleHandleW(entry.dll);
	if (!module) return;
	entry.hooked = true;
	void* getStateFn = (void*)GetProcAddress(module, "XInputGetState");
	void* getStateExFn = (void*)GetProcAddress(module, (LPCSTR)100);  // XInputGetStateEx (also reports the Guide button)
	bool ok = getStateFn && HookFunction(getStateFn, (void*)&H_XInputGetState<I>, (void**)&entry.getState, "XInputGetState");
	if (getStateExFn && getStateExFn != getStateFn) HookFunction(getStateExFn, (void*)&H_XInputGetStateEx<I>, (void**)&entry.getStateEx, "XInputGetStateEx");
	void* capsFn = (void*)GetProcAddress(module, "XInputGetCapabilities");
	void* setStateFn = (void*)GetProcAddress(module, "XInputSetState");
	bool okCaps = capsFn && HookFunction(capsFn, (void*)&H_XInputGetCapabilities<I>, (void**)&entry.getCapabilities, "XInputGetCapabilities");
	bool okSet = setStateFn && HookFunction(setStateFn, (void*)&H_XInputSetState<I>, (void**)&entry.setState, "XInputSetState");
	if (ok && !s_readPadState) s_readPadState = entry.getState;
	Log("menu: gamepad input of %s %s%s", Utf8(entry.dll).s,
		ok ? "hooked (blocked from the game while the menu is open; VR controllers merged into pad 0)" : "could not be hooked",
		ok && !(okCaps && okSet) ? " - capabilities/rumble not hooked" : "");
}
// Get inputs from a real controller
bool ReadPhysicalPad(XINPUT_GAMEPAD* pad) {
	if (!s_readPadState) {
		static bool tried;
		if (!tried) {
			tried = true;
			HMODULE module = LoadLibraryW(L"xinput1_4.dll");
			if (!module) module = LoadLibraryW(L"xinput1_3.dll");
			if (module) s_readPadState = (PFN_XInputGetState)(void*)GetProcAddress(module, "XInputGetState");
		}
		if (!s_readPadState) return false;
	}
	for (DWORD user = 0; user < 4; user++) {
		XINPUT_STATE state = {};
		if (s_readPadState(user, &state) == ERROR_SUCCESS) {
			*pad = state.Gamepad;
			return true;
		}
	}
	return false;
}
}  // namespace

std::atomic<bool> g_blockPad;

// There are 5 xinput dlls to hook
void InstallGamepad() {
	HookXInputDLL<0>();
	HookXInputDLL<1>();
	HookXInputDLL<2>();
	HookXInputDLL<3>();
	HookXInputDLL<4>();
}

// Call ReadPhysicalPad, then get VR controller inputs and return the two combined
// Only called from the mod menu
bool ReadGamepad(XINPUT_GAMEPAD* out) {
	bool found = ReadPhysicalPad(out);
	if (!found) *out = {};
	XINPUT_GAMEPAD vr;
	if (VrInputPad(&vr, true)) {
		MergeVrPad(*out, vr);
		found = true;
	}
	return found;
}
