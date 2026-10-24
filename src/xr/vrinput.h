// vrinput.h - VR controllers as a gamepad, and motion controls (vrinput.cpp).
#pragma once
#include <xinput.h>
#include "xr/xr.h"

// Game side (input/gamepad.cpp)
bool VrInputPad(XINPUT_GAMEPAD* out, bool evenIfOff = false);  // false = no VR gamepad; evenIfOff: for the menu
void VrInputRumble(unsigned short left, unsigned short right);

// VR thread
void VrInputInit(PFN_xrGetInstanceProcAddr gipa, XrInstance inst, bool pico);
void VrInputAttach(XrSession s);
void VrInputSync(bool focused, bool foreground);
void VrInputMotion(XrSpace local, XrSpace view, XrTime time, bool focused, bool flipped);  // motion controls, after VrInputSync
void VrInputProfileChanged();  // interaction profile changed event
void VrInputDetach();
void VrInputShutdown();
