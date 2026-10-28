// background_input.cpp - [Input] BackgroundInput: the game's own background play setting held on, so controllers work while its window isn't focused.
#include "game/background_input.h"
#include "core/config.h"
#include "core/log.h"
#include "core/safe_memory.h"
#include <stdint.h>
#include <windows.h>

namespace {
// The game's background play setting (1: on; written from its settings when a save is loaded, so it's off
// at the title screen). With it on, its pad input isn't cleared while the window is inactive; its keyboard
// and mouse input still is (they check the window's own inactive flag).
struct Build {
	uint32_t imageSize;  // NieRAutomata.exe's size in memory
	uintptr_t setting;
};
constexpr Build kBuilds[] = {{26177536, 0x149571C}};  // the 2021 Steam build (the autosplitter's "1.02")

bool s_checked, s_holding, s_loggedOn;
uintptr_t s_setting;
uint32_t s_gameValue;  // the setting before the mod held it on

uintptr_t FindSetting() {
	uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
	auto* nt = (const IMAGE_NT_HEADERS*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
	for (const Build& build : kBuilds)
		if (build.imageSize == nt->OptionalHeader.SizeOfImage) return exe + build.setting;
	Log("background input: unknown game build (%u bytes in memory): BackgroundInput does nothing", (unsigned)nt->OptionalHeader.SizeOfImage);
	return 0;
}
}  // namespace

void BackgroundInputPoll() {
	if (!s_checked) {
		s_checked = true;
		s_setting = FindSetting();
	}
	if (!s_setting) return;
	uint32_t value = 0;
	if (!SafeRead(s_setting, &value, sizeof(value))) return;
	if (g_cfg.backgroundInput) {
		if (!s_holding) {
			s_holding = true;
			s_gameValue = value;
		}
		if (value != 1) {
			const uint32_t on = 1;
			SafeWrite(s_setting, &on, sizeof(on));
		}
		if (!s_loggedOn) {
			s_loggedOn = true;
			Log("background input: on (the game's background play setting held on: controllers work while the window isn't focused)");
		}
	} else if (s_holding) {
		s_holding = false;
		SafeWrite(s_setting, &s_gameValue, sizeof(s_gameValue));
	}
}
