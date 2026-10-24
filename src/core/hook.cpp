// hook.cpp - inline function hooks through MinHook, shared by every hook the mod installs.
#include "core/hook.h"
#include "core/log.h"
#include "core/main.h"
#include "MinHook.h"

bool HookFunction(void* target, void* detour, void** orig, const char* name) {
	static SRWLOCK lock = SRWLOCK_INIT;
	static bool ready = false;
	AcquireSRWLockExclusive(&lock);
	if (!ready) {
		MH_STATUS st = MH_Initialize();
		ready = st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
		if (!ready) Log("ERROR: MinHook init failed (%d)", (int)st);
	}
	MH_STATUS st = ready ? MH_CreateHook(target, detour, orig) : MH_ERROR_NOT_INITIALIZED;
	if (st == MH_OK) st = MH_EnableHook(target);
	ReleaseSRWLockExclusive(&lock);
	if (st != MH_OK) {
		char where[300];
		DescribeAddress(target, where, sizeof(where));
		Log("ERROR: could not hook %s at %s (MinHook %d)", name, where, (int)st);
		return false;
	}
	return true;
}
