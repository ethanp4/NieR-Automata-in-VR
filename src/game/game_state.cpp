// game_state.cpp - what the game is doing (cutscene, loading), read from its memory with the NieR:Automata autosplitter's offsets.
#include "game/game_state.h"
#include "core/log.h"
#include "core/safe_memory.h"
#include <stdint.h>
#include <string.h>
#include <windows.h>

namespace {
// Offsets from NieRAutomata.exe, by build, from the NieR:Automata autosplitter:
// https://github.com/rodg/drakenier-splitters (nier_automata_autosplitter.asl)
struct Build {
	const char* name;
	uint32_t imageSize;  // NieRAutomata.exe's size in memory: tells the builds apart
	uintptr_t worldLoaded, loading, cutscenePlaying;
	uintptr_t cutsceneNamePointer, cutsceneNameOffset;  // [exe + pointer] + offset: up to 32 characters
};
constexpr Build kBuilds[] = {
	{"1.0", 113471488, 0x18F3978, 0x18DD444, 0x146A1AC, 0x1978868, 0x1F4},
	{"1.01", 106266624, 0x110ADC0, 0x147BF50, 0x1483974, 0x19925E8, 0x1F4},
	{"1.02", 26177536, 0xF6E240, 0x14005F4, 0x102A244, 0x17805F0, 0x294},
	{"Become as Gods (2021)", 26476544, 0xFE7F20, 0x11435C0, 0xFA54E8, 0x17FD648, 0x294},
};
constexpr int32_t kNameLength = 32;

struct State {
	bool worldLoaded, loading, cutscenePlaying;
	char cutsceneName[kNameLength + 1];
};

bool s_checked;
const Build* s_build;
uintptr_t s_exe;
State s_last;

uint32_t ImageSize(uintptr_t exe) {
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + dos->e_lfanew);
	return nt->OptionalHeader.SizeOfImage;
}

void FindBuild() {
	s_checked = true;
	s_exe = (uintptr_t)GetModuleHandleW(nullptr);
	uint32_t size = ImageSize(s_exe);
	for (const Build& build : kBuilds)
		if (build.imageSize == size) s_build = &build;
	if (s_build) Log("game state: build %s (exe %u bytes in memory): cutscenes and loading are logged", s_build->name, size);
	else Log("game state: unknown build (exe %u bytes in memory): no cutscene or loading state", size);
}

bool ReadFlag(uintptr_t offset) {
	uint8_t value = 0;
	return SafeRead(s_exe + offset, &value, sizeof(value)) && value;
}

// The cutscene's name, printable characters only ("" if it can't be read).
void ReadCutsceneName(char out[kNameLength + 1]) {
	out[0] = 0;
	uintptr_t names = 0;
	if (!SafeRead(s_exe + s_build->cutsceneNamePointer, &names, sizeof(names)) || !names) return;
	if (!SafeRead(names + s_build->cutsceneNameOffset, out, kNameLength)) {
		out[0] = 0;
		return;
	}
	out[kNameLength] = 0;
	for (int32_t i = 0; out[i]; i++)
		if (out[i] < 32 || out[i] > 126) out[i] = 0;
}

void LogChanges(const State& now) {
	if (now.loading != s_last.loading) Log("game state: loading %s", now.loading ? "started" : "finished");
	if (now.worldLoaded != s_last.worldLoaded) Log("game state: world %s", now.worldLoaded ? "loaded" : "unloaded");
	if (now.cutscenePlaying != s_last.cutscenePlaying)
		Log("game state: cutscene %s (%s)", now.cutscenePlaying ? "started" : "ended", now.cutsceneName[0] ? now.cutsceneName : "no name");
	else if (strcmp(now.cutsceneName, s_last.cutsceneName))
		Log("game state: cutscene name now %s%s", now.cutsceneName[0] ? now.cutsceneName : "(none)", now.cutscenePlaying ? " (playing)" : "");
}
}  // namespace

void GameStatePoll() {
	if (!s_checked) FindBuild();
	if (!s_build) return;
	State now = {};
	now.worldLoaded = ReadFlag(s_build->worldLoaded);
	now.loading = ReadFlag(s_build->loading);
	now.cutscenePlaying = ReadFlag(s_build->cutscenePlaying);
	ReadCutsceneName(now.cutsceneName);
	LogChanges(now);
	s_last = now;
}
