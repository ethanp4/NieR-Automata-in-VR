// vr_controllers.h - VR controllers as a gamepad, and motion controls (vr_controllers.cpp).
#pragma once
#include <xinput.h>
#include "xr/openxr.h"

// Game side (input/gamepad.cpp)
bool VrInputPad(XINPUT_GAMEPAD* out, bool evenIfOff = false);  // false = no VR gamepad; evenIfOff: for the menu
void VrInputRumble(unsigned short left, unsigned short right);

// VR thread
void VrInputInit(PFN_xrGetInstanceProcAddr getProcAddr, XrInstance instance, bool pico);
void VrInputAttach(XrSession session);
void VrInputSync(bool focused, bool foreground);
void VrInputMotion(XrSpace local, XrSpace view, XrTime time, bool focused, bool flipped);  // motion controls, after VrInputSync
void VrInputProfileChanged();                                                              // interaction profile changed event
void VrInputDetach();
void VrInputShutdown();
