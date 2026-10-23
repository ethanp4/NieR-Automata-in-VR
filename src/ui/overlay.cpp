// overlay.cpp - the stats overlay: a small see-through panel in the corner of your view with live
// numbers (frame rates, resolution, head-pose delay) and a headset frame-time graph.
// [Screen] StatsOverlay=1, or the Screen page of the settings menu.
//
// Drawn with Dear ImGui (third_party/imgui) on the VR thread's own D3D11 device: xr_main.cpp hands
// it a render target the size of the panel and shows the result on a quad layer that moves with
// your head. Everything here runs on the VR thread.
#include "core/common.h"
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
            Log("overlay: d3dcompiler_47.dll not available (error %u)", (unsigned)GetLastError());
            return E_FAIL;
        }
    }
    return real(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace {
bool g_ready, g_failed;
LARGE_INTEGER g_last, g_freq;

bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini in the game folder
    io.LogFilename = nullptr;
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 24.0f;  // the panel is small in your view: big text
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.075f, 0.07f, 0.70f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.85f, 0.83f, 0.73f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.77f, 0.66f, 0.42f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
    if (!ImGui_ImplDX11_Init(dev, ctx)) {
        Log("overlay: ImGui DX11 backend could not start");
        ImGui::DestroyContext();
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    Log("overlay: Dear ImGui %s", IMGUI_VERSION);
    return true;
}

void Contents(const OverlayStats& s) {
    const ImVec4 title(0.77f, 0.66f, 0.42f, 1.0f), dim(0.59f, 0.57f, 0.50f, 1.0f);
    ImGui::TextColored(title, "NieRVR " NIERVR_VERSION);
    ImGui::SameLine();
    ImGui::TextColored(dim, "  %s", s.mode);
    ImGui::Text("Headset %.0f fps    game L %.0f  R %.0f  flat %.0f", s.headsetFps, s.gameFps[0], s.gameFps[1], s.gameFps[2]);
    ImGui::Text("Game image %ux%u", s.gameW, s.gameH);
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

// Draws the overlay into rtv (w x h pixels, cleared first). False if ImGui couldn't start.
bool OverlayRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h,
                   const OverlayStats& s) {
    if (g_failed) return false;
    if (!g_ready) {
        g_ready = Init(dev, ctx);
        g_failed = !g_ready;
        if (!g_ready) return false;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    float dt = (float)(now.QuadPart - g_last.QuadPart) / (float)g_freq.QuadPart;
    io.DeltaTime = dt > 0 ? dt : 1.0f / 60;
    g_last = now;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("stats", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    Contents(s);
    ImGui::End();
    ImGui::Render();

    const float clear[4] = {0, 0, 0, 0};  // fully see-through outside the window
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}
