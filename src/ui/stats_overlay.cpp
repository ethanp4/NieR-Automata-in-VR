// stats_overlay.cpp - the stats overlay: a small see-through panel in the corner of your view with live
// numbers (frame rates, resolution, head-pose delay) and a headset frame-time graph.
// [Screen] StatsOverlay=1, or the Screen page of the settings menu.
//
// Drawn with Dear ImGui (ui/imgui_panel.h) on the VR thread: xr_frame.cpp hands it a render target
// the size of the panel and shows the result on a quad layer that moves with your head.
#include "ui/stats_overlay.h"
#include <stdio.h>
#include "ui/imgui_panel.h"
#include "imgui.h"

namespace {
UiPanel s_panel = {"stats overlay", 24.0f, false};

void Contents(const OverlayStats& stats) {
	const ImVec4 title(ui::Colors::accent[0], ui::Colors::accent[1], ui::Colors::accent[2], 1);
	const ImVec4 dim(ui::Colors::dim[0], ui::Colors::dim[1], ui::Colors::dim[2], 1);
	ImGui::TextColored(title, "NieRVR " NIERVR_VERSION);
	ImGui::SameLine();
	ImGui::TextColored(dim, "  %s", stats.mode);
	ImGui::Text("Headset %.0f fps    game L %.0f  R %.0f  flat %.0f", stats.headsetFps, stats.gameFps[0], stats.gameFps[1], stats.gameFps[2]);
	if (stats.sceneW && (stats.sceneW != stats.gameW || stats.sceneH != stats.gameH))
		ImGui::Text("Game image %ux%u, upscaled to %ux%u", stats.gameW, stats.gameH, stats.sceneW, stats.sceneH);
	else
		ImGui::Text("Game image %ux%u", stats.gameW, stats.gameH);
	if (stats.upscaler && stats.upscaler[0]) ImGui::TextColored(dim, "%s", stats.upscaler);
	if (stats.sharpX > 0)
		ImGui::Text("Sharpness %.0f%% across, %.0f%% up-down", stats.sharpX, stats.sharpY);
	else
		ImGui::TextColored(dim, "Sharpness: in stereo");
	if (stats.poseDelayMs >= 0)
		ImGui::Text("Pose delay %.0f ms, predicting %.0f ms", stats.poseDelayMs, stats.predictMs);
	else
		ImGui::TextColored(dim, "Pose delay: in stereo");
	if (stats.frameCount > 0) {
		float sum = 0, worst = 0;
		for (int i = 0; i < stats.frameCount; i++) {
			sum += stats.frameMs[i];
			if (stats.frameMs[i] > worst) worst = stats.frameMs[i];
		}
		char label[64];
		snprintf(label, sizeof(label), "headset frame %.1f ms avg, %.1f worst", (double)(sum / stats.frameCount), (double)worst);
		ImGui::PlotLines("##frames", stats.frameMs, stats.frameCount, stats.frameOffset, label, 0.0f, 33.3f,
						 ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 3.0f));
	}
}
}  // namespace

// Draws the overlay into rtv (w x h pixels). False if ImGui couldn't start.
bool OverlayRender(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int width, int height,
				   const OverlayStats& stats) {
	if (!UiPrepare(s_panel, device, context, width, height)) return false;
	UiNewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.075f, 0.07f, 0.70f));  // see-through
	ImGui::Begin("stats", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	Contents(stats);
	ImGui::End();
	ImGui::PopStyleColor();
	UiRender(context, rtv);
	return true;
}
