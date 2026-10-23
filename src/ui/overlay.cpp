// overlay.cpp - the stats overlay: a small see-through panel in the corner of your view with live
// numbers (frame rates, resolution, head-pose delay) and a headset frame-time graph.
// [Screen] StatsOverlay=1, or the Screen page of the settings menu.
//
// Drawn with Dear ImGui (ui/imgui_panel.h) on the VR thread: xr_main.cpp hands it a render target
// the size of the panel and shows the result on a quad layer that moves with your head.
#include "core/common.h"
#include "ui/imgui_panel.h"
#include "imgui.h"

namespace {
UiPanel g_panel = {"stats overlay", 24.0f, false};

void Contents(const OverlayStats& s) {
    const ImVec4 title(ui::Colors::accent[0], ui::Colors::accent[1], ui::Colors::accent[2], 1);
    const ImVec4 dim(ui::Colors::dim[0], ui::Colors::dim[1], ui::Colors::dim[2], 1);
    ImGui::TextColored(title, "NieRVR " NIERVR_VERSION);
    ImGui::SameLine();
    ImGui::TextColored(dim, "  %s", s.mode);
    ImGui::Text("Headset %.0f fps    game L %.0f  R %.0f  flat %.0f", s.headsetFps, s.gameFps[0], s.gameFps[1], s.gameFps[2]);
    if (s.sceneW && (s.sceneW != s.gameW || s.sceneH != s.gameH))
        ImGui::Text("Game image %ux%u, upscaled to %ux%u", s.gameW, s.gameH, s.sceneW, s.sceneH);
    else
        ImGui::Text("Game image %ux%u", s.gameW, s.gameH);
    if (s.upscaler && s.upscaler[0]) ImGui::TextColored(dim, "%s", s.upscaler);
    if (s.sharpX > 0)
        ImGui::Text("Sharpness %.0f%% across, %.0f%% up-down", s.sharpX, s.sharpY);
    else
        ImGui::TextColored(dim, "Sharpness: in stereo");
    if (s.poseDelayMs >= 0)
        ImGui::Text("Pose delay %.0f ms, predicting %.0f ms", s.poseDelayMs, s.predictMs);
    else
        ImGui::TextColored(dim, "Pose delay: in stereo");
    if (s.frameCount > 0) {
        float sum = 0, worst = 0;
        for (int i = 0; i < s.frameCount; i++) {
            sum += s.frameMs[i];
            if (s.frameMs[i] > worst) worst = s.frameMs[i];
        }
        char label[64];
        snprintf(label, sizeof(label), "headset frame %.1f ms avg, %.1f worst", (double)(sum / s.frameCount), (double)worst);
        ImGui::PlotLines("##frames", s.frameMs, s.frameCount, s.frameOffset, label, 0.0f, 33.3f,
                         ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 3.0f));
    }
}
}  // namespace

// Draws the overlay into rtv (w x h pixels). False if ImGui couldn't start.
bool OverlayRender(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv, int w, int h,
                   const OverlayStats& s) {
    if (!UiPrepare(g_panel, dev, dc, w, h)) return false;
    UiNewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.075f, 0.07f, 0.70f));  // see-through
    ImGui::Begin("stats", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    Contents(s);
    ImGui::End();
    ImGui::PopStyleColor();
    UiRender(dc, rtv);
    return true;
}
