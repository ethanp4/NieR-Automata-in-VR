// imgui_panel.cpp - Dear ImGui setup shared by the settings menu and the stats overlay.
#include "ui/imgui_panel.h"
#include "core/log.h"
#include <stdio.h>
#include <d3dcompiler.h>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

// ImGui's DX11 backend calls D3DCompile for its two small shaders. Defined here so the DLL doesn't
// import d3dcompiler_47.dll at load time: it's loaded on first use, like xr_main.cpp does.
HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* name, const D3D_SHADER_MACRO* defines,
						  ID3DInclude* include, const char* entry, const char* target, UINT flags1, UINT flags2,
						  ID3DBlob** code, ID3DBlob** errors) {
	static pD3DCompile real;
	if (!real) {
		HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
		real = m ? (pD3DCompile)(void*)GetProcAddress(m, "D3DCompile") : nullptr;
		if (!real) {
			Log("ui: d3dcompiler_47.dll not available (error %u)", (unsigned)GetLastError());
			return E_FAIL;
		}
	}
	return real(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace {
ImVec4 C(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }
ImVec4 C(const float* c, float a) { return ImVec4(c[0], c[1], c[2], a); }

// Segoe UI from Windows' font folder (on every Windows 10/11); ImGui's own font if it's missing.
void LoadFont(ImGuiIO& io, float size) {
	char path[MAX_PATH];
	UINT n = GetWindowsDirectoryA(path, MAX_PATH - 32);
	if (n && n < MAX_PATH - 32) {
		snprintf(path + n, MAX_PATH - n, "\\Fonts\\segoeui.ttf");
		if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(path, size)) return;
	}
	io.Fonts->AddFontDefault();
}

void Style(float fontSize) {
	using ui::Colors;
	ImGuiStyle& s = ImGui::GetStyle();
	s.FontSizeBase = fontSize;
	s.WindowRounding = 6;
	s.WindowBorderSize = 0;
	s.FrameRounding = 4;
	s.GrabRounding = 4;
	s.GrabMinSize = fontSize * 0.2f;  // a thin handle, so it doesn't cover the value text
	s.FramePadding = ImVec2(fontSize * 0.4f, fontSize * 0.2f);
	s.ItemSpacing = ImVec2(fontSize * 0.4f, fontSize * 0.3f);
	s.CellPadding = ImVec2(fontSize * 0.3f, fontSize * 0.25f);
	s.ScrollbarSize = fontSize * 0.5f;
	ImVec4* c = s.Colors;
	c[ImGuiCol_Text] = C(Colors::text);
	c[ImGuiCol_TextDisabled] = C(Colors::dim);
	c[ImGuiCol_WindowBg] = C(Colors::bg);
	c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_Separator] = C(Colors::bar);
	c[ImGuiCol_FrameBg] = C(Colors::bar);
	c[ImGuiCol_FrameBgHovered] = C(Colors::bar);
	c[ImGuiCol_FrameBgActive] = C(Colors::accent, 0.35f);
	c[ImGuiCol_SliderGrab] = C(Colors::accent, 0.55f);  // see-through: the value text stays readable
	c[ImGuiCol_SliderGrabActive] = C(Colors::text);
	c[ImGuiCol_CheckMark] = C(Colors::accent);
	c[ImGuiCol_CheckboxSelectedBg] = C(Colors::bar);
	c[ImGuiCol_Button] = C(Colors::bar);
	c[ImGuiCol_ButtonHovered] = C(Colors::bar);
	c[ImGuiCol_ButtonActive] = C(Colors::accent, 0.5f);
	c[ImGuiCol_Header] = C(Colors::bar);
	c[ImGuiCol_HeaderHovered] = C(Colors::bar);
	c[ImGuiCol_HeaderActive] = C(Colors::bar);
	c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TabHovered] = C(Colors::bar);
	c[ImGuiCol_TabSelected] = C(Colors::accent, 0.85f);
	c[ImGuiCol_TabSelectedOverline] = C(Colors::accent);
	c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TabDimmedSelected] = C(Colors::accent, 0.85f);
	c[ImGuiCol_PlotLines] = C(Colors::accent);
	c[ImGuiCol_NavCursor] = C(Colors::accent);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = C(Colors::bar);
	c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
}

bool Create(UiPanel& p, ID3D11Device* dev, ID3D11DeviceContext* dc) {
	p.ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(p.ctx);
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;  // no imgui.ini in the game folder
	io.LogFilename = nullptr;
	if (p.navigation) {
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	}
	LoadFont(io, p.fontSize);
	Style(p.fontSize);
	if (!ImGui_ImplDX11_Init(dev, dc)) {
		Log("ui: %s: ImGui DX11 backend could not start", p.name);
		ImGui::DestroyContext(p.ctx);
		p.ctx = nullptr;
		return false;
	}
	QueryPerformanceCounter(&p.last);
	Log("ui: %s: Dear ImGui %s", p.name, IMGUI_VERSION);
	return true;
}
}  // namespace

// Makes the panel's context current (creating it the first time) and sets size and frame time.
// False: ImGui couldn't start (logged once), draw nothing.
bool UiPrepare(UiPanel& p, ID3D11Device* dev, ID3D11DeviceContext* dc, int w, int h) {
	if (p.failed) return false;
	if (!p.ctx && !Create(p, dev, dc)) {
		p.failed = true;
		return false;
	}
	ImGui::SetCurrentContext(p.ctx);
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)w, (float)h);
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	float dt = (float)(now.QuadPart - p.last.QuadPart) / (float)freq.QuadPart;
	io.DeltaTime = dt > 0 && dt < 1 ? dt : 1.0f / 60;
	p.last = now;
	return true;
}

void UiNewFrame() {
	ImGui_ImplDX11_NewFrame();
	ImGui::NewFrame();
}

// Ends the frame and draws it into rtv, cleared to fully see-through first.
void UiRender(ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv) {
	ImGui::Render();
	const float clear[4] = {0, 0, 0, 0};
	dc->ClearRenderTargetView(rtv, clear);
	dc->OMSetRenderTargets(1, &rtv, nullptr);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	dc->OMSetRenderTargets(0, nullptr, nullptr);
}
