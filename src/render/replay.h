// replay.h - BothEyes: record the left eye's frame, play it back for the right eye at Present (replay.cpp).
#pragma once
#include <d3d11.h>

extern volatile bool g_replaying;  // playback running: hooks pass calls straight through
extern volatile LONG g_recOn;
extern ID3D11DeviceContext* g_recCtx;
extern volatile LONG g_recSuspend;  // mod-internal calls (not recorded)
inline bool RecOn(ID3D11DeviceContext* c) { return g_recOn && c == g_recCtx && !g_recSuspend && !g_replaying; }

enum RecStage { RS_VS = 0, RS_HS, RS_DS, RS_GS, RS_PS, RS_CS };
void RecSetCB(int stage, UINT start, UINT n, ID3D11Buffer* const* b);
void RecSetSRV(int stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v);
void RecSetShader(int stage, void* sh, ID3D11ClassInstance* const* ci, UINT n);
void RecCSSetUAV(UINT start, UINT n, ID3D11UnorderedAccessView* const* v, const UINT* counts);
void RecDraw(int kind, UINT a, UINT b, INT c, UINT d, UINT e);
void RecDispatch(UINT x, UINT y, UINT z, int viewFixKind);
void RecOMSetRT(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void RecViewports(UINT n, const D3D11_VIEWPORT* vp);
void RecBeforeMap(ID3D11Resource* res, D3D11_MAP type);
void RecMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, const D3D11_MAPPED_SUBRESOURCE* m);
void RecUnmap(ID3D11Resource* res, UINT sub);
void RecUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp);
void RecUnsupported(const char* what);

void ReplayInstall(ID3D11DeviceContext* ctx);
bool ReplayAvailable();
void ReplayFrameStart(ID3D11DeviceContext* c, bool bothEyes);
void ReplayRelease();  // before the swapchain is resized: hold nothing of the game's
void ReplayStop(bool fromUi);
void ReplayCaptureFinalPassState(ID3D11DeviceContext* c);
void ReplayApplyFinalPassState(ID3D11DeviceContext* c);
bool ReplaySecondEye(ID3D11DeviceContext* c, bool* inBackbuffer);
void ReplayReport();

// The game's pipeline state around the mod's own drawing on its context
void StateSaveTemp(ID3D11DeviceContext* c);
void StateRestoreTemp(ID3D11DeviceContext* c);
