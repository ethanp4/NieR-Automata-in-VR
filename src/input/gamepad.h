// gamepad.h - the XInput hooks and the menu's gamepad reading (gamepad.cpp).
#pragma once
#include <windows.h>
#include <xinput.h>

extern volatile LONG g_blockPad;  // the menu is open: the game sees a neutral pad

void InstallGamepad();
bool ReadGamepad(XINPUT_GAMEPAD* out);
