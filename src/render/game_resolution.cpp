// game_resolution.cpp - the render resolution chosen in the VR panel, applied through the game's own settings.
//
// NieR builds every one of its render targets from the resolution it saved, not from the swapchain it
// is handed: giving it a smaller swapchain cropped the picture and crashed it (2026-10-27). So the panel's
// resolution goes where the game's own menu puts it - SystemData.dat, two 32-bit numbers at bytes 8 and 12
// (the file starts with the Steam account id) - and the game picks it up the next time it starts. The game
// only rewrites that file when its own settings menu changes something, so a value written here stays.
#include "render/game_resolution.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "render/extra_resolutions.h"
#include "ui/menu.h"
#include <windows.h>
#include <stdio.h>

namespace {
constexpr DWORD kFileSize = 72;
constexpr DWORD kWidthOffset = 8, kHeightOffset = 12;

wchar_t s_path[MAX_PATH];
bool s_havePath;
uint32_t s_headsetW, s_headsetH;  // the headset's full detail as it was at start-up (it can move while running)
uint32_t s_writtenW, s_writtenH;  // what the file holds now, as far as we know

// Documents (wherever the player has moved it) \My Games\NieR_Automata\SystemData.dat
bool SettingsPath() {
	if (s_havePath) return true;
	wchar_t raw[MAX_PATH] = {};
	DWORD size = sizeof(raw);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", L"Personal",
					 RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &size) != ERROR_SUCCESS)
		return false;
	wchar_t documents[MAX_PATH];
	if (!ExpandEnvironmentStringsW(raw, documents, MAX_PATH)) return false;
	if (_snwprintf(s_path, MAX_PATH, L"%ls\\My Games\\NieR_Automata\\SystemData.dat", documents) < 0) return false;
	s_havePath = true;
	return true;
}

bool ReadFileBytes(uint8_t* data) {
	HANDLE file = CreateFileW(s_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD read = 0;
	bool ok = GetFileSize(file, nullptr) == kFileSize && ReadFile(file, data, kFileSize, &read, nullptr) && read == kFileSize;
	CloseHandle(file);
	return ok;
}

uint32_t At(const uint8_t* data, DWORD offset) { return (uint32_t)data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | (uint32_t)data[offset + 3] << 24; }
void Put(uint8_t* data, DWORD offset, uint32_t value) {
	for (int i = 0; i < 4; i++) data[offset + i] = (uint8_t)(value >> (8 * i));
}
bool Plausible(uint32_t w, uint32_t h) { return w >= 320 && h >= 240 && w <= 16384 && h <= 16384; }

// The size the panel asks for: the headset's full detail x Resolution, divided by the enlargement the
// DLSS quality stands for (no enlargement with the upscaler off).
bool PanelSize(uint32_t* w, uint32_t* h) {
	if (!g_cfg.panelResolution || s_headsetW < 640 || s_headsetH < 480) return false;
	float share = g_cfg.upscaleOutput > 10 ? g_cfg.upscaleOutput / 100.f : 1.f;
	int mode = g_cfg.upscaler ? g_cfg.upscaleQuality : 0;
	pure::upscale_input_for((uint32_t)(s_headsetW * share), (uint32_t)(s_headsetH * share), mode, w, h);
	return Plausible(*w, *h);
}

void ShowState(uint32_t panelW, uint32_t panelH) {
	char text[160];
	if ((uint32_t)g_cfg.gameWidth == panelW && (uint32_t)g_cfg.gameHeight == panelH)
		snprintf(text, sizeof(text), "Resolution: the game is drawing %ux%u, as the settings below ask.", panelW, panelH);
	else
		snprintf(text, sizeof(text), "Resolution: restart the game to apply - it will draw %ux%u (now %dx%d).", panelW, panelH, g_cfg.gameWidth,
				 g_cfg.gameHeight);
	MenuSetInfo(INFO_PANEL_RESOLUTION, text);
}

// Writes the size into the game's settings (a copy of the original is kept the first time).
bool Apply(uint32_t w, uint32_t h, const char* why) {
	uint8_t data[kFileSize];
	if (!ReadFileBytes(data)) {
		Log("resolution: cannot read the game's settings (%ls); its own resolution stays", s_path);
		return false;
	}
	uint32_t savedW = At(data, kWidthOffset), savedH = At(data, kHeightOffset);
	if (!Plausible(savedW, savedH)) {
		Log("resolution: the game's settings do not look as expected (%ux%u at bytes 8/12); left alone", savedW, savedH);
		return false;
	}
	s_writtenW = savedW, s_writtenH = savedH;
	if (savedW == w && savedH == h) return true;
	wchar_t backup[MAX_PATH];
	_snwprintf(backup, MAX_PATH, L"%ls.niervr-original", s_path);
	CopyFileW(s_path, backup, TRUE);  // TRUE: only the first time - it keeps the player's own setting
	Put(data, kWidthOffset, w);
	Put(data, kHeightOffset, h);
	HANDLE file = CreateFileW(s_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		Log("resolution: cannot write the game's settings (error %u)", (unsigned)GetLastError());
		return false;
	}
	DWORD written = 0;
	bool ok = WriteFile(file, data, kFileSize, &written, nullptr) && written == kFileSize;
	CloseHandle(file);
	if (!ok) return false;
	s_writtenW = w, s_writtenH = h;
	Log("resolution: the game's saved resolution is now %ux%u, was %ux%u (%s)", w, h, savedW, savedH, why);
	return true;
}
}  // namespace

bool GameResolutionPanelSize(uint32_t* width, uint32_t* height) { return PanelSize(width, height); }

bool GameResolutionSizes(uint32_t* drawW, uint32_t* drawH, uint32_t* headsetW, uint32_t* headsetH) {
	if (s_headsetW < 640 || s_headsetH < 480) return false;
	float share = g_cfg.upscaleOutput > 10 ? g_cfg.upscaleOutput / 100.f : 1.f;
	*headsetW = ((uint32_t)(s_headsetW * share) + 4) & ~7u;
	*headsetH = ((uint32_t)(s_headsetH * share) + 4) & ~7u;
	pure::upscale_input_for(*headsetW, *headsetH, g_cfg.upscaler ? g_cfg.upscaleQuality : 0, drawW, drawH);
	return true;
}

void GameResolutionInit() {
	s_headsetW = (uint32_t)g_cfg.headsetWidth;
	s_headsetH = (uint32_t)g_cfg.headsetHeight;
	uint32_t w, h;
	if (!PanelSize(&w, &h) || !SettingsPath()) return;
	Apply(w, h, "the panel's Resolution and DLSS quality");
}

void GameResolutionTick() {
	uint32_t w, h;
	if (!PanelSize(&w, &h)) return;
	ShowState(w, h);
	if (w == s_writtenW && h == s_writtenH) return;
	if (SettingsPath()) Apply(w, h, "changed in the panel; the game picks it up when it next starts");
}
