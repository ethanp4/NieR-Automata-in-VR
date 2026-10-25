// gamepad.h - the XInput hooks and the menu's gamepad reading (gamepad.cpp).
#pragma once
#include <atomic>
#include <windows.h>
#include <xinput.h>

extern std::atomic<bool> g_blockPad;  // the menu is open: the game sees a neutral pad

void InstallGamepad();
bool ReadGamepad(XINPUT_GAMEPAD* out);
DWORD GamepadAttackTick();  // when an attack button (X light, Y heavy) last went down on the pad the game reads (0: never)
