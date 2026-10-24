// hook.h - inline function hooks (MinHook).
#pragma once

bool HookFunction(void* target, void* detour, void** orig, const char* name);  // false: logged why
