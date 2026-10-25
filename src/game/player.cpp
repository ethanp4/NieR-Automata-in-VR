// player.cpp - the game's 2B: where it keeps her position, moved by your real steps (first person).
//
// NieRAutomata.exe+0x1020948 points at her object (found with the position finder, debug/position_finder.cpp;
// the 2021 Steam build), and her position is at +0x50 (x y z). Writing it moves her: the game's copies
// of her position are rebuilt from it every frame. Before writing, the object's position has to be
// where her bones are (first_person.cpp), so a different object (another character, a cutscene, another
// version of the game) is never written. Memory goes through Read/WriteProcessMemory: an object freed
// meanwhile is skipped, not a crash.
#include "game/player.h"
#include "core/log.h"
#include "render/first_person.h"
#include <math.h>
#include <stdint.h>
#include <windows.h>

namespace {
constexpr uintptr_t kPlayerPointer = 0x1020948;  // in the exe's own memory
constexpr uintptr_t kPositionOffset = 0x50;
constexpr float kSameSpot = 1.5f;  // her object's position this close to her bones (metres, each axis)

bool s_loggedFound, s_loggedMismatch;

bool ReadOwn(uintptr_t at, void* to, size_t size) {
	SIZE_T got = 0;
	return ReadProcessMemory(GetCurrentProcess(), (const void*)at, to, size, &got) && got == size;
}

bool WriteOwn(uintptr_t at, const void* from, size_t size) {
	SIZE_T wrote = 0;
	return WriteProcessMemory(GetCurrentProcess(), (void*)at, from, size, &wrote) && wrote == size;
}

// Where her position is, if her object is there and where her bones are (0: not).
uintptr_t HerPosition(pure::V3* position) {
	pure::V3 feet;
	if (!FirstPersonFeet(&feet)) return 0;
	uintptr_t object = 0;
	if (!ReadOwn((uintptr_t)GetModuleHandleW(nullptr) + kPlayerPointer, &object, sizeof(object)) || !object) return 0;
	float at[3];
	if (!ReadOwn(object + kPositionOffset, at, sizeof(at))) return 0;
	if (!(fabsf(at[0] - feet.x) < kSameSpot && fabsf(at[1] - feet.y) < kSameSpot && fabsf(at[2] - feet.z) < kSameSpot)) {
		if (!s_loggedMismatch) {
			s_loggedMismatch = true;
			Log("player: the object at NieRAutomata.exe+0x%llx is at %.2f %.2f %.2f, her bones at %.2f %.2f %.2f: not her (left alone)",
				(unsigned long long)kPlayerPointer, (double)at[0], (double)at[1], (double)at[2], (double)feet.x, (double)feet.y, (double)feet.z);
		}
		return 0;
	}
	if (!s_loggedFound) {
		s_loggedFound = true;
		Log("player: found 2B's object (NieRAutomata.exe+0x%llx -> %llx), her position at +0x%llx", (unsigned long long)kPlayerPointer,
			(unsigned long long)object, (unsigned long long)kPositionOffset);
	}
	*position = {at[0], at[1], at[2]};
	return object + kPositionOffset;
}
}  // namespace

bool PlayerMove(pure::V3 step) {
	pure::V3 position;
	uintptr_t at = HerPosition(&position);
	if (!at) return false;
	float x = position.x + step.x, z = position.z + step.z;  // x and z only: the game keeps her height (the ground)
	return WriteOwn(at, &x, sizeof(x)) && WriteOwn(at + 8, &z, sizeof(z));
}
