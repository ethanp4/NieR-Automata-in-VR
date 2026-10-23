// dlss.cpp - NVIDIA DLSS (Super Resolution) on the game's D3D11 device.
//
// NVIDIA's SDK libraries are built for MSVC, so the mod doesn't link them: the driver's own NGX
// loader (_nvngx.dll, found through the registry) exports the plain C functions the SDK library would
// call, and those are used directly. They load nvngx_dlss.dll (the DLSS model) from the game's folder.
//
// Settings go into an NVSDK_NGX_Parameter, a C++ interface implemented by the driver. Its methods are
// called by their slot in the vtable; MSVC stores overloaded virtual functions in reverse order, so
// the slots are (tested on an RTX 2070, driver 2026-07):
//   0-7 Set(void*, ID3D12Resource*, ID3D11Resource*, int, unsigned, double, float, unsigned long long)
//   8-15 Get(the same, as pointers), 16 Reset.
// Parameter names and values are NVIDIA's (nvsdk_ngx_defs.h in the DLSS SDK).
#include "render/dlss.h"
#include <stdarg.h>

namespace {
typedef int Res;  // NVSDK_NGX_Result
bool Ok(Res r) { return (r & 0xFFF00000) != 0xBAD00000; }
struct Param;
struct Handle;

typedef Res (*PFN_Init_Ext)(unsigned long long appId, const wchar_t* dataPath, ID3D11Device*, int sdkVersion,
                            const Param*);
typedef Res (*PFN_Params)(Param**);
typedef Res (*PFN_DestroyParams)(Param*);
typedef Res (*PFN_Create)(ID3D11DeviceContext*, int feature, Param*, Handle**);
typedef Res (*PFN_Eval)(ID3D11DeviceContext*, const Handle*, const Param*, void* progress);
typedef Res (*PFN_Release)(Handle*);
typedef Res (*PFN_Optimal)(Param*);

const int kSdkVersion = 0x15;           // NVSDK_NGX_Version_API (1.5)
const int kFeatureSuperSampling = 1;
const unsigned long long kAppId = 0x4E69657252ull;  // any id: the driver accepts custom ones

PFN_Init_Ext g_init;
PFN_Params g_getCaps, g_alloc;
PFN_DestroyParams g_destroy;
PFN_Create g_create;
PFN_Eval g_eval;
PFN_Release g_release;

// vtable slots (see the top of the file)
enum { S_PTR = 0, S_R11 = 2, S_I = 3, S_UI = 4, S_F = 6, GET = 8 };
template <class F> F V(Param* p, int slot) { return (F)(*(void***)p)[slot]; }
void SetUI(Param* p, const char* n, unsigned v) { V<void (*)(Param*, const char*, unsigned)>(p, S_UI)(p, n, v); }
void SetI(Param* p, const char* n, int v) { V<void (*)(Param*, const char*, int)>(p, S_I)(p, n, v); }
void SetF(Param* p, const char* n, float v) { V<void (*)(Param*, const char*, float)>(p, S_F)(p, n, v); }
void SetR(Param* p, const char* n, ID3D11Resource* v) { V<void (*)(Param*, const char*, ID3D11Resource*)>(p, S_R11)(p, n, v); }
Res GetI(Param* p, const char* n, int* v) { return V<Res (*)(Param*, const char*, int*)>(p, GET + S_I)(p, n, v); }

bool g_tried, g_ready;
char g_status[128] = "DLSS: not started";

struct Feature {
    Handle* h;
    Param* p;
    UINT rw, rh, ow, oh;
    int quality, model;
    bool inverted;
} g_feat[2];

void SetStatus(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void SetStatus(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, a);
    va_end(a);
    Log("%s", g_status);
}

void Release(Feature& f) {
    if (f.h) g_release(f.h);
    if (f.p) g_destroy(f.p);
    f = {};
}

// Our quality index -> NVSDK_NGX_PerfQuality_Value
int PerfQuality(int q) {
    static const int v[] = {5 /*DLAA*/, 2 /*MaxQuality*/, 1 /*Balanced*/, 0 /*MaxPerf*/, 3 /*UltraPerformance*/};
    return v[q < 0 ? 0 : q > 4 ? 4 : q];
}

// Is nvngx_dlss.dll in the game's folder (where NGX looks for it)?
bool DllPresent() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    wcscpy(slash + 1, L"nvngx_dlss.dll");
    return GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
}

// File version of a loaded DLL from its version resource ("310.9.16"), without version.dll.
void ModuleVersion(HMODULE m, char* out, size_t cap) {
    snprintf(out, cap, "?");
    HRSRC r = m ? FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /*RT_VERSION*/) : nullptr;
    HGLOBAL g = r ? LoadResource(m, r) : nullptr;
    const uint8_t* p = g ? (const uint8_t*)LockResource(g) : nullptr;
    DWORD n = r ? SizeofResource(m, r) : 0;
    for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= n; i += 4) {
        const VS_FIXEDFILEINFO* fi = (const VS_FIXEDFILEINFO*)(p + i);
        if (fi->dwSignature != 0xFEEF04BD) continue;
        snprintf(out, cap, "%u.%u.%u", (unsigned)HIWORD(fi->dwFileVersionMS), (unsigned)LOWORD(fi->dwFileVersionMS),
                 (unsigned)HIWORD(fi->dwFileVersionLS));
        return;
    }
}
}  // namespace

bool DlssInit(ID3D11Device* dev) {
    if (g_tried) return g_ready;
    g_tried = true;
    if (!DllPresent()) {
        SetStatus("DLSS: nvngx_dlss.dll is not in the game's folder");
        return false;
    }
    wchar_t dir[MAX_PATH] = {};
    DWORD size = sizeof(dir);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath",
                     RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) {
        SetStatus("DLSS: no NVIDIA driver with DLSS support found");
        return false;
    }
    wchar_t path[MAX_PATH];
    snwprintf(path, MAX_PATH, L"%ls\\_nvngx.dll", dir);
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        SetStatus("DLSS: could not load the driver's _nvngx.dll (error %u)", (unsigned)GetLastError());
        return false;
    }
#define LOAD(var, name) var = (decltype(var))(void*)GetProcAddress(m, "NVSDK_NGX_D3D11_" name)
    LOAD(g_init, "Init_Ext");
    LOAD(g_getCaps, "GetCapabilityParameters");
    LOAD(g_alloc, "AllocateParameters");
    LOAD(g_destroy, "DestroyParameters");
    LOAD(g_create, "CreateFeature");
    LOAD(g_eval, "EvaluateFeature");
    LOAD(g_release, "ReleaseFeature");
#undef LOAD
    if (!g_init || !g_getCaps || !g_alloc || !g_destroy || !g_create || !g_eval || !g_release) {
        SetStatus("DLSS: the driver's NGX loader lacks D3D11 functions (old driver?)");
        return false;
    }
    Res r = g_init(kAppId, g_modDir, dev, kSdkVersion, nullptr);
    if (!Ok(r)) {
        SetStatus("DLSS: NGX did not start (0x%08x)", (unsigned)r);
        return false;
    }
    Param* caps = nullptr;
    int avail = 0, needDriver = 0;
    if (Ok(g_getCaps(&caps)) && caps) {
        GetI(caps, "SuperSampling.Available", &avail);
        GetI(caps, "SuperSampling.NeedsUpdatedDriver", &needDriver);
    }
    if (avail != 1) {
        SetStatus("%s", needDriver ? "DLSS: needs a newer NVIDIA driver" : "DLSS: not supported by this graphics card");
        return false;
    }
    g_ready = true;
    char ver[32];
    ModuleVersion(GetModuleHandleW(L"nvngx_dlss.dll"), ver, sizeof(ver));  // loaded by the capability check
    snprintf(g_status, sizeof(g_status), "DLSS %s", ver);
    Log("DLSS: ready (nvngx_dlss.dll %s; NGX loader %s)", ver, Utf8(path).s);
    return true;
}

bool DlssPrepare(ID3D11DeviceContext* c, int eye, UINT rw, UINT rh, UINT ow, UINT oh, int quality, int model,
                 bool inverted) {
    if (!g_ready || eye < 0 || eye > 1) return false;
    Feature& f = g_feat[eye];
    if (f.h && f.rw == rw && f.rh == rh && f.ow == ow && f.oh == oh && f.quality == quality && f.model == model &&
        f.inverted == inverted)
        return true;
    Release(f);
    if (!Ok(g_alloc(&f.p)) || !f.p) return false;
    const unsigned preset = model ? 11 /*K, transformer*/ : 5 /*E, CNN*/;
    static const char* const hints[] = {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
                                        "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
                                        "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
    for (const char* h : hints) SetUI(f.p, h, preset);
    SetUI(f.p, "CreationNodeMask", 1);
    SetUI(f.p, "VisibilityNodeMask", 1);
    SetUI(f.p, "Width", rw);
    SetUI(f.p, "Height", rh);
    SetUI(f.p, "OutWidth", ow);
    SetUI(f.p, "OutHeight", oh);
    SetI(f.p, "PerfQualityValue", PerfQuality(quality));
    SetI(f.p, "RTXValue", 0);
    // motion vectors at render size; the game's depth convention
    SetI(f.p, "DLSS.Feature.Create.Flags", (1 << 1) /*MVLowRes*/ | (inverted ? 1 << 3 /*DepthInverted*/ : 0));
    SetI(f.p, "DLSS.Enable.Output.Subrects", 0);
    Res r = g_create(c, kFeatureSuperSampling, f.p, &f.h);
    if (!Ok(r) || !f.h) {
        Log("DLSS: could not create the %s eye's upscaler %ux%u -> %ux%u (0x%08x)", eye ? "right" : "left", rw, rh, ow,
            oh, (unsigned)r);
        Release(f);
        return false;
    }
    f.rw = rw, f.rh = rh, f.ow = ow, f.oh = oh, f.quality = quality, f.model = model, f.inverted = inverted;
    Log("DLSS: %s eye %ux%u -> %ux%u, quality %d, preset %s", eye ? "right" : "left", rw, rh, ow, oh, quality,
        model ? "K (transformer)" : "E (CNN)");
    return true;
}

bool DlssEvaluate(ID3D11DeviceContext* c, int eye, const DlssInputs& in) {
    if (!g_ready || eye < 0 || eye > 1 || !g_feat[eye].h) return false;
    Feature& f = g_feat[eye];
    Param* p = f.p;
    SetR(p, "Color", in.color);
    SetR(p, "Output", in.output);
    SetR(p, "Depth", in.depth);
    SetR(p, "MotionVectors", in.motion);
    SetF(p, "Jitter.Offset.X", in.jitterX);
    SetF(p, "Jitter.Offset.Y", in.jitterY);
    SetF(p, "Sharpness", 0);
    SetI(p, "Reset", in.reset ? 1 : 0);
    SetF(p, "MV.Scale.X", 1);
    SetF(p, "MV.Scale.Y", 1);
    SetUI(p, "DLSS.Render.Subrect.Dimensions.Width", f.rw);
    SetUI(p, "DLSS.Render.Subrect.Dimensions.Height", f.rh);
    SetF(p, "DLSS.Pre.Exposure", 1);
    SetF(p, "DLSS.Exposure.Scale", 1);
    Res r = g_eval(c, f.h, p, nullptr);
    if (!Ok(r)) {
        static volatile LONG budget;
        if (LogBudget(&budget, 5)) Log("DLSS: evaluate failed for the %s eye (0x%08x)", eye ? "right" : "left", (unsigned)r);
        return false;
    }
    return true;
}

void DlssReleaseFeatures() {
    if (!g_ready) return;
    for (auto& f : g_feat) Release(f);
}

const char* DlssStatus() { return g_status; }
