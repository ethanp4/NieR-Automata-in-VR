// camera.cpp - alternating stereo on the game side.
//
// NieR uploads its camera once per frame in a 256-byte cbuffer "SceneBuffer" (b0):
//   +0 g_View, +64 g_Proj, +128 g_ViewProjection (= Proj*View), +192 g_ViewInverseMatrix
// (column-vector matrices stored row by row; right-handed, +Y up, camera looks down -Z), and a
// 32-byte "CamParam_HPixel_Buffer" (b13): g_CameraParam = (near, far, tan(hfov/2), tan(vfov/2)),
// g_CameraVec = camera position. On alternate frames we rewrite both for the left or right eye,
// using the headset's eye pose (relative to the recentred reference) on top of the game camera.
// See DEVELOPMENT.md and tools/analyze_capture.py for how these were found.
#include "render/camera.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "render/culling.h"
#include "render/frame_share.h"
#include "render/replay.h"
#include "render/upscale.h"
#include <string.h>

StereoShared g_stereo;

namespace {

struct FrameState {
	bool active;     // this frame renders an eye
	int eye;         // 0 left, 1 right
	pure::M4 eyeRel; // eye pose relative to the reference (metres)
	float halfW, halfH;
	float localPos[3], localRot[4];  // eye pose in LOCAL space, for submission
	bool patchedScene, patchedCam;
	bool observe;  // stereo is held off only by TopDownFlat: watch the game camera's pitch, don't patch
	float ipd;  // distance between the two eye positions of the same head pose (metres)
	float refRot[4];  // the reference (recentre) orientation this frame was rendered with
	long long poseBase;  // g_stereo.poseBase of the head pose used
};
FrameState s_frame;
FrameState s_frame2;
// Depth reprojection (0.3.30): what each eye was last rendered with, to move its image to a later
// camera. Ce = eye-to-world, Pe = its projection, E = eye relative to the camera (scaled).
struct Shot {
	bool valid;
	unsigned frameNo;
	pure::M4 Ce, Pe, E;
	float localPos[3], localRot[4], halfW, halfH;
};
Shot s_shot[2];  // Synchronized: the right eye, same head pose, for the replay at Present
// Upscaling (upscale.cpp): each eye's camera this frame [0] and the last frame it was drawn [1], for
// motion vectors. Pe without the sub-pixel jitter, which is kept apart (render pixels).
struct UpCam {
	bool valid;
	unsigned frameNo;
	pure::M4 Ce, Pe;
	float jitter[2];
};
UpCam s_upCam[2][2];
bool s_synchronized;  // this frame: left eye live, right eye replayed
bool s_secondEye;     // the replay is running (s_frame = the right eye)
unsigned s_frameNo;
float s_bbAspect = 16.f / 9.f;
pure::M4 s_lastCam;      // camera used for the last eye (after levelling), camera-to-world
pure::M4 s_lastGameCam;  // the game's own camera, camera-to-world
// Head-direction culling (culling.cpp, logic thread): the eye's pose relative to the game camera,
// written by PatchScene, with the tick it was written at.
SRWLOCK s_relLock = SRWLOCK_INIT;
pure::M4 s_headRel;
DWORD s_headRelTick;
pure::M4 s_viewFix, s_viewFixInv;  // game view -> eye view, and back (this frame)
float s_lastDolly[3];    // world-space offset applied for CameraDolly last frame
bool s_haveCam;
bool s_steepLogged;
// TopDownFlat: the game camera's pitch, once per frame, with hysteresis (about 1 s each way).
unsigned s_pitchFrame;
int s_steepRun, s_levelRun;
bool s_topDownFlat;
float s_lastPitch;

void NotePitch(float pitch) {
	if (s_pitchFrame == s_frameNo) return;
	s_pitchFrame = s_frameNo;
	s_lastPitch = pitch;
	// TopDownFlat: enter at TopDownFlatFrom (50), leave only below TopDownFlatUntil (35). The flight
	// unit's views range from ~40 (near-isometric) to ~68 deg (0.3.15 log), and normal play rarely
	// stays below -50 for a second.
	float a = pitch < 0 ? -pitch : pitch;
	s_steepRun = a >= g_cfg.topDownFrom ? s_steepRun + 1 : 0;
	s_levelRun = a < g_cfg.topDownUntil ? s_levelRun + 1 : 0;
}
// Simple scenes (the title screen: ~90 draws per frame vs 1000+ in gameplay) are shown flat.
volatile LONG s_drawsThisFrame;
int s_lowRun, s_highRun;
bool s_simpleScene;
float s_loggedSep;
volatile LONG s_sepBudget;
bool s_loggedFirstPatch, s_loggedMismatch;

struct Pending {
	ID3D11Resource* res;
	float* data;
	int kind;  // 1 SceneBuffer, 2 CamParam
};
Pending s_pending[8];

bool Near(float a, float b, float rel) {
	float d = a - b;
	if (d < 0) d = -d;
	float m = b < 0 ? -b : b;
	return d <= rel * (m > 1e-6f ? m : 1e-6f);
}

pure::M4 Load(const float* p) {
	pure::M4 m;
	memcpy(m.m, p, 64);
	return m;
}

// First person (prototype): this frame's camera stands at the game camera's look-at target.
bool s_fp;
unsigned s_fpMatched, s_fpNewest, s_fpNone;
DWORD s_fpReportTick;
bool s_fpLogged;

float WorldScale() { return s_fp ? g_cfg.fpWorldScale : g_cfg.worldScale; }

pure::M4 EyeMatrixScaled() {
	pure::M4 e = s_frame.eyeRel;
	float s = WorldScale();
	e.at(0, 3) *= s;
	e.at(1, 3) *= s;
	e.at(2, 3) *= s;
	return e;
}

// First person: the camera at the point the game camera follows (+ offsets), facing the way it faces
// with a level horizon. False if the look-at isn't known (head culling's hook in culling.cpp delivers it).
bool FirstPersonCamera(const pure::M4& C, pure::M4* out) {
	float cam[3] = {C.at(0, 3), C.at(1, 3), C.at(2, 3)}, eye[3], target[3], p[3];
	bool matched = false;
	if (!CullingLookAt(cam, eye, target, &matched) ||
		!pure::first_person_pos(eye, target, g_cfg.fpHeight, g_cfg.fpForward, p)) {
		s_fpNone++;
		return false;
	}
	(matched ? s_fpMatched : s_fpNewest)++;
	*out = pure::m4_level_camera(C);
	for (int k = 0; k < 3; k++) out->at(k, 3) = p[k];
	if (!s_fpLogged) {
		s_fpLogged = true;
		float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
		Log("first person: on; the camera follows a point %.2f units away (%.2f up), eyes at %.2f %.2f %.2f",
			(double)pure::fsqrt(dx * dx + dy * dy + dz * dz), (double)dy, (double)p[0], (double)p[1], (double)p[2]);
	}
	if (GetTickCount() - s_fpReportTick > 30000) {
		s_fpReportTick = GetTickCount();
		Log("first person: last 30 s: %u frames with their own look-at, %u with the newest, %u without", s_fpMatched,
			s_fpNewest, s_fpNone);
		s_fpMatched = s_fpNewest = s_fpNone = 0;
	}
	return true;
}

bool IsMainScene(const float* d) {
	pure::ProjInfo pi;
	if (!pure::classify_projection(d + 16, &pi) || pi.rowMajor || pi.wSign > 0) return false;
	if (!Near(pi.aspect, s_bbAspect, 0.03f)) return false;  // e.g. the 1:1 menu/model cameras
	return pure::m4_is_rigid(Load(d), 1e-2f);
}

bool IsCamParam(const float* d) {
	// tangents up to 200: with CullingFov=170 the game uploads tan(hfov/2) ~ 20 (0.3.20 rejected it)
	return d[0] > 0.001f && d[0] < 10 && d[1] > 100 && d[2] > 0.05f && d[2] < 200 && d[3] > 0.05f && d[3] < 200 &&
		   Near(d[2] / d[3], s_bbAspect, 0.03f);
}

void PatchScene(float* d) {
	pure::M4 V = Load(d), P = Load(d + 16);
	pure::M4 C = pure::m4_rigid_inverse(V);
	s_lastGameCam = C;
	pure::M4 C0 = C;
	if (g_cfg.levelCamera) {
		// Levelled while the game camera looks roughly horizontally; in steep (top-down) views its
		// pitch is kept, so the arena lies in front of you instead of at your feet.
		float pitch = 0, kept = 0;
		C0 = pure::m4_level_camera_adaptive(C, g_cfg.steepFrom, g_cfg.steepFull, &pitch, &kept);
		bool steep = kept > 0.5f;
		if (steep != s_steepLogged) {
			s_steepLogged = steep;
			Log("stereo: game camera pitch %.0f deg (game fov %.0f deg, camera at %.1f %.1f %.1f): %s", (double)pitch,
				(double)CullingGameFov(), (double)C.at(0, 3), (double)C.at(1, 3), (double)C.at(2, 3),
				steep ? "steep (top-down) view, keeping the camera's tilt" : "levelled again");
		}
	}
	if (!s_secondEye) NotePitch(pure::camera_pitch_deg(C));
	pure::M4 fpCam;
	s_fp = g_cfg.firstPerson && FirstPersonCamera(C, &fpCam);
	if (s_fp) {
		// s_lastDolly: the camera's move away from the game's, also for CamParam's position
		for (int k = 0; k < 3; k++) s_lastDolly[k] = fpCam.at(k, 3) - C.at(k, 3);
		C0 = fpCam;
	} else {
		// CameraDolly: move along the game camera's own line of sight (towards the character it
		// follows) so nearby things get real depth without changing the world's scale.
		for (int k = 0; k < 3; k++) {
			s_lastDolly[k] = -C.at(k, 2) * g_cfg.cameraDolly;
			C0.at(k, 3) += s_lastDolly[k];
		}
	}
	s_lastCam = C0;
	s_haveCam = true;
	pure::M4 Ce = pure::m4_mul(C0, EyeMatrixScaled());
	// Diagnostics: the eye separation applied (both eyes from the same head pose, in game units),
	// logged when it changes, i.e. after Ctrl+F9/F10.
	{
		float sep = s_frame.ipd * WorldScale();
		if (sep > 0 && (s_loggedSep <= 0 || sep > s_loggedSep * 1.02f || sep < s_loggedSep / 1.02f) &&
			LogBudget(&s_sepBudget, 60)) {
			s_loggedSep = sep;
			Log("stereo: eye separation in the game world now %.2f cm (world scale %.3f)", (double)(sep * 100),
				(double)WorldScale());
		}
	}
	{
		pure::M4 rel = pure::m4_relative(C, Ce);
		AcquireSRWLockExclusive(&s_relLock);
		s_headRel = rel;
		s_headRelTick = GetTickCount();
		ReleaseSRWLockExclusive(&s_relLock);
	}
	pure::M4 Ve = pure::m4_rigid_inverse(Ce);
	pure::M4 Pe = pure::m4_symmetric_projection(P, s_frame.halfW, s_frame.halfH);
	// Upscaling: shift the image by a different fraction of a pixel each frame (clip-space
	// translation; w = -z, so the offset goes into the z column with a minus)
	float jn[2] = {0, 0}, jp[2] = {0, 0};
	pure::M4 PeJ = Pe;
	if (s_frame.eye >= 0 && s_frame.eye < 2 && UpscaleJitter(s_frame.eye, jn, jp)) {
		PeJ.at(0, 2) -= jn[0];
		PeJ.at(1, 2) -= jn[1];
	}
	if (s_frame.eye >= 0 && s_frame.eye < 2) {
		UpCam* u = s_upCam[s_frame.eye];
		if (u[0].valid && u[0].frameNo != s_frameNo) u[1] = u[0];
		u[0].valid = true;
		u[0].frameNo = s_frameNo;
		u[0].Ce = Ce;
		u[0].Pe = Pe;
		u[0].jitter[0] = jp[0];
		u[0].jitter[1] = jp[1];
	}
	pure::M4 VPe = pure::m4_mul(PeJ, Ve);
	CullingObserveGameFov(2 * pure::fatan(1 / P.at(1, 1)) * 57.29578f);
	s_viewFix = pure::m4_mul(Ve, C);     // game view space -> world -> eye view space
	s_viewFixInv = pure::m4_mul(V, Ce);  // and back
	memcpy(d, Ve.m, 64);
	memcpy(d + 16, PeJ.m, 64);
	memcpy(d + 32, VPe.m, 64);
	memcpy(d + 48, Ce.m, 64);
	s_frame.patchedScene = true;
	if (s_frame.eye >= 0 && s_frame.eye < 2 && !s_secondEye) {
		Shot& s = s_shot[s_frame.eye];
		s.valid = true;
		s.frameNo = s_frameNo;
		s.Ce = Ce;
		s.Pe = Pe;
		s.E = EyeMatrixScaled();
		memcpy(s.localPos, s_frame.localPos, 12);
		memcpy(s.localRot, s_frame.localRot, 16);
		s.halfW = s_frame.halfW;
		s.halfH = s_frame.halfH;
	}
	if (!s_loggedFirstPatch) {
		s_loggedFirstPatch = true;
		Log("stereo: first camera patch. game camera at (%.2f, %.2f, %.2f), game vfov %.1f deg; eye frustum "
			"tan %.3f x %.3f (%.1f x %.1f deg%s), world scale %.2f, level camera %d",
			(double)C.at(0, 3), (double)C.at(1, 3), (double)C.at(2, 3),
			(double)(2 * pure::fatan(1 / P.at(1, 1)) * 57.29578f), (double)s_frame.halfW, (double)s_frame.halfH,
			(double)(2 * pure::fatan(s_frame.halfW) * 57.29578f), (double)(2 * pure::fatan(s_frame.halfH) * 57.29578f),
			g_cfg.anamorphic ? ", anamorphic" : "", (double)g_cfg.worldScale, g_cfg.levelCamera);
		// for depth-based reprojection: how the game maps view depth to the depth buffer
		Log("stereo: scene projection depth terms: P22 %.6f P23 %.6f P32 %.3f P33 %.3f (column-vector layout)",
			(double)P.at(2, 2), (double)P.at(2, 3), (double)P.at(3, 2), (double)P.at(3, 3));
	}
}

void PatchCamParam(float* d) {
	d[2] = s_frame.halfW;
	d[3] = s_frame.halfH;
	if (s_haveCam) {
		// Uploaded before this frame's SceneBuffer: use last frame's orientation with this
		// frame's position (the difference is far below a pixel).
		pure::M4 E = EyeMatrixScaled();
		float t[3] = {E.at(0, 3), E.at(1, 3), E.at(2, 3)};
		for (int k = 0; k < 3; k++)
			d[4 + k] += s_lastCam.at(k, 0) * t[0] + s_lastCam.at(k, 1) * t[1] + s_lastCam.at(k, 2) * t[2] +
						s_lastDolly[k];
	}
	s_frame.patchedCam = true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Game thread API
// ---------------------------------------------------------------------------------------------
void StereoSetAspect(float aspect) {
	if (aspect > 0.1f && aspect < 10) s_bbAspect = aspect;
}

// Called right after the game's Present: decides what the next frame renders.
void StereoCountDraw() { s_drawsThisFrame++; }

// The eye's pose and frustum for this frame (g_stereo.lock held, shared).
static void FillEye(FrameState& f, int which) {
	f.eye = which;
	pure::M4 ref = pure::m4_from_pose({g_stereo.refRot[0], g_stereo.refRot[1], g_stereo.refRot[2], g_stereo.refRot[3]},
									  {g_stereo.refPos[0], g_stereo.refPos[1], g_stereo.refPos[2]});
	const float* er = g_stereo.eyeRot[f.eye];
	const float* ep = g_stereo.eyePos[f.eye];
	pure::M4 eye = pure::m4_from_pose({er[0], er[1], er[2], er[3]}, {ep[0], ep[1], ep[2]});
	f.eyeRel = pure::m4_relative(ref, eye);
	memcpy(f.refRot, g_stereo.refRot, 16);
	{
		const float* p0 = g_stereo.eyePos[0];
		const float* p1 = g_stereo.eyePos[1];
		float dx = p0[0] - p1[0], dy = p0[1] - p1[1], dz = p0[2] - p1[2];
		f.ipd = pure::fsqrt(dx * dx + dy * dy + dz * dz);
	}
	memcpy(f.localPos, ep, 12);
	memcpy(f.localRot, er, 16);
	f.poseBase = g_stereo.poseBase;
	const float* t = g_stereo.eyeTan[f.eye];  // tan of left, right, up, down
	if (g_cfg.anamorphic) {
		// Fill the eye's own field of view in both directions: the 16:9 image is stretched to
		// the eye's shape (non-square pixels) instead of wasting width. CamParam stores the
		// horizontal and vertical tangents separately, so depth-based effects stay correct.
		f.halfW = -t[0] > t[1] ? -t[0] : t[1];
		f.halfH = t[2] > -t[3] ? t[2] : -t[3];
		if (g_cfg.verticalFill < 1) {  // optional: crop the vertical angle for more detail
			float vf = g_cfg.verticalFill < 0.5f ? 0.5f : g_cfg.verticalFill;
			f.halfH = pure::ftan(pure::fatan(f.halfH) * vf);
		}
	} else {
		pure::cover_fov(t[0], t[1], t[2], t[3], s_bbAspect, &f.halfW, &f.halfH);
		// VerticalFill < 1 trades vertical field of view for sharpness: at 0 the frustum just
		// covers the eye's width, cropping the top and bottom of the view.
		float wide = -t[0] > t[1] ? -t[0] : t[1];
		if (f.halfW > wide && g_cfg.verticalFill < 1) {
			float hw = wide + (f.halfW - wide) * (g_cfg.verticalFill < 0 ? 0 : g_cfg.verticalFill);
			f.halfH *= hw / f.halfW;
			f.halfW = hw;
		}
	}
	f.halfW *= g_cfg.fovScale;
	f.halfH *= g_cfg.fovScale;
}

void StereoBeginFrame() {
	s_frameNo++;
	{
		LONG draws = s_drawsThisFrame;
		s_drawsThisFrame = 0;
		bool low = g_cfg.flatBelowDraws > 0 && draws < g_cfg.flatBelowDraws;
		s_lowRun = low ? s_lowRun + 1 : 0;
		s_highRun = low ? 0 : s_highRun + 1;
		if (!s_simpleScene && s_lowRun >= 20) {
			s_simpleScene = true;
			Log("stereo: simple scene (%d draws per frame, e.g. the title screen): shown flat", (int)draws);
		} else if (s_simpleScene && s_highRun >= 20) {
			s_simpleScene = false;
			Log("stereo: full scene again (%d draws per frame): stereo", (int)draws);
		}
	}
	FrameState f = {};
	AcquireSRWLockShared(&g_stereo.lock);
	bool on = g_stereo.enabled && g_stereo.valid && g_stereo.refValid && !g_stereo.menuMode && !s_simpleScene;
	// TopDownFlat: steep top-down views (e.g. the flight unit) go to the flat screen about 1 s after
	// the camera tilts down TopDownFlatFrom or more, and back about 2 s after it is below
	// TopDownFlatUntil.
	bool wasTop = s_topDownFlat;
	if (!g_cfg.topDownFlat) s_topDownFlat = false;
	else if (!s_topDownFlat && s_steepRun >= 60) s_topDownFlat = true;
	else if (s_topDownFlat && s_levelRun >= 120) s_topDownFlat = false;
	if (s_topDownFlat != wasTop)
		Log("stereo: %s (camera pitch %.0f deg)", s_topDownFlat ? "top-down view: flat screen (TopDownFlat=1)" : "top-down view ended: stereo",
			(double)s_lastPitch);
	f.observe = on && s_topDownFlat;
	on = on && !s_topDownFlat;
	if (on) {
		bool synchronized = g_cfg.synchronized && ReplayAvailable();
		FillEye(f, synchronized ? 0 : (int)(s_frameNo & 1));
		s_synchronized = synchronized;
		if (synchronized) {
			s_frame2 = f;
			FillEye(s_frame2, 1);
		}
	}
	ReleaseSRWLockShared(&g_stereo.lock);
	f.active = on;
	s_frame = f;
	if (!on) s_synchronized = false;
	s_frame2.active = on;
	s_frame2.observe = false;
	s_secondEye = false;
	CullingUpdate(on);
	for (auto& p : s_pending) p.res = nullptr;
}

// Describes the frame that is being presented now.
void StereoGetFrameTag(FrameTag* tag) {
	tag->eye = s_frame.active ? s_frame.eye : -1;
	tag->hasScene = false;
	tag->patched = s_frame.active && s_frame.patchedScene;
	memcpy(tag->pos, s_frame.localPos, 12);
	memcpy(tag->rot, s_frame.localRot, 16);
	tag->tanHalfW = s_frame.halfW;
	tag->tanHalfH = s_frame.halfH;
	tag->poseBase = s_frame.active ? s_frame.poseBase : 0;
	tag->hasWorld = false;
	if (s_frame.active && s_frame.patchedScene && s_haveCam) {
		// eye world orientation = C0 * eyeRel = C0 * ref^-1 * eye: LOCAL -> world is C0 * ref^-1
		const float* r = s_frame.refRot;
		pure::M4 ref = pure::m4_from_pose({r[0], r[1], r[2], r[3]}, {0, 0, 0});
		pure::M4 c = s_lastCam;
		c.at(0, 3) = c.at(1, 3) = c.at(2, 3) = 0;
		pure::Q q = pure::q_from_m4(pure::m4_mul(c, pure::m4_rigid_inverse(ref)));
		tag->worldRot[0] = q.x;
		tag->worldRot[1] = q.y;
		tag->worldRot[2] = q.z;
		tag->worldRot[3] = q.w;
		tag->hasWorld = true;
	}
	if (s_frame.active && !s_frame.patchedScene && !s_loggedMismatch && s_frameNo > 600) {
		s_loggedMismatch = true;
		Log("stereo: a frame finished without the main camera being found (menu/loading screen, or the game "
			"changed how it uploads the camera)");
	}
}

bool StereoActive() { return s_frame.active; }
bool StereoSynchronizedFrame() { return s_synchronized && s_frame.active; }
void StereoBeginSecondEye() {
	FrameState f = s_frame2;
	f.patchedScene = f.patchedCam = false;
	s_frame = f;
	s_secondEye = true;
	for (auto& p : s_pending) p.res = nullptr;
}
// Depth reprojection: the other eye's image from the previous frame, moved to this frame's camera.
// T = that eye's view as it would be now (same head pose relative to the camera) <- its view then;
// proj = its projection's x/y scale and depth terms. Returns false when there is no such image.
bool StereoReprojectParams(int eye, float T[16], float proj[4], FrameTag* tag) {
	if (eye < 0 || eye > 1 || !s_frame.active || !s_frame.patchedScene || s_frame.eye == eye || !s_haveCam) return false;
	const Shot& s = s_shot[eye];
	if (!s.valid || s.frameNo + 1 != s_frameNo) return false;
	pure::M4 now = pure::m4_mul(s_lastCam, s.E);
	pure::M4 t = pure::m4_mul(pure::m4_rigid_inverse(now), s.Ce);
	memcpy(T, t.m, 64);
	proj[0] = s.Pe.at(0, 0);
	proj[1] = s.Pe.at(1, 1);
	proj[2] = s.Pe.at(2, 2);
	proj[3] = s.Pe.at(2, 3);
	*tag = {};
	tag->eye = eye;
	tag->patched = true;
	tag->hasScene = true;
	tag->noUI = true;
	memcpy(tag->pos, s.localPos, 12);
	memcpy(tag->rot, s.localRot, 16);
	tag->tanHalfW = s.halfW;
	tag->tanHalfH = s.halfH;
	return true;
}
int StereoFrameEye() { return s_frame.active ? s_frame.eye : -1; }

// Upscaling: this frame's camera of `eye` and how it moved since that eye's previous frame.
// T = previous eye view <- this eye view; cur/prev = x scale, y scale, P22, P23 of the projections
// (without jitter); jitter = this frame's offset in render pixels. False if the camera of `eye`
// wasn't found this frame. *hasPrev: there is a previous frame close enough to compare with.
bool StereoUpscaleCams(int eye, float T[16], float cur[4], float prev[4], float jitter[2], bool* hasPrev) {
	if (eye < 0 || eye > 1) return false;
	const UpCam& c = s_upCam[eye][0];
	const UpCam& p = s_upCam[eye][1];
	if (!c.valid || c.frameNo != s_frameNo) return false;
	*hasPrev = p.valid && c.frameNo - p.frameNo <= 3;  // alternating mode: 2 frames apart
	pure::M4 t = pure::m4_mul(pure::m4_rigid_inverse(p.Ce), c.Ce);
	memcpy(T, t.m, 64);
	const pure::M4* m[2] = {&c.Pe, &p.Pe};
	float* o[2] = {cur, prev};
	for (int k = 0; k < 2; k++) {
		o[k][0] = m[k]->at(0, 0);
		o[k][1] = m[k]->at(1, 1);
		o[k][2] = m[k]->at(2, 2);
		o[k][3] = m[k]->at(2, 3);
	}
	jitter[0] = c.jitter[0];
	jitter[1] = c.jitter[1];
	return true;
}

bool StereoSecondEyePatched() { return s_secondEye && s_frame.patchedScene; }
// The game's own camera (camera-to-world, before levelling): position, forward, up.
bool StereoGameCamera(float pos[3], float fwd[3], float up[3]) {
	if (!s_haveCam) return false;
	for (int k = 0; k < 3; k++) {
		pos[k] = s_lastGameCam.at(k, 3);
		fwd[k] = -s_lastGameCam.at(k, 2);
		up[k] = s_lastGameCam.at(k, 1);
	}
	return true;
}
// The eye's orientation relative to the game camera, if an eye was rendered in the last 250 ms.
bool StereoHeadRelative(pure::M4* rel) {
	AcquireSRWLockShared(&s_relLock);
	bool ok = s_headRelTick && GetTickCount() - s_headRelTick < 250;
	if (ok) *rel = s_headRel;
	ReleaseSRWLockShared(&s_relLock);
	return ok && s_frame.active;
}
// Head culling (0.3.24): vertical fov (radians) of a culling view, centred on the eye's direction,
// that holds the eye's whole view plus marginDeg on every side (game frames lag the head a little).
// 0 if no eye has been rendered yet.
float StereoHeadCullFov(float marginDeg) {
	float w = s_frame.halfW, h = s_frame.halfH;
	if (!(w > 0.05f && h > 0.05f && s_bbAspect > 0.1f)) return 0;
	float v = h > w / s_bbAspect ? h : w / s_bbAspect;  // the frustum has the game's aspect
	float half = pure::fatan(v) + marginDeg * 0.0174533f;
	return 2 * half;
}
// The camera buffers need looking at: an eye is rendered, or TopDownFlat waits for the view to level.
bool StereoWatching() { return s_frame.active || s_frame.observe; }

bool StereoGetViewFix(float T[16], float Tinv[16]) {
	if (!s_frame.active || !s_frame.patchedScene) return false;
	memcpy(T, s_viewFix.m, 64);
	memcpy(Tinv, s_viewFixInv.m, 64);
	return true;
}

// From the D3D11 hooks (game's immediate context only).
// TopDownFlat: the frame renders mono, but the game camera is still watched to know when the
// steep view ends.
void ObserveScene(const float* d) {
	if (IsMainScene(d)) NotePitch(pure::camera_pitch_deg(pure::m4_rigid_inverse(Load(d))));
}

void StereoOnMap(ID3D11Resource* res, void* data, UINT size) {
	if (!(s_frame.active || s_frame.observe) || !data || (size != 256 && size != 32)) return;
	for (auto& p : s_pending)
		if (!p.res) {
			p = {res, (float*)data, size == 256 ? 1 : 2};
			return;
		}
}

void StereoBeforeUnmap(ID3D11Resource* res) {
	if (!(s_frame.active || s_frame.observe)) return;
	for (auto& p : s_pending) {
		if (p.res != res) continue;
		// The mapped memory may be write-combined: read it once into a local copy.
		float local[64];
		UINT n = p.kind == 1 ? 64 : 8;
		memcpy(local, p.data, n * 4);
		if (!s_frame.active) {
			if (p.kind == 1) ObserveScene(local);
		} else if (p.kind == 1 && IsMainScene(local)) {
			PatchScene(local);
			memcpy(p.data, local, n * 4);
		} else if (p.kind == 2 && IsCamParam(local)) {
			PatchCamParam(local);
			memcpy(p.data, local, n * 4);
		}
		p.res = nullptr;
		return;
	}
}

// UpdateSubresource path: returns the data to pass on (possibly a patched copy in `scratch`).
const void* StereoPatchUpdate(UINT size, const void* data, float* scratch /* 64 floats */) {
	if (s_frame.observe && data && size == 256) ObserveScene((const float*)data);
	if (!s_frame.active || !data || (size != 256 && size != 32)) return data;
	memcpy(scratch, data, size);
	if (size == 256 && IsMainScene(scratch)) {
		PatchScene(scratch);
		return scratch;
	}
	if (size == 32 && IsCamParam(scratch)) {
		PatchCamParam(scratch);
		return scratch;
	}
	return data;
}
