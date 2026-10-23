#pragma once
#include <xinput.h>

extern volatile LONG g_blockPad;

void InstallGamepad();
bool ReadGamepad(XINPUT_GAMEPAD* g);