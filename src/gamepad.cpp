#include "common.h"
#include "gamepad.h"

namespace {
// ---------------------------------------------------------------------------------------------
// XInput
// ---------------------------------------------------------------------------------------------
// The same hooks also hand the VR controllers to the game as the first gamepad (vrinput.cpp,
// 0.4.0): their state is merged into what pad 0 reports (a real pad keeps working), pad 0 reads as
// connected while the VR controllers are in use, and the game's rumble goes to them.
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
struct PadHook {
    const wchar_t* dll;
    PFN_XInputGetState orig, origEx;
    PFN_XInputGetCapabilities origCaps;
    PFN_XInputSetState origSet;
    bool done;
};
PadHook g_pads[] = {{L"xinput1_4.dll"}, {L"xinput1_3.dll"}, {L"xinput9_1_0.dll"}, {L"xinput1_2.dll"}, {L"xinput1_1.dll"}};
PFN_XInputGetState g_ReadGamepad;  // for our own reading (an original, never blocked)

// Adds the VR controllers' state to a gamepad state: buttons combined, the stronger trigger and
// the stick that is pushed further.
void MergeVr(XINPUT_GAMEPAD& g, const XINPUT_GAMEPAD& v) {
    g.wButtons |= v.wButtons;
    if (v.bLeftTrigger > g.bLeftTrigger) g.bLeftTrigger = v.bLeftTrigger;
    if (v.bRightTrigger > g.bRightTrigger) g.bRightTrigger = v.bRightTrigger;
    auto mag = [](int x, int y) { return (long long)x * x + (long long)y * y; };
    if (mag(v.sThumbLX, v.sThumbLY) > mag(g.sThumbLX, g.sThumbLY)) g.sThumbLX = v.sThumbLX, g.sThumbLY = v.sThumbLY;
    if (mag(v.sThumbRX, v.sThumbRY) > mag(g.sThumbRX, g.sThumbRY)) g.sThumbRX = v.sThumbRX, g.sThumbRY = v.sThumbRY;
}

DWORD Filter(DWORD user, DWORD r, XINPUT_STATE* s) {
    if (!s) return r;
    XINPUT_GAMEPAD v;
    if (user == 0 && VrInputPad(&v)) {
        if (r != ERROR_SUCCESS) {  // no real pad: the VR controllers are pad 0
            memset(s, 0, sizeof(*s));
            r = ERROR_SUCCESS;
        }
        MergeVr(s->Gamepad, v);
        // games skip a state whose packet number didn't change: count changes of the merged state
        static XINPUT_GAMEPAD last;
        static DWORD packet = 1;
        if (memcmp(&last, &s->Gamepad, sizeof(last))) {
            last = s->Gamepad;
            packet++;
        }
        s->dwPacketNumber = packet;
    }
    // Block input when mod menu is open
    if (r == ERROR_SUCCESS && g_blockPad) memset(&s->Gamepad, 0, sizeof(s->Gamepad));
    return r;
}
template <int I>
DWORD WINAPI H_XInputGetState(DWORD user, XINPUT_STATE* s) { return Filter(user, g_pads[I].orig(user, s), s); }
template <int I>
DWORD WINAPI H_XInputGetStateEx(DWORD user, XINPUT_STATE* s) { return Filter(user, g_pads[I].origEx(user, s), s); }
template <int I>
DWORD WINAPI H_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* c) {
    DWORD r = g_pads[I].origCaps(user, flags, c);
    XINPUT_GAMEPAD v;
    if (r != ERROR_SUCCESS && user == 0 && c && VrInputPad(&v)) {
        memset(c, 0, sizeof(*c));  // describe an ordinary Xbox controller
        c->Type = XINPUT_DEVTYPE_GAMEPAD;
        c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        c->Gamepad.wButtons = 0xF3FF;
        c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 0xFF;
        c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = (SHORT)0xFFC0;
        c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFFFF;
        r = ERROR_SUCCESS;
    }
    return r;
}
template <int I>
DWORD WINAPI H_XInputSetState(DWORD user, XINPUT_VIBRATION* vib) {
    DWORD r = g_pads[I].origSet(user, vib);
    if (user == 0 && vib) {
        VrInputRumble(vib->wLeftMotorSpeed, vib->wRightMotorSpeed);
        XINPUT_GAMEPAD v;
        if (r != ERROR_SUCCESS && VrInputPad(&v)) r = ERROR_SUCCESS;
    }
    return r;
}

// Hook XInput calls to add VR controller input and block input while in mod menu
template <int I>
void HookXInputDLL() {
    PadHook& p = g_pads[I];
    if (p.done) return;
    HMODULE m = GetModuleHandleW(p.dll);
    if (!m) return;
    p.done = true;
    void* fn = (void*)GetProcAddress(m, "XInputGetState");
    void* ex = (void*)GetProcAddress(m, (LPCSTR)100);  // XInputGetStateEx (also reports the Guide button)
    bool ok = fn && HookFunction(fn, (void*)&H_XInputGetState<I>, (void**)&p.orig, "XInputGetState");
    if (ex && ex != fn) HookFunction(ex, (void*)&H_XInputGetStateEx<I>, (void**)&p.origEx, "XInputGetStateEx");
    void* caps = (void*)GetProcAddress(m, "XInputGetCapabilities");
    void* set = (void*)GetProcAddress(m, "XInputSetState");
    bool okCaps = caps && HookFunction(caps, (void*)&H_XInputGetCapabilities<I>, (void**)&p.origCaps, "XInputGetCapabilities");
    bool okSet = set && HookFunction(set, (void*)&H_XInputSetState<I>, (void**)&p.origSet, "XInputSetState");
    if (ok && !g_ReadGamepad) g_ReadGamepad = p.orig;
    Log("menu: gamepad input of %S %s%s", p.dll,
        ok ? "hooked (blocked from the game while the menu is open; VR controllers merged into pad 0)" : "could not be hooked",
        ok && !(okCaps && okSet) ? " - capabilities/rumble not hooked" : "");
}
// Get inputs from a real controller
bool ReadPhysicalPad(XINPUT_GAMEPAD* g) {
    if (!g_ReadGamepad) {
        static bool tried;
        if (!tried) {
            tried = true;
            HMODULE m = LoadLibraryW(L"xinput1_4.dll");
            if (!m) m = LoadLibraryW(L"xinput1_3.dll");
            if (m) g_ReadGamepad = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
        }
        if (!g_ReadGamepad) return false;
    }
    for (DWORD u = 0; u < 4; u++) {
        XINPUT_STATE s = {};
        if (g_ReadGamepad(u, &s) == ERROR_SUCCESS) {
            *g = s.Gamepad;
            return true;
        }
    }
    return false;
}
}

volatile LONG g_blockPad;

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
bool ReadGamepad(XINPUT_GAMEPAD* g) {
    bool pad = ReadPhysicalPad(g);
    if (!pad) memset(g, 0, sizeof(*g));
    XINPUT_GAMEPAD v;
    if (VrInputPad(&v, true)) {
        MergeVr(*g, v);
        pad = true;
    }
    return pad;
}