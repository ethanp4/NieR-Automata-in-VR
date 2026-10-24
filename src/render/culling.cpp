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

// culling_stubs.S: the redirected fov reads jump into stubs there; these are what they use. nvr_fov_obj is
// the main camera object (0 = off), nvr_fov_wide the widened fov (radians).
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

namespace {
bool s_loggedEffect;  // the game camera was seen using the wide fov

// ---------------------------------------------------------------------------------------------
// The camera controller sets the fov every frame through a tiny setter,
//   NieRAutomata.exe+0x27ace0:  mov rax,[rcx]; movss [rax+0x274],xmm1; mov dword [rax+0x284],1; ret
// (fov at +0x274, "dirty" flag at +0x284). When the flag is set, the camera update (exe+0x27aaa2)
// rebuilds the projection and the culling frustum from that fov, so a value written anywhere else is
// overwritten before the next frame is culled: the setter is hooked instead. Only the main 16:9
// cameras are widened (render-to-texture cameras are square), and only while stereo is rendering.
// ---------------------------------------------------------------------------------------------
typedef void (*PFN_SetFov)(void** cam, float fov);
PFN_SetFov o_SetFov;
volatile LONG s_widening;
volatile LONG s_widenedCount, s_leftAloneCount;
bool s_setterHooked;

volatile float s_gameFov;  // the last field of view the game asked for (main camera, radians)
bool s_readSitesHooked;  // the fov reads are redirected (else the stored fov itself is widened: lower model detail)

// ---------------------------------------------------------------------------------------------
// Head-direction culling. A symmetric frustum can't reach past 90 deg to a side (at CullingFov=170 it
// reaches ~87), so turning your head more than ~35-40 deg away from where the game camera points took the
// edge of your view out of it. The frustum builder after site B gets LookAt(eye, target, up) as three
// pointers (r14, rbp, rsi); culling_stubs.S calls nvr_head_cull_builder right before that call, which
// turns target and up by the eye's rotation relative to the game camera (StereoHeadRelative), so the
// culling view looks where you look.
// ---------------------------------------------------------------------------------------------
volatile LONG s_headCullTurned, s_headCullSkipped;
bool s_headCullRefused, s_loggedFirstTurn;
// The look-at is a temporary on this thread's stack, or a fixed block in the game's data: then the
// original is put back right after the builder returns (nvr_post_builder hooks the instruction after
// the call), so the camera itself never changes. If neither works, head culling switches itself off.
bool s_restoreHooked;      // the after-call hook is in place
float* volatile s_changedLookAt;  // block changed and not yet restored (0 = none)
DWORD s_changedLookAtThread;
float s_savedLookAt[9];  // eye, target, up
volatile LONG s_headCullRestored;

bool OnThisStack(const void* p, size_t n) {
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
SRWLOCK s_lookAtLock = SRWLOCK_INIT;
LookAt s_recentLookAts[6];
int s_nextLookAt;
}  // namespace

extern "C" void nvr_head_cull_builder(float* eye, float* target, float* up) {
	if (s_headCullRefused) return;
	bool stack = OnThisStack(eye, 48);
	if (target != eye + 4 || up != eye + 8 || (!stack && !s_restoreHooked)) {
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
		AcquireSRWLockExclusive(&s_lookAtLock);
		LookAt& lookAt = s_recentLookAts[s_nextLookAt];
		memcpy(lookAt.eye, eye, 12);
		memcpy(lookAt.target, target, 12);
		lookAt.tick = GetTickCount();
		s_nextLookAt = (s_nextLookAt + 1) % 6;
		ReleaseSRWLockExclusive(&s_lookAtLock);
	}
	// Head culling: turn the view the way the headset looks. First person: also move it to where your
	// eyes are, so what's behind the third-person camera's position isn't culled.
	pure::M4 rel;
	float turnedTarget[3], turnedUp[3];
	bool turn = g_cfg.headCulling && StereoHeadRelative(&rel) && pure::head_lookat(eye, target, up, rel, turnedTarget, turnedUp);
	if (g_cfg.headCulling && !turn) InterlockedIncrement(&s_headCullSkipped);
	if (!turn) {
		memcpy(turnedTarget, target, 12);
		memcpy(turnedUp, up, 12);
	}
	float firstPersonEye[3], shift[3] = {0, 0, 0};
	bool moved = g_cfg.firstPerson && StereoWatching() &&
				 pure::first_person_pos(eye, target, g_cfg.fpHeight, g_cfg.fpForward, firstPersonEye);
	if (moved)
		for (int k = 0; k < 3; k++) shift[k] = firstPersonEye[k] - eye[k];
	if (!turn && !moved) return;
	if (turn && !s_loggedFirstTurn) {
		s_loggedFirstTurn = true;
		Log("culling: head direction: first turn; eye %.2f %.2f %.2f, target %.2f %.2f %.2f -> %.2f %.2f %.2f, "
			"up %.3f %.3f %.3f -> %.3f %.3f %.3f",
			(double)eye[0], (double)eye[1], (double)eye[2], (double)target[0], (double)target[1], (double)target[2],
			(double)turnedTarget[0], (double)turnedTarget[1], (double)turnedTarget[2], (double)up[0], (double)up[1], (double)up[2], (double)turnedUp[0],
			(double)turnedUp[1], (double)turnedUp[2]);
	}
	if (!stack) {
		memcpy(s_savedLookAt, eye, 12);
		memcpy(s_savedLookAt + 3, target, 12);
		memcpy(s_savedLookAt + 6, up, 12);
		s_changedLookAtThread = GetCurrentThreadId();
		s_changedLookAt = eye;
	}
	for (int k = 0; k < 3; k++) {
		eye[k] += shift[k];
		turnedTarget[k] += shift[k];
	}
	memcpy(target, turnedTarget, 12);
	memcpy(up, turnedUp, 12);
	if (turn) InterlockedIncrement(&s_headCullTurned);
}

// culling_stubs.S (nvr_post_builder): right after the frustum builder call returns.
extern "C" void nvr_head_cull_restore() {
	float* block = s_changedLookAt;
	if (!block || s_changedLookAtThread != GetCurrentThreadId()) return;
	memcpy(block, s_savedLookAt, 12);
	memcpy(block + 4, s_savedLookAt + 3, 12);
	memcpy(block + 8, s_savedLookAt + 6, 12);
	s_changedLookAt = nullptr;
	InterlockedIncrement(&s_headCullRestored);
}

namespace {
// The fov getter, `mov rax,[rcx]; movss xmm0,[rax+0x274]; ret`, is called ~11 times a frame from 5-6
// places. Culling frusta are built from its result, so it has to return the wide fov. But the camera's
// easing (the controller moving the fov towards its target) reads it too, and given the wide value it
// drifts the game's own fov wide (Pod 042 then aims at the sky). So the easing caller gets the real fov
// and every other caller the wide one. The easing caller is the one that reads last before SetFov, on
// SetFov's thread: found by votes over the first SetFov calls (every caller gets the real value
// meanwhile), or known from an earlier run (FovEasingCaller in the ini).
typedef float (*PFN_GetFov)(void** cam);
PFN_GetFov o_GetFov;
bool s_getterHooked;
struct GetterCaller {
	void* returnAddress;
	volatile LONG calls;
	volatile LONG votes;  // times it was the last reader before SetFov
	bool rejected;        // picked before, but the game's own fov still drifted wide: not the easing
	bool alwaysReal;      // FovRealCallers: always the game's own fov
	unsigned exeOffset;   // exe offset of the return address
};
// Site 0 (the projection rebuild) keeps the game's own fov: the wide one made Pod 042 aim at the sky.
bool SiteGetsWideFov(int i) { return i != 0; }
GetterCaller s_callers[16];
volatile LONG s_callerCount;
SRWLOCK s_callerLock = SRWLOCK_INIT;
volatile DWORD s_setFovThread;
GetterCaller* volatile s_lastReaderOnSetFovThread;
GetterCaller* volatile s_easingCaller;  // decided after 60 votes
volatile LONG s_voteCount;
bool s_loggedEasingCaller;
void* s_savedEasingCaller;   // from the ini (FovEasingCaller): the easing caller found in an earlier run
bool s_easingCallerSaved;

GetterCaller* FindOrAddCaller(void* ra) {
	LONG n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++)
		if (s_callers[i].returnAddress == ra) return &s_callers[i];
	AcquireSRWLockExclusive(&s_callerLock);
	GetterCaller* e = nullptr;
	n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++)
		if (s_callers[i].returnAddress == ra) e = &s_callers[i];
	if (!e && n < 16) {
		e = &s_callers[n];
		e->returnAddress = ra;
		e->calls = 0;
		e->votes = 0;
		unsigned off = (unsigned)((uint8_t*)ra - (uint8_t*)GetModuleHandleW(nullptr));
		for (int i = 0; i < g_cfg.fovRealCount; i++)
			if (g_cfg.fovRealCallers[i] == off) e->alwaysReal = true;
		e->exeOffset = off;
		InterlockedIncrement(&s_callerCount);
		if (ra == s_savedEasingCaller && !s_easingCaller) {
			s_easingCaller = e;  // known from an earlier run: no search needed
			s_easingCallerSaved = true;
		}
	}
	ReleaseSRWLockExclusive(&s_callerLock);
	return e;
}

float H_GetFov(void** cam) {
	float v = o_GetFov(cam);
	void* obj = nvr_fov_obj;
	if (obj && cam && *cam == obj) {
		GetterCaller* e = FindOrAddCaller(__builtin_return_address(0));
		if (e) {
			InterlockedIncrement(&e->calls);
			if (GetCurrentThreadId() == s_setFovThread) s_lastReaderOnSetFovThread = e;
			GetterCaller* easing = s_easingCaller;
			if (easing && e != easing && !e->alwaysReal) return nvr_fov_wide;
		}
	}
	return v;
}

// From H_SetFov while widening: count which getter caller read the fov just before.
void VoteEasingCaller() {
	GetterCaller* last = s_lastReaderOnSetFovThread;
	s_lastReaderOnSetFovThread = nullptr;
	if (s_easingCaller || !last) return;
	InterlockedIncrement(&last->votes);
	// with the caller known from the ini, wait much longer before voting: it only starts reading once
	// the gameplay camera runs, and an early vote picks the wrong caller
	if (InterlockedIncrement(&s_voteCount) < (s_savedEasingCaller ? 3600 : 60)) return;
	GetterCaller* best = nullptr;
	LONG n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++)
		if (!s_callers[i].rejected && (!best || s_callers[i].votes > best->votes)) best = &s_callers[i];
	s_easingCaller = best;
	s_loggedEasingCaller = false;
}

// If the game's own fov still climbs towards the wide one with a controller picked, the pick was
// wrong: drop it and count again without it.
int s_driftFrames;
void CheckDrift(float requested, float want) {
	GetterCaller* easing = s_easingCaller;
	if (!easing) {
		s_driftFrames = 0;
		return;
	}
	s_driftFrames = requested > want - 0.26f ? s_driftFrames + 1 : 0;  // within 15 deg of the wide fov
	if (s_driftFrames < 30) return;
	s_driftFrames = 0;
	easing->rejected = true;
	LONG n = s_callerCount;
	for (LONG i = 0; i < n && i < 16; i++) s_callers[i].votes = 0;
	s_voteCount = 0;
	s_easingCaller = nullptr;
	char where[300];
	DescribeAddress(easing->returnAddress, where, sizeof(where));
	Log("culling: the game's own fov kept drifting wide with %s as the camera's easing; looking again", where);
	s_easingCallerSaved = false;
}

// With head culling the frustum is centred on where you look, so it only has to hold the
// headset's view plus a margin (~135 deg tall instead of 170: far fewer objects drawn).
volatile LONG s_loggedHeadCullFov;
float HeadCullFov(float want) {
	if (!nvr_head_cull || g_cfg.headCullMargin <= 0) return want;
	float f = StereoHeadCullFov(g_cfg.headCullMargin);
	if (!(f > 0.5f) || f >= want) return want;
	if (!InterlockedExchange(&s_loggedHeadCullFov, 1))
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
		float wideFov = g_cfg.cullFov * 0.0174533f;
		if (s_widening && main && wideFov > fov) {
			InterlockedIncrement(&s_widenedCount);
			if (s_readSitesHooked) {
				// the object keeps the game's own fov; only its projection and frustum reads see the wide one
				if (s_getterHooked && nvr_fov_obj == *cam) {
					VoteEasingCaller();
					CheckDrift(fov, wideFov);
				}
				nvr_fov_wide = wideFov;
				nvr_fov_wide_b = HeadCullFov(wideFov);
				nvr_fov_obj = *cam;
				for (int i = 0; i < 4; i++) nvr_fov_site_obj[i] = SiteGetsWideFov(i) ? *cam : nullptr;
			} else {
				fov = wideFov;
			}
		} else if (s_widening) {
			InterlockedIncrement(&s_leftAloneCount);
		}
	}
	o_SetFov(cam, fov);
}

const uint8_t kSetFovBytes[] = {0x48, 0x8b, 0x01, 0xf3, 0x0f, 0x11, 0x88, 0x74, 0x02, 0x00, 0x00,
								0xc7, 0x80, 0x84, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3};

// Where the camera's fov field (+0x274) is read:
//   exe+0x27aaaf  camera update, when dirty: movss xmm1,[rbx+0x274] -> * k -> projection
//   exe+0x27abf7  camera update: movss xmm1,[rax+0x274] -> argument of a frustum builder
//   exe+0x22392d, exe+0x223fd6  render threads: movss xmm0,[rax+0x274] -> * k -> [rbx+0xd4]
//   exe+0x19415a  worker, ~70x per frame: model detail (screen size of each object)
//   exe+0x27ad44  getter (camera controller: eases the fov from the current value)
// Only the first four are redirected (culling_stubs.S): widening the stored value instead made the easing
// start from the wide fov and dropped model detail. Each is found by its bytes plus neighbours and must
// match exactly once.
struct FovSite {
	const char* what;
	uint8_t bytes[28];
	int len, at;  // pattern length, offset of the movss in it
};
const FovSite kSites[4] = {
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

uint8_t* FindOnce(HMODULE exe, const uint8_t* pat, int len, int* matches) {
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

void FovSitesInstall(HMODULE exe) {
	void (*stubs[4])() = {nvr_fov_stub0, nvr_fov_stub1, nvr_fov_stub2, nvr_fov_stub3};
	uint8_t* siteAddr[4] = {};
	for (int i = 0; i < 4; i++) {
		int matches = 0;
		uint8_t* found = FindOnce(exe, kSites[i].bytes, kSites[i].len, &matches);
		if (!found) {
			Log("culling: fov read '%s' %s", kSites[i].what, matches ? "found more than once" : "not found");
			continue;
		}
		siteAddr[i] = found + kSites[i].at;
	}
	if (!siteAddr[0]) {
		Log("culling: without the projection site the stored field of view is widened as before (model detail drops)");
		return;
	}
	uint8_t* post = nullptr;
	if (siteAddr[1]) {
		// movss [rsp+0x28],xmm0; movss [rsp+0x20],xmm1; call builder; mov rbp,[rsp+0x90] <- hooked
		static const uint8_t kArgs[] = {0xf3, 0x0f, 0x11, 0x44, 0x24, 0x28, 0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x20, 0xe8};
		static const uint8_t kAfter[] = {0x48, 0x8b, 0xac, 0x24, 0x90, 0x00, 0x00, 0x00};
		if (memcmp(siteAddr[1] + 8, kArgs, sizeof(kArgs)) == 0 && memcmp(siteAddr[1] + 25, kAfter, sizeof(kAfter)) == 0)
			post = siteAddr[1] + 25;
		else
			Log("culling: head direction: the code after the frustum builder read isn't as expected; head culling off");
	}
	for (int i = 0; i < 4; i++) {
		if (!siteAddr[i]) continue;
		nvr_fov_resume[i] = siteAddr[i] + 8;
		bool ok = HookFunction(siteAddr[i], (void*)stubs[i], &nvr_fov_orig[i], kSites[i].what);
		if (i == 0 && !ok) return;
		char where[300];
		DescribeAddress(siteAddr[i], where, sizeof(where));
		Log("culling: fov read '%s' at %s %s", kSites[i].what, where, ok ? "widened" : "could not be changed");
	}
	if (post) {
		s_restoreHooked = HookFunction(post, (void*)nvr_post_builder, &nvr_post_orig, "after frustum builder");
		char where[300];
		DescribeAddress(post, where, sizeof(where));
		Log("culling: head direction: after-call hook at %s %s", where, s_restoreHooked ? "in place" : "failed (head culling off)");
	}
	s_readSitesHooked = true;
	Log("culling: the camera keeps the game's own field of view; model detail follows it");
	static const uint8_t kGetter[] = {0xf3, 0x0f, 0x10, 0x80, 0x6c, 0x02, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
									  0x48, 0x8b, 0x01, 0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xc3};
	int matches = 0;
	uint8_t* getter = FindOnce(exe, kGetter, (int)sizeof(kGetter), &matches);
	if (!getter) {
		Log("culling: fov getter %s; culling may stay narrow", matches ? "found more than once" : "not found");
		return;
	}
	if (g_cfg.fovEasingCaller) s_savedEasingCaller = (uint8_t*)exe + g_cfg.fovEasingCaller;
	s_getterHooked = HookFunction(getter + 13, (void*)&H_GetFov, (void**)&o_GetFov, "fov getter");
	char where[300];
	DescribeAddress(getter + 13, where, sizeof(where));
	Log("culling: fov getter at %s %s", where, s_getterHooked ? "hooked: culling gets the wide view, the camera's easing the real one" : "could not be hooked");
}

// Game frame times in stereo, for the 30 s report.
LARGE_INTEGER s_qpcFrequency, s_lastFrameQpc;
double s_frameMsSum, s_frameMsMax;
LONG s_frameCount, s_slowFrames, s_headCullFrames;

}  // namespace

// Finds the setter by its exact bytes in the exe's code (so another build of the game either
// matches exactly or is left alone).
void CullingInstallHook() {
	if (g_cfg.cullFov <= 1) {
		Log("culling: CullingFov=0, game culling left as it is");
		return;
	}
	HMODULE exe = GetModuleHandleW(nullptr);
	int matches = 0;
	uint8_t* found = FindOnce(exe, kSetFovBytes, (int)sizeof(kSetFovBytes), &matches);
	if (!found) {
		Log("culling: camera fov setter %s; culling left as it is (send me the log)",
			matches ? "found more than once" : "not found (different game version?)");
		return;
	}
	s_setterHooked = HookFunction(found, (void*)&H_SetFov, (void**)&o_SetFov, "camera SetFov");
	char where[300];
	DescribeAddress(found, where, sizeof(where));
	Log("culling: %s camera fov setter at %s; during stereo the game culls with %.0f deg", s_setterHooked ? "hooked" : "could not hook",
		where, (double)g_cfg.cullFov);
	if (s_setterHooked) FovSitesInstall(exe);
}

bool CullingLookAt(const float camPos[3], float eye[3], float target[3], bool* matched) {
	DWORD now = GetTickCount();
	int best = -1, newest = -1;
	float bestDistSq = 0.05f * 0.05f;  // the frame's own look-at: its eye within 5 cm of the camera
	AcquireSRWLockShared(&s_lookAtLock);
	for (int i = 0; i < 6; i++) {
		const LookAt& lookAt = s_recentLookAts[i];
		if (!lookAt.tick || now - lookAt.tick > 250) continue;
		if (newest < 0 || (int)(lookAt.tick - s_recentLookAts[newest].tick) >= 0) newest = i;
		float dx = lookAt.eye[0] - camPos[0], dy = lookAt.eye[1] - camPos[1], dz = lookAt.eye[2] - camPos[2];
		float distSq = dx * dx + dy * dy + dz * dz;
		if (distSq <= bestDistSq) best = i, bestDistSq = distSq;
	}
	int i = best >= 0 ? best : newest;
	if (i >= 0) {
		memcpy(eye, s_recentLookAts[i].eye, 12);
		memcpy(target, s_recentLookAts[i].target, 12);
	}
	ReleaseSRWLockShared(&s_lookAtLock);
	*matched = best >= 0;
	return i >= 0;
}

void CullingUpdate(bool wide) {
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (!s_qpcFrequency.QuadPart) QueryPerformanceFrequency(&s_qpcFrequency);
		if (s_lastFrameQpc.QuadPart && wide) {
			double ms = (double)(now.QuadPart - s_lastFrameQpc.QuadPart) * 1000.0 / (double)s_qpcFrequency.QuadPart;
			if (ms < 1000) {
				s_frameMsSum += ms;
				s_frameCount++;
				if (ms > s_frameMsMax) s_frameMsMax = ms;
				if (ms > 25) s_slowFrames++;
				if (nvr_head_cull) s_headCullFrames++;
			}
		}
		s_lastFrameQpc = now;
	}
	InterlockedExchange(&s_widening, wide && s_setterHooked && g_cfg.cullFov > 1 ? 1 : 0);
	if (!s_widening) {
		nvr_fov_obj = nullptr;
		for (int i = 0; i < 4; i++) nvr_fov_site_obj[i] = nullptr;
	}
	nvr_head_cull = s_widening && (g_cfg.headCulling || g_cfg.firstPerson) && !s_headCullRefused ? 1 : 0;
	static DWORD lastReport;
	if (wide && s_setterHooked && GetTickCount() - lastReport > 30000) {  // a short report now and then
		lastReport = GetTickCount();
		LONG widened = InterlockedExchange(&s_widenedCount, 0), leftAlone = InterlockedExchange(&s_leftAloneCount, 0);
		if (widened || leftAlone) Log("culling: %ld camera updates widened, %ld left alone (other cameras) in the last 30 s", (long)widened, (long)leftAlone);
		if (s_frameCount) {
			Log("culling: game frames in stereo: %ld, avg %.1f ms, slowest %.1f ms, %ld slower than 25 ms (head culling on in %ld%%)",
				(long)s_frameCount, s_frameMsSum / s_frameCount, s_frameMsMax, (long)s_slowFrames, (long)(s_headCullFrames * 100 / s_frameCount));
			{
				char info[160];
				snprintf(info, sizeof(info), "Game frame time %.1f ms average, slowest %.1f ms, %ld slower than 25 ms",
						 s_frameMsSum / s_frameCount, s_frameMsMax, (long)s_slowFrames);
				MenuSetInfo(2, info);
			}
			s_frameMsSum = s_frameMsMax = 0;
			s_frameCount = s_slowFrames = s_headCullFrames = 0;
		}
		if (s_readSitesHooked) {
			LONG64 readHits[4];
			for (int i = 0; i < 4; i++) readHits[i] = InterlockedExchange64(&nvr_fov_hits[i], 0);
			Log("culling: widened reads: projection %lld, frustum %lld, render cameras %lld / %lld; game's own fov %.0f deg",
				(long long)readHits[0], (long long)readHits[1], (long long)readHits[2], (long long)readHits[3], (double)(s_gameFov * 57.29578f));
			LONG n = s_callerCount;
			GetterCaller* easing = s_easingCaller;
			for (LONG i = 0; i < n && i < 16; i++) {
				LONG calls = InterlockedExchange(&s_callers[i].calls, 0);
				if (!calls) continue;
				char where[300];
				DescribeAddress(s_callers[i].returnAddress, where, sizeof(where));
				const char* value = "wide value";
				if (!easing) value = "real value (still finding the camera's easing)";
				else if (&s_callers[i] == easing) value = "real value (camera easing)";
				else if (s_callers[i].alwaysReal) value = "real value (FovRealCallers)";
				Log("culling:   fov getter caller %s: %ld calls, %ld votes, %s", where, (long)calls, (long)s_callers[i].votes, value);
			}
			{
				LONG turned = InterlockedExchange(&s_headCullTurned, 0), skipped = InterlockedExchange(&s_headCullSkipped, 0);
				LONG restored = InterlockedExchange(&s_headCullRestored, 0);
				if (g_cfg.headCulling && !s_headCullRefused)
					Log("culling: head direction: culling view turned %ld times (%ld put back after), %ld left as the "
						"game's (no recent eye pose)",
						(long)turned, (long)restored, (long)skipped);
			}
			if (easing && !s_easingCallerSaved && s_gameFov < nvr_fov_wide - 0.5f) {
				s_easingCallerSaved = true;
				unsigned off = (unsigned)((uint8_t*)easing->returnAddress - (uint8_t*)GetModuleHandleW(nullptr));
				ConfigSaveEasingCaller(off);
				Log("culling: camera easing caller saved to NieRVR.ini (FovEasingCaller=0x%x): no search next time", off);
			}
			if (easing && !s_loggedEasingCaller) {
				s_loggedEasingCaller = true;
				char where[300];
				DescribeAddress(easing->returnAddress, where, sizeof(where));
				Log("culling: the camera's easing reads the fov at %s (%ld of %ld votes)", where, (long)easing->votes, (long)s_voteCount);
			}
		}
	}
}

// From PatchScene: the vertical fov the game rendered this frame with (before our replacement).
float CullingGameFov() { return s_gameFov * 57.29578f; }

void CullingObserveGameFov(float deg) {
	if (s_loggedEffect || !s_setterHooked) return;
	float want = g_cfg.cullFov;
	if (deg > want - 2 && deg < want + 2) {
		s_loggedEffect = true;
		Log("culling: confirmed, the game's own camera now uses %.0f deg (culling follows it)", (double)deg);
	}
}

// Ctrl+Shift+F5: head-direction culling on/off (saved to NieRVR.ini).
void CullingToggleHeadCulling() {
	g_cfg.headCulling = !g_cfg.headCulling;
	s_loggedHeadCullFov = 0;
	if (s_headCullRefused)
		Log("culling: head direction culling can't be used with this game build (see the earlier log line)");
	else
		Log("culling: head direction culling %s", g_cfg.headCulling ? "ON (the culling view follows your head)"
																	: "OFF (the culling view follows the game camera)");
	ConfigSave(&g_cfg.headCulling);
}
