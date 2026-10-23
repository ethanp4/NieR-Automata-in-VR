// imgui_panel.h - Dear ImGui for the flat panels the VR thread shows (settings menu, stats overlay).
//
// Each panel has its own ImGui context, created on first use with the mod's font and colours and
// ImGui's DX11 backend on the VR thread's device. A frame of a panel:
//   if (!UiPrepare(panel, dev, dc, w, h)) return false;   // makes the panel's context current
//   ...feed input with ImGui::GetIO().AddKeyEvent(...) (optional)...
//   UiNewFrame();
//   ...ImGui windows and widgets...
//   UiRender(dc, rtv);                                     // into the panel's render target
// Everything here runs on the VR thread.
#pragma once
#include "core/common.h"

struct ImGuiContext;
struct ImFont;

namespace ui {
// The mod's colours (sRGB, 0..1)
struct Colors {
    static constexpr float bg[4] = {0.118f, 0.114f, 0.102f, 1};       // dark warm grey
    static constexpr float text[4] = {0.855f, 0.831f, 0.733f, 1};     // beige
    static constexpr float dim[4] = {0.588f, 0.569f, 0.502f, 1};
    static constexpr float accent[4] = {0.769f, 0.659f, 0.416f, 1};   // gold
    static constexpr float bar[4] = {0.275f, 0.263f, 0.235f, 1};
    static constexpr float warn[4] = {0.839f, 0.471f, 0.353f, 1};     // "(restart)"
};
}  // namespace ui

struct UiPanel {
    const char* name;    // for the log
    float fontSize;      // pixels
    bool navigation;     // keyboard/gamepad navigation (the menu); the overlay has none
    ImGuiContext* ctx;
    bool failed;
    LARGE_INTEGER last;
};

bool UiPrepare(UiPanel& p, ID3D11Device* dev, ID3D11DeviceContext* dc, int w, int h);
void UiNewFrame();
void UiRender(ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv);
