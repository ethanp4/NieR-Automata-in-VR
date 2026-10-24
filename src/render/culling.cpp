// culling.cpp - widens the field of view NieR culls (and lays out) its scene with.
//
// The game decides what to draw from its own camera: about 50 degrees tall, 16:9. In the headset
// you see ~95 x 109 degrees and can look around, so things outside the game's view were missing
// (NPC hair, screen contents, lights at the edges). While stereo is rendering, the game camera's
// field-of-view setter is hooked to hand the game a wider value (CullingFov), so its own camera
// update builds a wider culling frustum. The renderer's projection is replaced per eye anyway
// (camera.cpp), so the VR image keeps its shape.
#include "render/culling.h"
#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/main.h"
#include "core/pure.h"
#include "render/camera.h"
#include "ui/menu.h"
#include <stdio.h>
#include <string.h>

namespace {
bool s_loggedEffect;  // the game camera was seen using the wide fov
}  // namespace

// ---------------------------------------------------------------------------------------------
// The fix (0.3.11): a hardware-breakpoint watch showed the camera controller calls a tiny setter every frame,
//   NieRAutomata.exe+0x27ace0:  mov rax,[rcx]; movss [rax+0x274],xmm1; mov dword [rax+0x284],1; ret
// (fov at +0x274, "dirty" flag at +0x284), and the camera update (exe+0x27aaa2) rebuilds the
// projection and the culling frustum from that fov when the flag is set. So our writes at Present
// were overwritten before the next frame was culled. Hooking the setter widens the value the game
// itself uses, right where it is set. Only the main 16:9 cameras are widened (render-to-texture
// cameras are square), and only while stereo is rendering.
// ---------------------------------------------------------------------------------------------
typedef void (*PFN_SetFov)(void** cam, float fov);
PFN_SetFov o_SetFov;
volatile LONG s_wideOn;
volatile LONG s_widened, s_passed;
bool s_hookOk;

volatile float s_gameFov;  // the last field of view the game asked for (main camera, radians)
// culling_stubs.S: the main camera object (0 = off) and the widened fov (radians) for the sites below.
extern "C" {
extern void* volatile nvr_fov_obj;
extern void* volatile nvr_fov_site_obj[4];  // per read site
extern volatile LONG nvr_head_cull;          // culling_stubs.S calls nvr_head_cull_builder when set
extern volatile float nvr_fov_wide;
extern volatile float nvr_fov_wide_b;  // the frustum builder's (site B): headset-sized with head culling
extern void* nvr_fov_resume[4];
extern void* nvr_fov_orig[4];
extern volatile LONG64 nvr_fov_hits[4];
void nvr_fov_stub0();
void nvr_fov_stub1();
void nvr_fov_stub2();
void nvr_fov_stub3();
void nvr_post_builder();
extern void* nvr_post_orig;
}
int s_sitesOk;     // fov read sites redirected (0 = old way: widen the value SetFov stores)
bool s_siteA;      // the projection rebuild site is among them (required for the new way)

// ---------------------------------------------------------------------------------------------
// Head-direction culling (0.3.22). A symmetric frustum can't reach past 90 deg to a side, and at
// CullingFov=170 it reaches ~87: turn your head further than ~35-40 deg from where the game camera
// points and the edge of your view leaves it (0.3.21 user report). The frustum builder after site B
// gets LookAt(eye, target, up) as three pointers (0.3.20/0.3.21 logs: r14 = eye, rbp = target,
// rsi = up, values with leftover w lanes, i.e. a temporary on the stack). culling_stubs.S calls this
// right before that call: the target and up are turned by the eye's rotation relative to the game
// camera (StereoHeadRelative), so the culling view looks where you look. Only done when the three
// vectors are on this thread's stack: then they're a copy made for this call and the camera itself
// isn't changed. If they're not, head culling switches itself off and says so in the log.
// ---------------------------------------------------------------------------------------------
volatile LONG s_hcDone, s_hcSkipped;
bool s_headCullRefused, s_hcLogged;
// 0.3.22 log: the look-at is a fixed block in the exe's data (not on the stack). The builder call is
// the camera update's last act (code after site B: two movss to the stack, call, epilogue), so
// 0.3.23 hooks the instruction after the call and puts the original target/up back there.
bool s_hcRestoreOk;      // the after-call hook is in place
float* volatile s_hcBlock;  // block changed and not yet restored (0 = none)
DWORD s_hcTid;
float s_hcSaved[9];  // eye, target, up
volatile LONG s_hcRestored;

static bool OnThisStack(const void* p, size_t n) {
	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	const uint8_t* a = (const uint8_t*)p;
	return a >= (const uint8_t*)tib->StackLimit && a + n <= (const uint8_t*)tib->StackBase;
}

// First person: the last few look-ats the camera update made (logic thread), for the render thread to
// pick the one of the frame it draws.
struct LookAt {
	float eye[3], target[3];
	DWORD tick;
};
SRWLOCK s_laLock = SRWLOCK_INIT;
LookAt s_la[6];
int s_laNext;
volatile LONG s_fpCull;  // culling views moved to the first-person position

extern "C" void nvr_head_cull_builder(float* eye, float* target, float* up) {
	if (s_headCullRefused) return;
	bool stack = OnThisStack(eye, 48);
	if (target != eye + 4 || up != eye + 8 || (!stack && !s_hcRestoreOk)) {
		s_headCullRefused = true;
		nvr_head_cull = 0;
		Log("culling: head direction: the frustum builder's look-at (eye %p, target %p, up %p) isn't laid out as "
			"expected or can't be put back after the call; head culling stays off (send me this log)",
			(void*)eye, (void*)target, (void*)up);
		return;
	}
	static bool whereLogged;
	if (!whereLogged) {
		whereLogged = true;
		char where[300];
		DescribeAddress(eye, where, sizeof(where));
		Log("culling: head direction: look-at block at %s (%s)", where,
			stack ? "on the stack" : "game data: put back right after the call");
	}
	{
		AcquireSRWLockExclusive(&s_laLock);
		LookAt& l = s_la[s_laNext];
		memcpy(l.eye, eye, 12);
		memcpy(l.target, target, 12);
		l.tick = GetTickCount();
		s_laNext = (s_laNext + 1) % 6;
		ReleaseSRWLockExclusive(&s_laLock);
	}
	// Head culling: turn the view the way the headset looks. First person: also move it to where your
	// eyes are, so what's behind the third-person camera's position isn't culled.
	pure::M4 rel;
	float t[3], u[3];
	bool turn = g_cfg.headCulling && StereoHeadRelative(&rel) && pure::head_lookat(eye, target, up, rel, t, u);
	if (g_cfg.headCulling && !turn) InterlockedIncrement(&s_hcSkipped);
	if (!turn) {
		memcpy(t, target, 12);
		memcpy(u, up, 12);
	}
	float fp[3], shift[3] = {0, 0, 0};
	bool moved = g_cfg.firstPerson && StereoWatching() &&
				 pure::first_person_pos(eye, target, g_cfg.fpHeight, g_cfg.fpForward, fp);
	if (moved)
		for (int k = 0; k < 3; k++) shift[k] = fp[k] - eye[k];
	if (!turn && !moved) return;
	if (moved) InterlockedIncrement(&s_fpCull);
	if (turn && !s_hcLogged) {
		s_hcLogged = true;
		Log("culling: head direction: first turn; eye %.2f %.2f %.2f, target %.2f %.2f %.2f -> %.2f %.2f %.2f, "
			"up %.3f %.3f %.3f -> %.3f %.3f %.3f",
			(double)eye[0], (double)eye[1], (double)eye[2], (double)target[0], (double)target[1], (double)target[2],
			(double)t[0], (double)t[1], (double)t[2], (double)up[0], (double)up[1], (double)up[2], (double)u[0],
			(double)u[1], (double)u[2]);
	}
	if (!stack) {
		memcpy(s_hcSaved, eye, 12);
		memcpy(s_hcSaved + 3, target, 12);
		memcpy(s_hcSaved + 6, up, 12);
		s_hcTid = GetCurrentThreadId();
		s_hcBlock = eye;
	}
	for (int k = 0; k < 3; k++) {
		eye[k] += shift[k];
		t[k] += shift[k];
	}
	memcpy(target, t, 12);
	memcpy(up, u, 12);
	if (turn) InterlockedIncrement(&s_hcDone);
}

// culling_stubs.S (nvr_post_builder): right after the frustum builder call returns.
extern "C" void nvr_head_cull_restore() {
	float* b = s_hcBlock;
	if (!b || s_hcTid != GetCurrentThreadId()) return;
	memcpy(b, s_hcSaved, 12);
	memcpy(b + 4, s_hcSaved + 3, 12);
	memcpy(b + 8, s_hcSaved + 6, 12);
	s_hcBlock = nullptr;
	InterlockedIncrement(&s_hcRestored);
}

// The getter: `mov rax,[rcx]; movss xmm0,[rax+0x274]; ret`, ~11 calls a frame from 5-6 places.
// 0.3.17 left it alone and culling went back to the game's own fov, so culling frusta are built
// from its result. But the camera controller also reads it to ease towards its target fov, which is
// how widening polluted the game's own fov (0.3.16, 0.3.18). 0.3.18 guessed the controller by
// distance to the SetFov call: wrong (0.3.18 log: SetFov from exe+0x73e272, getter callers at
// +0x7807ed/+0x7808df 4x a frame, +0x75164f/+0x7397de/+0x729b49 1x a frame, none nearby).
// 0.3.19: the controller is the caller that reads the fov last before SetFov, on SetFov's thread.
// For the first 60 SetFov calls with stereo on every caller gets the real value while the mod
// counts who that is; then that caller keeps the real value and all others get the wide one.
typedef float (*PFN_GetFov)(void** cam);
PFN_GetFov o_GetFov;
bool s_getterOk;
struct GetterCaller {
	void* ra;
	volatile LONG count;
	volatile LONG votes;  // times it was the last reader before SetFov
	bool rejected;        // picked before, but the game's own fov still drifted wide: not the easing
	bool real;            // FovRealCallers: always the game's own fov
	unsigned off;         // exe offset of the return address
};
// Site 0 (the projection rebuild) gets the wide fov only with WideProjection: it makes Pod 042 aim up.
bool SiteOn(int i) { return i != 0 || g_cfg.wideProjection; }
GetterCaller s_callers[16];
volatile LONG s_callerCount;
SRWLOCK s_callerLock = SRWLOCK_INIT;
volatile DWORD s_setFovThread;
GetterCaller* volatile s_lastOnSetFovThread;
GetterCaller* volatile s_controller;  // decided after 60 votes
volatile LONG s_votes;
bool s_controllerLogged;
void* s_knownEasingRa;   // from the ini (FovEasingCaller): the easing caller found in an earlier run
bool s_easingSaved;

GetterCaller* NoteCaller(void* ra) {
	LONG n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++)
		if (s_callers[i].ra == ra) return &s_callers[i];
	AcquireSRWLockExclusive(&s_callerLock);
	GetterCaller* e = nullptr;
	n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++)
		if (s_callers[i].ra == ra) e = &s_callers[i];
	if (!e && n < 16) {
		e = &s_callers[n];
		e->ra = ra;
		e->count = 0;
		e->votes = 0;
		unsigned off = (unsigned)((uint8_t*)ra - (uint8_t*)GetModuleHandleW(nullptr));
		for (int i = 0; i < g_cfg.fovRealCount; i++)
			if (g_cfg.fovRealCallers[i] == off) e->real = true;
		e->off = off;
		InterlockedIncrement(&s_callerCount);
		if (ra == s_knownEasingRa && !s_controller) {
			s_controller = e;  // known from an earlier run: no search needed
			s_easingSaved = true;
		}
	}
	ReleaseSRWLockExclusive(&s_callerLock);
	return e;
}

float H_GetFov(void** cam) {
	float v = o_GetFov(cam);
	void* obj = nvr_fov_obj;
	if (obj && cam && *cam == obj) {
		GetterCaller* e = NoteCaller(__builtin_return_address(0));
		if (e) {
			InterlockedIncrement(&e->count);
			if (GetCurrentThreadId() == s_setFovThread) s_lastOnSetFovThread = e;
			GetterCaller* ctl = s_controller;
			if (ctl && e != ctl && !e->real) return nvr_fov_wide;
		}
	}
	return v;
}

// From H_SetFov while widening: count which getter caller read the fov just before.
void VoteController() {
	GetterCaller* last = s_lastOnSetFovThread;
	s_lastOnSetFovThread = nullptr;
	if (s_controller || !last) return;
	InterlockedIncrement(&last->votes);
	// 0.3.24: with the caller known from the ini, wait for it (it only starts reading once the
	// gameplay camera runs; 0.3.21-0.3.23 voted first, picked wrong callers, the game's own fov
	// drifted wide meanwhile and Pod 042 aimed at the sky for the first ~15 s)
	if (InterlockedIncrement(&s_votes) < (s_knownEasingRa ? 3600 : 60)) return;
	GetterCaller* best = nullptr;
	LONG n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++)
		if (!s_callers[i].rejected && (!best || s_callers[i].votes > best->votes)) best = &s_callers[i];
	s_controller = best;
	s_controllerLogged = false;
}

// If the game's own fov still climbs towards the wide one with a controller picked, the pick was
// wrong: drop it and count again without it.
int s_driftRun;
void CheckDrift(float requested, float want) {
	GetterCaller* ctl = s_controller;
	if (!ctl) {
		s_driftRun = 0;
		return;
	}
	s_driftRun = requested > want - 0.26f ? s_driftRun + 1 : 0;  // within 15 deg of the wide fov
	if (s_driftRun < 30) return;
	s_driftRun = 0;
	ctl->rejected = true;
	LONG n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++) s_callers[i].votes = 0;
	s_votes = 0;
	s_controller = nullptr;
	char where[300];
	DescribeAddress(ctl->ra, where, sizeof(where));
	Log("culling: the game's own fov kept drifting wide with %s as the camera's easing; looking again", where);
	s_easingSaved = false;
}

// 0.3.24: with head culling the frustum is centred on where you look, so it only has to hold the
// headset's view plus a margin (~135 deg tall instead of 170: far fewer objects drawn).
volatile LONG s_hcFovLogged;
float HeadCullFov(float want) {
	if (!nvr_head_cull || g_cfg.headCullMargin <= 0) return want;
	float f = StereoHeadCullFov(g_cfg.headCullMargin);
	if (!(f > 0.5f) || f >= want) return want;
	if (!InterlockedExchange(&s_hcFovLogged, 1))
		Log("culling: head direction: culling view %.0f deg tall (headset view + %.0f deg margin) instead of %.0f",
			(double)(f * 57.29578f), (double)g_cfg.headCullMargin, (double)(want * 57.29578f));
	return f;
}

void H_SetFov(void** cam, float fov) {
	s_setFovThread = GetCurrentThreadId();
	if (cam && *cam) {
		const float* obj = (const float*)*cam;
		float p00 = obj[16], p11 = obj[21];  // the camera's projection lives at +0x40 (P00, P11)
		float aspect = p00 > 1e-4f ? p11 / p00 : 0;
		bool main = aspect > 1.2f && aspect < 3.5f;
		if (main) s_gameFov = fov;
		float want = g_cfg.cullFov * 0.0174533f;
		if (s_wideOn && main && want > fov) {
			InterlockedIncrement(&s_widened);
			if (s_siteA) {
				// 0.3.17: the object keeps the game's own fov; only its projection/frustum reads
				// see the wide one
				if (s_getterOk && nvr_fov_obj == *cam) {
					VoteController();
					CheckDrift(fov, want);
				}
				nvr_fov_wide = want;
				nvr_fov_wide_b = HeadCullFov(want);
				nvr_fov_obj = *cam;
				for (int i = 0; i < 4; i++) nvr_fov_site_obj[i] = SiteOn(i) ? *cam : nullptr;
			} else {
				fov = want;
			}
		} else if (s_wideOn) {
			InterlockedIncrement(&s_passed);
		}
	}
	o_SetFov(cam, fov);
}

static const uint8_t kSetFovBytes[] = {0x48, 0x8b, 0x01, 0xf3, 0x0f, 0x11, 0x88, 0x74, 0x02, 0x00, 0x00,
									   0xc7, 0x80, 0x84, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3};

// Where the camera's fov field (+0x274) is read (0.3.10 hardware-breakpoint watch):
//   exe+0x27aaaf  camera update, when dirty: movss xmm1,[rbx+0x274] -> * k -> projection
//   exe+0x27abf7  camera update: movss xmm1,[rax+0x274] -> argument of a frustum builder
//   exe+0x22392d, exe+0x223fd6  render threads: movss xmm0,[rax+0x274] -> * k -> [rbx+0xd4]
//   exe+0x19415a  worker, ~70x per frame: model detail (screen size of each object)
//   exe+0x27ad44  getter (camera controller: eases the fov from the current value)
// 0.3.11-0.3.16 widened the stored value, so all of them saw 120 deg: the controller then eased
// from 120 (the game "asked" for 113-119, 0.3.16 log) and model detail dropped. Now only the first
// four are redirected (culling_stubs.S); each is found by its bytes plus neighbours and must match once.
struct FovSite {
	const char* what;
	uint8_t bytes[28];
	int len, at;  // pattern length, offset of the movss in it
};
static const FovSite kSites[4] = {
	{"projection rebuild",
	 {0x83, 0xbb, 0x84, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xb4, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x8b, 0x74, 0x02,
	  0x00, 0x00, 0xf3, 0x0f, 0x59, 0x0d},
	 25,
	 13},
	{"frustum builder",
	 {0xf3, 0x0f, 0x10, 0x80, 0x70, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x30, 0xf3, 0x0f, 0x10, 0x88, 0x74,
	  0x02, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x44, 0x24, 0x28},
	 28,
	 14},
	{"render camera 1",
	 {0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x59, 0x05, 0x2b, 0x6f, 0xb9, 0x00, 0xf3, 0x0f, 0x11,
	  0x83, 0xd4, 0x00, 0x00, 0x00},
	 24,
	 0},
	{"render camera 2",
	 {0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x59, 0x05, 0x82, 0x68, 0xb9, 0x00, 0xf3, 0x0f, 0x11,
	  0x86, 0xd4, 0x00, 0x00, 0x00},
	 24,
	 0},
};

static uint8_t* FindOnce(HMODULE exe, const uint8_t* pat, int len, int* matches) {
	auto* dos = (IMAGE_DOS_HEADER*)exe;
	auto* nt = (IMAGE_NT_HEADERS64*)((char*)exe + dos->e_lfanew);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	uint8_t* found = nullptr;
	*matches = 0;
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
		uint8_t* b = (uint8_t*)exe + sec[i].VirtualAddress;
		size_t n = sec[i].Misc.VirtualSize;
		for (size_t k = 0; k + (size_t)len <= n; k++)
			if (b[k] == pat[0] && memcmp(b + k, pat, (size_t)len) == 0) {
				found = b + k;
				(*matches)++;
			}
	}
	return *matches == 1 ? found : nullptr;
}

static void FovSitesInstall(HMODULE exe) {
	if (!g_cfg.lodFix) {
		Log("culling: LodFix=0, the camera's stored field of view is widened (model detail drops)");
		return;
	}
	void (*stubs[4])() = {nvr_fov_stub0, nvr_fov_stub1, nvr_fov_stub2, nvr_fov_stub3};
	uint8_t* at[4] = {};
	for (int i = 0; i < 4; i++) {
		int m = 0;
		uint8_t* p = FindOnce(exe, kSites[i].bytes, kSites[i].len, &m);
		if (!p) {
			Log("culling: fov read '%s' %s", kSites[i].what, m ? "found more than once" : "not found");
			continue;
		}
		at[i] = p + kSites[i].at;
	}
	if (!at[0]) {
		Log("culling: without the projection site the stored field of view is widened as before (model detail drops)");
		return;
	}
	uint8_t* post = nullptr;
	if (at[1]) {
		// movss [rsp+0x28],xmm0; movss [rsp+0x20],xmm1; call builder; mov rbp,[rsp+0x90] <- hooked
		static const uint8_t kArgs[] = {0xf3, 0x0f, 0x11, 0x44, 0x24, 0x28, 0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x20, 0xe8};
		static const uint8_t kAfter[] = {0x48, 0x8b, 0xac, 0x24, 0x90, 0x00, 0x00, 0x00};
		if (memcmp(at[1] + 8, kArgs, sizeof(kArgs)) == 0 && memcmp(at[1] + 25, kAfter, sizeof(kAfter)) == 0)
			post = at[1] + 25;
		else
			Log("culling: head direction: the code after the frustum builder read isn't as expected; head culling off");
	}
	for (int i = 0; i < 4; i++) {
		if (!at[i]) continue;
		nvr_fov_resume[i] = at[i] + 8;
		bool ok = HookFunction(at[i], (void*)stubs[i], &nvr_fov_orig[i], kSites[i].what);
		if (i == 0 && !ok) return;
		if (ok) s_sitesOk++;
		char where[300];
		DescribeAddress(at[i], where, sizeof(where));
		Log("culling: fov read '%s' at %s %s", kSites[i].what, where, ok ? "widened" : "could not be changed");
	}
	if (post) {
		s_hcRestoreOk = HookFunction(post, (void*)nvr_post_builder, &nvr_post_orig, "after frustum builder");
		char where[300];
		DescribeAddress(post, where, sizeof(where));
		Log("culling: head direction: after-call hook at %s %s", where, s_hcRestoreOk ? "in place" : "failed (head culling off)");
	}
	s_siteA = true;
	Log("culling: the camera keeps the game's own field of view; model detail follows it");
	static const uint8_t kGetter[] = {0xf3, 0x0f, 0x10, 0x80, 0x6c, 0x02, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
									  0x48, 0x8b, 0x01, 0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xc3};
	int m = 0;
	uint8_t* g = FindOnce(exe, kGetter, (int)sizeof(kGetter), &m);
	if (!g) {
		Log("culling: fov getter %s; culling may stay narrow", m ? "found more than once" : "not found");
		return;
	}
	if (g_cfg.fovEasingCaller) s_knownEasingRa = (uint8_t*)exe + g_cfg.fovEasingCaller;
	s_getterOk = HookFunction(g + 13, (void*)&H_GetFov, (void**)&o_GetFov, "fov getter");
	char where[300];
	DescribeAddress(g + 13, where, sizeof(where));
	Log("culling: fov getter at %s %s", where, s_getterOk ? "hooked: culling gets the wide view, the camera's easing the real one" : "could not be hooked");
}

// Finds the setter by its exact bytes in the exe's code (so another build of the game either
// matches exactly or is left alone).
void CullingInstallHook() {
	if (g_cfg.cullFov <= 1) {
		Log("culling: CullingFov=0, game culling left as it is");
		return;
	}
	HMODULE exe = GetModuleHandleW(nullptr);
	auto* dos = (IMAGE_DOS_HEADER*)exe;
	auto* nt = (IMAGE_NT_HEADERS64*)((char*)exe + dos->e_lfanew);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	uint8_t* found = nullptr;
	int matches = 0;
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
		uint8_t* b = (uint8_t*)exe + sec[i].VirtualAddress;
		size_t n = sec[i].Misc.VirtualSize;
		for (size_t k = 0; k + sizeof(kSetFovBytes) <= n; k++)
			if (b[k] == 0x48 && memcmp(b + k, kSetFovBytes, sizeof(kSetFovBytes)) == 0) {
				found = b + k;
				matches++;
			}
	}
	if (matches != 1) {
		Log("culling: camera fov setter %s; culling left as it is (send me the log)",
			matches ? "found more than once" : "not found (different game version?)");
		return;
	}
	s_hookOk = HookFunction(found, (void*)&H_SetFov, (void**)&o_SetFov, "camera SetFov");
	char where[300];
	DescribeAddress(found, where, sizeof(where));
	Log("culling: %s camera fov setter at %s; during stereo the game culls with %.0f deg", s_hookOk ? "hooked" : "could not hook",
		where, (double)g_cfg.cullFov);
	if (s_hookOk) FovSitesInstall(exe);
}

// Game frame times (0.3.24: "jitter/stutter" with head culling), reported with the 30 s summary.
LARGE_INTEGER s_qpf, s_lastFrame;
double s_ftSum, s_ftMax;
LONG s_ftCount, s_ftSlow, s_ftHc;

bool CullingLookAt(const float camPos[3], float eye[3], float target[3], bool* matched) {
	DWORD now = GetTickCount();
	int best = -1, newest = -1;
	float bestD = 0.05f * 0.05f;  // the frame's own look-at: its eye within 5 cm of the camera
	AcquireSRWLockShared(&s_laLock);
	for (int i = 0; i < 6; i++) {
		const LookAt& l = s_la[i];
		if (!l.tick || now - l.tick > 250) continue;
		if (newest < 0 || (int)(l.tick - s_la[newest].tick) >= 0) newest = i;
		float dx = l.eye[0] - camPos[0], dy = l.eye[1] - camPos[1], dz = l.eye[2] - camPos[2];
		float d = dx * dx + dy * dy + dz * dz;
		if (d <= bestD) best = i, bestD = d;
	}
	int i = best >= 0 ? best : newest;
	if (i >= 0) {
		memcpy(eye, s_la[i].eye, 12);
		memcpy(target, s_la[i].target, 12);
	}
	ReleaseSRWLockShared(&s_laLock);
	*matched = best >= 0;
	return i >= 0;
}

void CullingUpdate(bool wide) {
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
		if (s_lastFrame.QuadPart && wide) {
			double ms = (double)(now.QuadPart - s_lastFrame.QuadPart) * 1000.0 / (double)s_qpf.QuadPart;
			if (ms < 1000) {
				s_ftSum += ms;
				s_ftCount++;
				if (ms > s_ftMax) s_ftMax = ms;
				if (ms > 25) s_ftSlow++;
				if (nvr_head_cull) s_ftHc++;
			}
		}
		s_lastFrame = now;
	}
	InterlockedExchange(&s_wideOn, wide && s_hookOk && g_cfg.cullFov > 1 ? 1 : 0);
	if (!s_wideOn) {
		nvr_fov_obj = nullptr;
		for (int i = 0; i < 4; i++) nvr_fov_site_obj[i] = nullptr;
	}
	nvr_head_cull = s_wideOn && (g_cfg.headCulling || g_cfg.firstPerson) && !s_headCullRefused ? 1 : 0;
	static DWORD last;
	if (wide && s_hookOk && GetTickCount() - last > 30000) {  // a short report now and then
		last = GetTickCount();
		LONG w = InterlockedExchange(&s_widened, 0), p = InterlockedExchange(&s_passed, 0);
		if (w || p) Log("culling: %ld camera updates widened, %ld left alone (other cameras) in the last 30 s", (long)w, (long)p);
		if (s_ftCount) {
			Log("culling: game frames in stereo: %ld, avg %.1f ms, slowest %.1f ms, %ld slower than 25 ms (head culling on in %ld%%)",
				(long)s_ftCount, s_ftSum / s_ftCount, s_ftMax, (long)s_ftSlow, (long)(s_ftHc * 100 / s_ftCount));
			{
				char info[160];
				snprintf(info, sizeof(info), "Game frame time %.1f ms average, slowest %.1f ms, %ld slower than 25 ms",
						 s_ftSum / s_ftCount, s_ftMax, (long)s_ftSlow);
				MenuSetInfo(2, info);
			}
			s_ftSum = s_ftMax = 0;
			s_ftCount = s_ftSlow = s_ftHc = 0;
		}
		if (s_siteA) {
			LONG64 h[4];
			for (int i = 0; i < 4; i++) h[i] = InterlockedExchange64(&nvr_fov_hits[i], 0);
			Log("culling: widened reads: projection %lld, frustum %lld, render cameras %lld / %lld; game's own fov %.0f deg",
				(long long)h[0], (long long)h[1], (long long)h[2], (long long)h[3], (double)(s_gameFov * 57.29578f));
			LONG n = s_callerCount;
			GetterCaller* ctl = s_controller;
			for (LONG i = 0; i < n && i < 16; i++) {
				LONG c = InterlockedExchange(&s_callers[i].count, 0);
				if (!c) continue;
				char where[300];
				DescribeAddress(s_callers[i].ra, where, sizeof(where));
				const char* value = "wide value";
				if (!ctl) value = "real value (still finding the camera's easing)";
				else if (&s_callers[i] == ctl) value = "real value (camera easing)";
				else if (s_callers[i].real) value = "real value (FovRealCallers)";
				Log("culling:   fov getter caller %s: %ld calls, %ld votes, %s", where, (long)c, (long)s_callers[i].votes, value);
			}
			{
				LONG d = InterlockedExchange(&s_hcDone, 0), k = InterlockedExchange(&s_hcSkipped, 0);
				LONG r = InterlockedExchange(&s_hcRestored, 0);
				if (g_cfg.headCulling && !s_headCullRefused)
					Log("culling: head direction: culling view turned %ld times (%ld put back after), %ld left as the "
						"game's (no recent eye pose)",
						(long)d, (long)r, (long)k);
			}
			if (ctl && !s_easingSaved && s_gameFov < nvr_fov_wide - 0.5f) {
				s_easingSaved = true;
				unsigned off = (unsigned)((uint8_t*)ctl->ra - (uint8_t*)GetModuleHandleW(nullptr));
				ConfigSaveEasingCaller(off);
				Log("culling: camera easing caller saved to NieRVR.ini (FovEasingCaller=0x%x): no search next time", off);
			}
			if (ctl && !s_controllerLogged) {
				s_controllerLogged = true;
				char where[300];
				DescribeAddress(ctl->ra, where, sizeof(where));
				Log("culling: the camera's easing reads the fov at %s (%ld of %ld votes)", where, (long)ctl->votes, (long)s_votes);
			}
		}
	}
}

// From PatchScene: the vertical fov the game rendered this frame with (before our replacement).
float CullingGameFov() { return s_gameFov * 57.29578f; }

void CullingObserveGameFov(float deg) {
	if (s_loggedEffect || !s_hookOk) return;
	float want = g_cfg.cullFov;
	if (deg > want - 2 && deg < want + 2) {
		s_loggedEffect = true;
		Log("culling: confirmed, the game's own camera now uses %.0f deg (culling follows it)", (double)deg);
	}
}

// Ctrl+Shift+F5: head-direction culling on/off (saved to NieRVR.ini).
void CullingToggleHeadCulling() {
	g_cfg.headCulling = !g_cfg.headCulling;
	s_hcFovLogged = 0;
	if (s_headCullRefused)
		Log("culling: head direction culling can't be used with this game build (see the earlier log line)");
	else
		Log("culling: head direction culling %s", g_cfg.headCulling ? "ON (the culling view follows your head)"
																	: "OFF (the culling view follows the game camera)");
	ConfigSave(&g_cfg.headCulling);
}
