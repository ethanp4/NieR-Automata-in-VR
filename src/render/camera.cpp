// camera.cpp - the game camera rewritten for each eye, in both stereo modes.
#include "render/camera.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "render/culling.h"
#include "render/foveated.h"
#include "game/first_person.h"
#include "game/player.h"
#include "input/gamepad.h"
#include "render/frame_share.h"
#include "render/replay.h"
#include "render/upscale.h"
#include <atomic>
#include <math.h>
#include <string.h>

StereoShared g_stereo;

namespace {

struct FrameState {
	bool active;      // this frame renders an eye
	int32_t eye;      // 0 left, 1 right
	pure::M4 eyeRel;  // eye pose relative to the reference (metres)
	bool handValid[2];
	pure::M4 handRel[2];  // the controllers' grip poses relative to the reference (metres)
	float halfW, halfH;
	float localPos[3], localRot[4];  // eye pose in LOCAL space, for submission
	bool patchedScene, patchedCam;
	bool observe;      // stereo is held off only by TopDownFlat: watch the game camera's pitch, don't patch
	float ipd;         // distance between the two eye positions of the same head pose (metres)
	float refRot[4];   // the reference (recentre) orientation this frame was rendered with
	int64_t poseBase;  // g_stereo.poseBase of the head pose used
};
FrameState s_frame;
FrameState s_rightEyeFrame;
// Depth reprojection: what each eye was last rendered with, to move its image to a later
// camera. Ce = eye-to-world, Pe = its projection, E = eye relative to the camera (scaled).
struct EyeShot {
	bool valid;
	uint32_t frameNo;
	pure::M4 Ce, Pe, E;
	float localPos[3], localRot[4], halfW, halfH;
};
EyeShot s_lastShot[2];  // Synchronized: the right eye, same head pose, for the replay at Present
// Upscaling (upscale.cpp): each eye's camera this frame [0] and the last frame it was drawn [1], for
// motion vectors. Pe without the sub-pixel jitter, which is kept apart (render pixels).
struct UpscaleCamera {
	bool valid;
	uint32_t frameNo;
	pure::M4 Ce, Pe;
	float jitter[2];
};
UpscaleCamera s_upscaleCams[2][2];
bool s_synchronized;  // this frame: left eye live, right eye replayed
bool s_secondEye;     // the replay is running (s_frame = the right eye)
uint32_t s_frameNo;
float s_backbufferAspect = 16.f / 9.f;
constexpr DWORD kHeadsetGoneMs = 500;  // no headset frame shown for this long: flat
bool s_headsetShown;
float GameCameraAspect() { return s_backbufferAspect > kGameMinAspect ? s_backbufferAspect : kGameMinAspect; }
pure::M4 s_lastCam;      // camera used for the last eye (after levelling), camera-to-world
pure::M4 s_lastGameCam;  // the game's own camera, camera-to-world
// Head-direction culling (culling.cpp, logic thread): the eye's pose relative to the game camera,
// written by PatchScene, with the tick it was written at.
SRWLOCK s_headRelLock = SRWLOCK_INIT;
pure::M4 s_headRel;
DWORD s_headRelTick;
pure::M4 s_viewFix, s_viewFixInv;  // game view -> eye view, and back (this frame)
float s_lastCameraOffset[3];       // world-space offset applied for CameraDolly last frame
bool s_haveCamera;
bool s_loggedSteep;
// TopDownFlat: the game camera's pitch, once per frame, with hysteresis (about 1 s each way).
uint32_t s_pitchFrame;
int32_t s_steepFrames, s_levelFrames;
bool s_topDownFlat;
float s_lastPitch;

void NotePitch(float pitch) {
	if (s_pitchFrame == s_frameNo) return;
	s_pitchFrame = s_frameNo;
	s_lastPitch = pitch;
	// TopDownFlat: enter at TopDownFlatFrom (50), leave only below TopDownFlatUntil (35). The flight
	// unit's views range from ~40 (near-isometric) to ~68 deg, and normal play rarely
	// stays below -50 for a second.
	float steepness = pitch < 0 ? -pitch : pitch;
	s_steepFrames = steepness >= g_cfg.topDownFrom ? s_steepFrames + 1 : 0;
	s_levelFrames = steepness < g_cfg.topDownUntil ? s_levelFrames + 1 : 0;
}
// Simple scenes (the title screen: ~90 draws per frame vs 1000+ in gameplay) are shown flat.
std::atomic<int32_t> s_drawsThisFrame;
int32_t s_simpleFrames, s_fullFrames;
bool s_simpleScene;
float s_loggedSeparation;
std::atomic<int32_t> s_separationLogs;
bool s_loggedFirstPatch, s_loggedMismatch;

enum BufferKind {
	SCENE_BUFFER = 1,      // SceneBuffer, 256 bytes
	CAM_PARAM_BUFFER = 2,  // CamParam_HPixel_Buffer, 32 bytes
};
// Camera buffers mapped this frame, patched at Unmap
struct PendingMap {
	ID3D11Resource* res;
	float* data;
	BufferKind kind;
};
PendingMap s_pendingMaps[8];

bool NearlyEqual(float a, float b, float rel) {
	float diff = a - b;
	if (diff < 0) diff = -diff;
	float size = b < 0 ? -b : b;
	return diff <= rel * (size > 1e-6f ? size : 1e-6f);
}

pure::M4 LoadMatrix(const float* floats) {
	pure::M4 matrix;
	memcpy(matrix.m, floats, 64);
	return matrix;
}

// First person: this frame's camera is at the character's eyes (first_person.cpp), or else at the point
// the game camera follows.
bool s_firstPerson;
bool s_fpOnHead;  // ... at their eyes
float s_fpSteadyLift;  // how far steady height moved this camera from their eyes (up)
// Sideways the camera follows their body (their object's position), not their head bone, which flips and
// sways with animations (a backflip in the double jump): their eyes' place from it is followed slowly.
constexpr float kEyeDriftSeconds = 1.0f;  // time constant of following their eyes' place from their body
constexpr float kMaxEyeDrift = 0.3f;      // metres: their eyes are taken at most this far from their body
constexpr double kDriftGapMs = 500;       // a gap this long starts over at their eyes
struct EyeDrift {
	bool valid;
	pure::V3 offset;  // their eyes from their body (world, sideways)
	double ms;
} s_eyeDrift;

// Their eyes, sideways at their body plus the slowly followed offset (unchanged if their body isn't found).
pure::V3 SteadyEyePlace(pure::V3 eye) {
	pure::V3 body;
	if (!g_cfg.steadyEyes || !PlayerPosition(&body)) {
		s_eyeDrift.valid = false;
		return eye;
	}
	pure::V3 now = {eye.x - body.x, 0, eye.z - body.z};
	double ms = NowMs();
	if (!s_eyeDrift.valid || ms - s_eyeDrift.ms > kDriftGapMs) s_eyeDrift.offset = now;
	else s_eyeDrift.offset = s_eyeDrift.offset + (now - s_eyeDrift.offset) * (1 - expf(-(float)(ms - s_eyeDrift.ms) / 1000 / kEyeDriftSeconds));
	s_eyeDrift.valid = true;
	s_eyeDrift.ms = ms;
	float drift = pure::length(s_eyeDrift.offset);
	if (drift > kMaxEyeDrift) s_eyeDrift.offset = s_eyeDrift.offset * (kMaxEyeDrift / drift);
	return {body.x + s_eyeDrift.offset.x, eye.y, body.z + s_eyeDrift.offset.z};
}
uint32_t s_fpHead, s_fpOwnLookAt, s_fpNewestLookAt, s_fpNoLookAt;
DWORD s_fpReportTick;
bool s_loggedFirstPerson, s_loggedHeadCamera;
float WorldScale() { return s_firstPerson ? 1.0f : g_cfg.worldScale; }  // first person is life size

// First person tracking (FirstPersonTracking): your real steps move the camera, and the character
// follows it (game/player.cpp). The camera's sideways place from their eyes is `offset` instead of your
// head's place in the room; your height still counts. Where the game stops them (a wall), the camera
// stays ahead: you step out of them, and back in by walking back.
constexpr float kRecentredJump = 0.3f;    // your head moving this far in one frame was recentred, not a step
constexpr float kFollowStep = 0.05f;      // the character moves at most this far towards the camera per frame
constexpr float kAwayFrom = 0.3f;         // the camera this far from their eyes: out of them (head shown, not posed)
constexpr float kBackWithin = 0.2f;       // ... and back in within this
constexpr DWORD kStickMs = 200;           // the stick moved them this recently: their own movement carries the camera
constexpr float kOwnMove = 0.01f;         // they moved this much more than asked in a frame: the game moved them too (attack, dodge)
constexpr float kBlockedShare = 0.5f;     // they got less than this share of the way asked, or slid this much of it sideways: a wall
constexpr float kStillBlocked = 0.5f;     // a wall stops them until the camera's way from them turns away from it (cos)
struct Walking {
	bool active;
	pure::V3 lastHead;  // your head in the room (scaled like the eyes) last frame
	pure::V3 offset;    // the camera's sideways place from their eyes (camera axes)
	pure::V3 shift;     // this frame: added to the eyes' place (offset less your head's place)
	bool moving;        // asked them to move last frame: from `from` by `asked` (world)
	pure::V3 from, asked;
	bool away;          // the camera is out of them
	bool blocked;       // a wall stopped them: not moved towards `wall` (world, unit) any more
	pure::V3 wall;
} s_walk;
bool s_loggedWalking, s_loggedBlocked;

pure::M4 EyeMatrixScaled() {
	pure::M4 eye = s_frame.eyeRel;
	float scale = WorldScale();
	eye.at(0, 3) *= scale;
	eye.at(1, 3) *= scale;
	eye.at(2, 3) *= scale;
	if (s_walk.active) {
		eye.at(0, 3) += s_walk.shift.x;
		eye.at(2, 3) += s_walk.shift.z;
	}
	return eye;
}

// A controller relative to the camera, scaled and shifted like the eyes (false: not tracked).
bool HandMatrixScaled(int hand, pure::M4* out) {
	if (!s_frame.handValid[hand]) return false;
	*out = s_frame.handRel[hand];
	float scale = WorldScale();
	for (int k = 0; k < 3; k++) out->at(k, 3) *= scale;
	if (s_walk.active) {
		out->at(0, 3) += s_walk.shift.x;
		out->at(2, 3) += s_walk.shift.z;
	}
	return true;
}

// Your head's centre in the room (scaled like the eyes): this eye moved half the eye distance towards the other.
pure::V3 HeadInRoom() {
	const pure::M4& eye = s_frame.eyeRel;
	float scale = WorldScale(), toCentre = (s_frame.eye == 0 ? 0.5f : -0.5f) * s_frame.ipd;  // the left eye is left of it
	return {(eye.at(0, 3) + eye.at(0, 0) * toCentre) * scale, (eye.at(1, 3) + eye.at(1, 0) * toCentre) * scale,
			(eye.at(2, 3) + eye.at(2, 0) * toCentre) * scale};
}

// Once per game frame, the first person camera `cam` (camera-to-world, level) known: your head's
// sideways movement since last frame moves the camera, and the character is moved towards it.
void UpdateWalking(const pure::M4& cam) {
	pure::V3 head = HeadInRoom();
	if (!g_cfg.fpTracking || !s_firstPerson || !s_fpOnHead) {
		s_walk.active = false;
		s_walk.away = false;
		return;
	}
	if (!s_walk.active) s_walk = {true, head};  // starting: the camera on their eyes
	pure::V3 step = {head.x - s_walk.lastHead.x, 0, head.z - s_walk.lastHead.z};
	s_walk.lastHead = head;
	if (pure::length(step) < kRecentredJump) s_walk.offset = s_walk.offset + step;
	else s_walk.offset = {};  // recentred: back in them
	pure::V3 right = {cam.at(0, 0), 0, cam.at(2, 0)}, back = {cam.at(0, 2), 0, cam.at(2, 2)};
	// Where they got to from last frame's move: the game may have stopped them, or slid them along a wall,
	// and all of that stays with them (the camera stays with your head). When the game moved them too (the
	// stick, an attack, a dodge), that carries the camera: only what went the asked way counts.
	pure::V3 now;
	if (s_walk.moving && PlayerPosition(&now)) {
		pure::V3 moved = {now.x - s_walk.from.x, 0, now.z - s_walk.from.z};
		float asked = pure::length(s_walk.asked);
		pure::V3 direction = s_walk.asked * (1 / asked);
		float walked = pure::dot(moved, direction);
		if (walked < 0) walked = 0;
		if (walked > asked) walked = asked;
		pure::V3 forward = direction * walked;
		if (GetTickCount() - GamepadMoveTick() < kStickMs || pure::length(moved) > asked + kOwnMove) {
			moved = forward;  // the game moved them too: that carries the camera
		} else if (walked < asked * kBlockedShare || pure::length(moved - forward) > asked * kBlockedShare) {
			// a wall stopped them or slid them along it: back to where they got to the asked way, and no more
			// moving towards it
			pure::V3 undo = forward - moved, at;
			PlayerMove(undo, &at);
			moved = forward;
			if (!s_loggedBlocked) {
				s_loggedBlocked = true;
				Log("first person: a wall stops the character; your steps take you out of them");
			}
			s_walk.blocked = true;
			s_walk.wall = direction;
		}
		s_walk.offset = s_walk.offset - pure::V3{pure::dot(moved, right), 0, pure::dot(moved, back)};
	}
	s_walk.moving = false;
	float behind = pure::length(s_walk.offset);
	pure::V3 towardsCamera = right * s_walk.offset.x + back * s_walk.offset.z;
	if (s_walk.blocked && (behind < 0.001f || pure::dot(towardsCamera, s_walk.wall) < behind * kStillBlocked)) s_walk.blocked = false;
	if (behind > 0.001f && !s_walk.blocked) {
		pure::V3 move = behind > kFollowStep ? s_walk.offset * (kFollowStep / behind) : s_walk.offset;
		pure::V3 world = right * move.x + back * move.z;
		if (PlayerMove(world, &s_walk.from)) {
			s_walk.moving = true;
			s_walk.asked = world;
			if (!s_loggedWalking) {
				s_loggedWalking = true;
				Log("first person: your real steps move the character");
			}
		}
	}
	behind = pure::length(s_walk.offset);
	bool away = s_walk.away ? behind > kBackWithin : behind > kAwayFrom;
	if (away != s_walk.away) Log("first person: %s the character (%.2f m from their eyes)", away ? "stepped out of" : "back in", (double)behind);
	s_walk.away = away;
	s_walk.shift = {s_walk.offset.x - head.x, 0, s_walk.offset.z - head.z};
}

// First person at the character's eyes, level, turning with the game camera (the right stick); your
// headset turns you on top. With SteadyEyes, running bob doesn't move you up and down (jumps still do),
// and their animations don't move you sideways (SteadyEyePlace).
bool HeadCamera(const pure::HeadPose& head, const pure::M4& C, pure::M4* out) {
	pure::V3 eye = SteadyEyePlace(head.eye);
	float steadyY;
	if (g_cfg.steadyEyes && FirstPersonSteadyEyeHeight(&steadyY)) eye.y = steadyY;
	s_fpSteadyLift = eye.y - head.eye.y;
	*out = pure::m4_level_camera(C);
	out->at(0, 3) = eye.x;
	out->at(1, 3) = eye.y;
	out->at(2, 3) = eye.z;
	if (!s_loggedHeadCamera) {
		s_loggedHeadCamera = true;
		Log("first person: camera at the character's eyes");
	}
	return true;
}

// First person: at the character's eyes when their head is known, else at the point the game camera
// follows, facing the way it faces with a level horizon. False if neither is known (head culling's hook
// in culling.cpp delivers the look-at).
bool FirstPersonCamera(const pure::M4& C, pure::M4* out) {
	if (GetTickCount() - s_fpReportTick > 30000) {
		s_fpReportTick = GetTickCount();
		Log("first person: last 30 s: %u frames at the character's eyes; %u at the camera's look-at (%u with their own, %u the "
			"newest), %u without",
			s_fpHead, s_fpOwnLookAt + s_fpNewestLookAt, s_fpOwnLookAt, s_fpNewestLookAt, s_fpNoLookAt);
		s_fpHead = s_fpOwnLookAt = s_fpNewestLookAt = s_fpNoLookAt = 0;
	}
	pure::HeadPose head;
	s_fpOnHead = FirstPersonHead(&head) && HeadCamera(head, C, out);
	if (s_fpOnHead) {
		s_fpHead++;
		return true;
	}
	float cam[3] = {C.at(0, 3), C.at(1, 3), C.at(2, 3)}, eye[3], target[3];
	bool matched = false;
	if (!CullingLookAt(cam, eye, target, &matched)) {
		s_fpNoLookAt++;
		return false;
	}
	(matched ? s_fpOwnLookAt : s_fpNewestLookAt)++;
	*out = pure::m4_level_camera(C);
	for (int k = 0; k < 3; k++) out->at(k, 3) = target[k];
	if (!s_loggedFirstPerson) {
		s_loggedFirstPerson = true;
		float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
		Log("first person: the character's bones not found (yet); the camera follows a point %.2f units away (%.2f up), "
			"eyes at %.2f %.2f %.2f",
			(double)pure::fsqrt(dx * dx + dy * dy + dz * dz), (double)dy, (double)target[0], (double)target[1], (double)target[2]);
	}
	return true;
}

bool IsMainScene(const float* data) {
	pure::ProjInfo proj;
	if (!pure::classify_projection(data + 16, &proj) || proj.rowMajor || proj.wSign > 0) return false;
	if (!NearlyEqual(proj.aspect, GameCameraAspect(), 0.03f)) return false;  // e.g. the 1:1 menu/model cameras
	return pure::m4_is_rigid(LoadMatrix(data), 1e-2f);
}

bool IsCamParam(const float* data) {
	// tangents up to 200: with CullingFov=170 the game uploads tan(hfov/2) ~ 20
	return data[0] > 0.001f && data[0] < 10 && data[1] > 100 && data[2] > 0.05f && data[2] < 200 && data[3] > 0.05f && data[3] < 200 &&
		   NearlyEqual(data[2] / data[3], GameCameraAspect(), 0.03f);
}

void PatchScene(float* data) {
	pure::M4 V = LoadMatrix(data), P = LoadMatrix(data + 16);
	pure::M4 C = pure::m4_rigid_inverse(V);
	s_lastGameCam = C;
	pure::M4 C0 = C;
	if (g_cfg.levelCamera) {
		// Levelled while the game camera looks roughly horizontally; in steep (top-down) views its
		// pitch is kept, so the arena lies in front of you instead of at your feet.
		float pitch = 0, kept = 0;
		C0 = pure::m4_level_camera_adaptive(C, g_cfg.steepFrom, g_cfg.steepFull, &pitch, &kept);
		bool steep = kept > 0.5f;
		if (steep != s_loggedSteep) {
			s_loggedSteep = steep;
			Log("stereo: game camera pitch %.0f deg (game fov %.0f deg, camera at %.1f %.1f %.1f): %s", (double)pitch,
				(double)CullingGameFov(), (double)C.at(0, 3), (double)C.at(1, 3), (double)C.at(2, 3),
				steep ? "steep (top-down) view, keeping the camera's tilt" : "levelled again");
		}
	}
	if (!s_secondEye) NotePitch(pure::camera_pitch_deg(C));
	pure::M4 fpCam = {};
	s_firstPerson = g_cfg.firstPerson && FirstPersonCamera(C, &fpCam);
	FirstPersonSetHiding(s_firstPerson && s_fpOnHead && !s_walk.away);  // out of them: their head shown, not posed
	if (s_firstPerson) {
		// s_lastCameraOffset: the camera's move away from the game's, also for CamParam's position
		for (int k = 0; k < 3; k++) s_lastCameraOffset[k] = fpCam.at(k, 3) - C.at(k, 3);
		C0 = fpCam;
	} else {
		// CameraDolly: move along the game camera's own line of sight (towards the character it
		// follows) so nearby things get real depth without changing the world's scale.
		for (int k = 0; k < 3; k++) {
			s_lastCameraOffset[k] = -C.at(k, 2) * g_cfg.cameraDolly;
			C0.at(k, 3) += s_lastCameraOffset[k];
		}
	}
	s_lastCam = C0;
	s_haveCamera = true;
	if (!s_secondEye) UpdateWalking(C0);
	pure::M4 Ce = pure::m4_mul(C0, EyeMatrixScaled());
	if (s_firstPerson && s_fpOnHead && !s_secondEye) {  // your head, between your eyes: the character's body follows it
		pure::M4 head = Ce;
		float toCentre = (s_frame.eye == 0 ? 0.5f : -0.5f) * s_frame.ipd * WorldScale();  // the left eye is left of it
		for (int k = 0; k < 3; k++) head.at(k, 3) += head.at(k, 0) * toCentre;
		// where your head is from theirs: from the camera's own place, less steady height's lift
		pure::V3 offset = {head.at(0, 3) - C0.at(0, 3), head.at(1, 3) - C0.at(1, 3) + s_fpSteadyLift, head.at(2, 3) - C0.at(2, 3)};
		FirstPersonSetViewer(head, C0, offset);
		if (g_cfg.fpTracking) PlayerUpdate({-head.at(0, 2), -head.at(1, 2), -head.at(2, 2)});
		pure::M4 hands[2];
		bool handValid[2];
		for (int hand = 0; hand < 2; hand++) {
			pure::M4 rel;
			handValid[hand] = HandMatrixScaled(hand, &rel);
			if (handValid[hand]) hands[hand] = pure::m4_mul(C0, rel);
		}
		FirstPersonSetHands(handValid, hands);
	}
	// Diagnostics: the eye separation applied (both eyes from the same head pose, in game units),
	// logged when it changes, i.e. after Ctrl+F9/F10.
	{
		float sep = s_frame.ipd * WorldScale();
		if (sep > 0 && (s_loggedSeparation <= 0 || sep > s_loggedSeparation * 1.02f || sep < s_loggedSeparation / 1.02f) &&
			LogBudget(s_separationLogs, 60)) {
			s_loggedSeparation = sep;
			Log("stereo: eye separation in the game world now %.2f cm (world scale %.3f)", (double)(sep * 100),
				(double)WorldScale());
		}
	}
	{
		pure::M4 rel = pure::m4_relative(C, Ce);
		ExclusiveLock guard(&s_headRelLock);
		s_headRel = rel;
		s_headRelTick = GetTickCount();
	}
	pure::M4 Ve = pure::m4_rigid_inverse(Ce);
	pure::M4 Pe = pure::m4_symmetric_projection(P, s_frame.halfW, s_frame.halfH);
	// Upscaling: shift the image by a different fraction of a pixel each frame (clip-space
	// translation; w = -z, so the offset goes into the z column with a minus)
	float jitterClip[2] = {0, 0}, jitterPixels[2] = {0, 0};
	pure::M4 PeJ = Pe;
	if (s_frame.eye >= 0 && s_frame.eye < 2 && UpscaleJitter(s_frame.eye, jitterClip, jitterPixels)) {
		PeJ.at(0, 2) -= jitterClip[0];
		PeJ.at(1, 2) -= jitterClip[1];
	}
	if (s_frame.eye >= 0 && s_frame.eye < 2) {
		UpscaleCamera* cams = s_upscaleCams[s_frame.eye];
		if (cams[0].valid && cams[0].frameNo != s_frameNo) cams[1] = cams[0];
		cams[0].valid = true;
		cams[0].frameNo = s_frameNo;
		cams[0].Ce = Ce;
		cams[0].Pe = Pe;
		cams[0].jitter[0] = jitterPixels[0];
		cams[0].jitter[1] = jitterPixels[1];
	}
	pure::M4 VPe = pure::m4_mul(PeJ, Ve);
	CullingObserveGameFov(2 * pure::fatan(1 / P.at(1, 1)) * 57.29578f);
	s_viewFix = pure::m4_mul(Ve, C);     // game view space -> world -> eye view space
	s_viewFixInv = pure::m4_mul(V, Ce);  // and back
	memcpy(data, Ve.m, 64);
	memcpy(data + 16, PeJ.m, 64);
	memcpy(data + 32, VPe.m, 64);
	memcpy(data + 48, Ce.m, 64);
	s_frame.patchedScene = true;
	if (s_frame.eye >= 0 && s_frame.eye < 2 && !s_secondEye) {
		EyeShot& shot = s_lastShot[s_frame.eye];
		shot.valid = true;
		shot.frameNo = s_frameNo;
		shot.Ce = Ce;
		shot.Pe = Pe;
		shot.E = EyeMatrixScaled();
		memcpy(shot.localPos, s_frame.localPos, 12);
		memcpy(shot.localRot, s_frame.localRot, 16);
		shot.halfW = s_frame.halfW;
		shot.halfH = s_frame.halfH;
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

void PatchCamParam(float* data) {
	data[2] = s_frame.halfW;
	data[3] = s_frame.halfH;
	if (s_haveCamera) {
		// Uploaded before this frame's SceneBuffer: use last frame's orientation with this
		// frame's position (the difference is far below a pixel).
		pure::M4 E = EyeMatrixScaled();
		float offset[3] = {E.at(0, 3), E.at(1, 3), E.at(2, 3)};
		for (int k = 0; k < 3; k++)
			data[4 + k] += s_lastCam.at(k, 0) * offset[0] + s_lastCam.at(k, 1) * offset[1] + s_lastCam.at(k, 2) * offset[2] +
						   s_lastCameraOffset[k];
	}
	s_frame.patchedCam = true;
}

}  // namespace

// Game thread API
void StereoSetAspect(float aspect) {
	if (aspect > 0.1f && aspect < 10) s_backbufferAspect = aspect;
}

void StereoCountDraw() { s_drawsThisFrame++; }

namespace {
// The eye's pose and frustum for this frame (g_stereo.lock held, shared).
void FillEye(FrameState& frame, int eyeIndex) {
	frame.eye = eyeIndex;
	pure::M4 ref = pure::m4_from_pose({g_stereo.refRot[0], g_stereo.refRot[1], g_stereo.refRot[2], g_stereo.refRot[3]},
									  {g_stereo.refPos[0], g_stereo.refPos[1], g_stereo.refPos[2]});
	const float* eyeRot = g_stereo.eyeRot[frame.eye];
	const float* eyePos = g_stereo.eyePos[frame.eye];
	pure::M4 eye = pure::m4_from_pose({eyeRot[0], eyeRot[1], eyeRot[2], eyeRot[3]}, {eyePos[0], eyePos[1], eyePos[2]});
	frame.eyeRel = pure::m4_relative(ref, eye);
	for (int hand = 0; hand < 2; hand++) {
		frame.handValid[hand] = g_stereo.handValid[hand];
		const float* p = g_stereo.handPos[hand];
		const float* q = g_stereo.handRot[hand];
		if (frame.handValid[hand]) frame.handRel[hand] = pure::m4_relative(ref, pure::m4_from_pose({q[0], q[1], q[2], q[3]}, {p[0], p[1], p[2]}));
	}
	memcpy(frame.refRot, g_stereo.refRot, 16);
	{
		const float* p0 = g_stereo.eyePos[0];
		const float* p1 = g_stereo.eyePos[1];
		float dx = p0[0] - p1[0], dy = p0[1] - p1[1], dz = p0[2] - p1[2];
		frame.ipd = pure::fsqrt(dx * dx + dy * dy + dz * dz);
	}
	memcpy(frame.localPos, eyePos, 12);
	memcpy(frame.localRot, eyeRot, 16);
	frame.poseBase = g_stereo.poseBase;
	const float* tangents = g_stereo.eyeTan[frame.eye];  // tan of left, right, up, down
	if (g_cfg.anamorphic) {
		// Fill the eye's own field of view in both directions: the 16:9 image is stretched to
		// the eye's shape (non-square pixels) instead of wasting width. CamParam stores the
		// horizontal and vertical tangents separately, so depth-based effects stay correct.
		frame.halfW = -tangents[0] > tangents[1] ? -tangents[0] : tangents[1];
		frame.halfH = tangents[2] > -tangents[3] ? tangents[2] : -tangents[3];
		if (g_cfg.verticalFill < 1) {  // optional: crop the vertical angle for more detail
			float fill = g_cfg.verticalFill < 0.5f ? 0.5f : g_cfg.verticalFill;
			frame.halfH = pure::ftan(pure::fatan(frame.halfH) * fill);
		}
	} else {
		pure::cover_fov(tangents[0], tangents[1], tangents[2], tangents[3], s_backbufferAspect, &frame.halfW, &frame.halfH);
		// VerticalFill < 1 trades vertical field of view for sharpness: at 0 the frustum just
		// covers the eye's width, cropping the top and bottom of the view.
		float wide = -tangents[0] > tangents[1] ? -tangents[0] : tangents[1];
		if (frame.halfW > wide && g_cfg.verticalFill < 1) {
			float narrowW = wide + (frame.halfW - wide) * (g_cfg.verticalFill < 0 ? 0 : g_cfg.verticalFill);
			frame.halfH *= narrowW / frame.halfW;
			frame.halfW = narrowW;
		}
	}
	frame.halfW *= g_cfg.fovScale;
	frame.halfH *= g_cfg.fovScale;
}
}  // namespace

// Called right after the game's Present: decides what the next frame renders.
void StereoBeginFrame() {
	s_frameNo++;
	{
		int32_t draws = s_drawsThisFrame.exchange(0);
		bool simple = g_cfg.flatBelowDraws > 0 && draws < g_cfg.flatBelowDraws;
		s_simpleFrames = simple ? s_simpleFrames + 1 : 0;
		s_fullFrames = simple ? 0 : s_fullFrames + 1;
		if (!s_simpleScene && s_simpleFrames >= 20) {
			s_simpleScene = true;
			Log("stereo: simple scene (%d draws per frame, e.g. the title screen): shown flat", (int)draws);
		} else if (s_simpleScene && s_fullFrames >= 20) {
			s_simpleScene = false;
			Log("stereo: full scene again (%d draws per frame): stereo", (int)draws);
		}
	}
	FrameState frame = {};
	AcquireSRWLockShared(&g_stereo.lock);
	// the headset showing frames: else (disconnected, taken off, the runtime paused) the game renders flat
	bool shown = g_stereo.shownTick && GetTickCount() - g_stereo.shownTick < kHeadsetGoneMs;
	if (shown != s_headsetShown) {
		s_headsetShown = shown;
		Log("stereo: %s", shown ? "the headset shows frames: stereo" : "the headset shows no frames (disconnected or idle): flat");
	}
	bool on = g_stereo.enabled && shown && g_stereo.valid && g_stereo.refValid && !g_stereo.menuMode && !s_simpleScene;
	// TopDownFlat: steep top-down views (e.g. the flight unit) go to the flat screen about 1 s after
	// the camera tilts down TopDownFlatFrom or more, and back about 2 s after it is below
	// TopDownFlatUntil.
	bool wasTopDown = s_topDownFlat;
	if (!g_cfg.topDownFlat) s_topDownFlat = false;
	else if (!s_topDownFlat && s_steepFrames >= 60) s_topDownFlat = true;
	else if (s_topDownFlat && s_levelFrames >= 120) s_topDownFlat = false;
	if (s_topDownFlat != wasTopDown)
		Log("stereo: %s (camera pitch %.0f deg)", s_topDownFlat ? "top-down view: flat screen (TopDownFlat=1)" : "top-down view ended: stereo",
			(double)s_lastPitch);
	frame.observe = on && s_topDownFlat;
	on = on && !s_topDownFlat;
	if (on) {
		bool synchronized = g_cfg.synchronized && ReplayAvailable();
		FillEye(frame, synchronized ? 0 : (int)(s_frameNo & 1));
		s_synchronized = synchronized;
		if (synchronized) {
			s_rightEyeFrame = frame;
			FillEye(s_rightEyeFrame, 1);
		}
	}
	ReleaseSRWLockShared(&g_stereo.lock);
	frame.active = on;
	s_frame = frame;
	FirstPersonNewFrame();
	FirstPersonSetHiding(false);  // until this frame's camera is at the character's eyes
	FoveatedFrameStart();
	if (!on) s_synchronized = false;
	s_rightEyeFrame.active = on;
	s_rightEyeFrame.observe = false;
	s_secondEye = false;
	CullingUpdate(on);
	for (auto& p : s_pendingMaps) p.res = nullptr;
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
	if (s_frame.active && s_frame.patchedScene && s_haveCamera) {
		// eye world orientation = C0 * eyeRel = C0 * ref^-1 * eye: LOCAL -> world is C0 * ref^-1
		const float* refRot = s_frame.refRot;
		pure::M4 ref = pure::m4_from_pose({refRot[0], refRot[1], refRot[2], refRot[3]}, {0, 0, 0});
		pure::M4 camRot = s_lastCam;
		camRot.at(0, 3) = camRot.at(1, 3) = camRot.at(2, 3) = 0;
		pure::Q q = pure::q_from_m4(pure::m4_mul(camRot, pure::m4_rigid_inverse(ref)));
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
bool StereoEyeTangents(float* halfW, float* halfH) {
	if (!s_frame.active || !(s_frame.halfW > 0.05f && s_frame.halfH > 0.05f)) return false;
	*halfW = s_frame.halfW;
	*halfH = s_frame.halfH;
	return true;
}
bool StereoSynchronizedFrame() { return s_synchronized && s_frame.active; }
void StereoBeginSecondEye() {
	FrameState frame = s_rightEyeFrame;
	frame.patchedScene = frame.patchedCam = false;
	s_frame = frame;
	s_secondEye = true;
	for (auto& p : s_pendingMaps) p.res = nullptr;
}
// Depth reprojection: the other eye's image from the previous frame, moved to this frame's camera.
// T = that eye's view as it would be now (same head pose relative to the camera) <- its view then;
// proj = its projection's x/y scale and depth terms. Returns false when there is no such image.
bool StereoReprojectParams(int eye, float T[16], float proj[4], FrameTag* tag) {
	if (eye < 0 || eye > 1 || !s_frame.active || !s_frame.patchedScene || s_frame.eye == eye || !s_haveCamera) return false;
	const EyeShot& shot = s_lastShot[eye];
	if (!shot.valid || shot.frameNo + 1 != s_frameNo) return false;
	pure::M4 now = pure::m4_mul(s_lastCam, shot.E);
	pure::M4 transform = pure::m4_mul(pure::m4_rigid_inverse(now), shot.Ce);
	memcpy(T, transform.m, 64);
	proj[0] = shot.Pe.at(0, 0);
	proj[1] = shot.Pe.at(1, 1);
	proj[2] = shot.Pe.at(2, 2);
	proj[3] = shot.Pe.at(2, 3);
	*tag = {};
	tag->eye = eye;
	tag->patched = true;
	tag->hasScene = true;
	tag->noUI = true;
	memcpy(tag->pos, shot.localPos, 12);
	memcpy(tag->rot, shot.localRot, 16);
	tag->tanHalfW = shot.halfW;
	tag->tanHalfH = shot.halfH;
	return true;
}
int StereoFrameEye() { return s_frame.active ? s_frame.eye : -1; }

// Upscaling: this frame's camera of `eye` and how it moved since that eye's previous frame.
// T = previous eye view <- this eye view; cur/prev = x scale, y scale, P22, P23 of the projections
// (without jitter); jitter = this frame's offset in render pixels. False if the camera of `eye`
// wasn't found this frame. *hasPrev: there is a previous frame close enough to compare with.
bool StereoUpscaleCams(int eye, float T[16], float cur[4], float prev[4], float jitter[2], bool* hasPrev) {
	if (eye < 0 || eye > 1) return false;
	const UpscaleCamera& current = s_upscaleCams[eye][0];
	const UpscaleCamera& previous = s_upscaleCams[eye][1];
	if (!current.valid || current.frameNo != s_frameNo) return false;
	*hasPrev = previous.valid && current.frameNo - previous.frameNo <= 3;  // alternating mode: 2 frames apart
	pure::M4 transform = pure::m4_mul(pure::m4_rigid_inverse(previous.Ce), current.Ce);
	memcpy(T, transform.m, 64);
	const pure::M4* projs[2] = {&current.Pe, &previous.Pe};
	float* outs[2] = {cur, prev};
	for (int k = 0; k < 2; k++) {
		outs[k][0] = projs[k]->at(0, 0);
		outs[k][1] = projs[k]->at(1, 1);
		outs[k][2] = projs[k]->at(2, 2);
		outs[k][3] = projs[k]->at(2, 3);
	}
	jitter[0] = current.jitter[0];
	jitter[1] = current.jitter[1];
	return true;
}

bool StereoSecondEyePatched() { return s_secondEye && s_frame.patchedScene; }
// The game's own camera (camera-to-world, before levelling): position, forward, up.
bool StereoGameCamera(float pos[3], float fwd[3], float up[3]) {
	if (!s_haveCamera) return false;
	for (int k = 0; k < 3; k++) {
		pos[k] = s_lastGameCam.at(k, 3);
		fwd[k] = -s_lastGameCam.at(k, 2);
		up[k] = s_lastGameCam.at(k, 1);
	}
	return true;
}
// The eye's orientation relative to the game camera, if an eye was rendered in the last 250 ms.
bool StereoHeadRelative(pure::M4* rel) {
	AcquireSRWLockShared(&s_headRelLock);
	bool ok = s_headRelTick && GetTickCount() - s_headRelTick < 250;
	if (ok) *rel = s_headRel;
	ReleaseSRWLockShared(&s_headRelLock);
	return ok && s_frame.active;
}
// Head culling: vertical fov (radians) of a culling view, centred on the eye's direction,
// that holds the eye's whole view plus marginDeg on every side (game frames lag the head a little).
// 0 if no eye has been rendered yet.
float StereoHeadCullFov(float marginDeg) {
	float halfW = s_frame.halfW, halfH = s_frame.halfH;
	if (!(halfW > 0.05f && halfH > 0.05f && s_backbufferAspect > 0.1f)) return 0;
	float halfV = halfH > halfW / s_backbufferAspect ? halfH : halfW / s_backbufferAspect;  // the frustum has the game's aspect
	float half = pure::fatan(halfV) + marginDeg * 0.0174533f;
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
void ObserveScene(const float* data) {
	if (IsMainScene(data)) NotePitch(pure::camera_pitch_deg(pure::m4_rigid_inverse(LoadMatrix(data))));
}

void StereoOnMap(ID3D11Resource* res, void* data, UINT size) {
	if (!(s_frame.active || s_frame.observe) || !data || (size != 256 && size != 32)) return;
	for (auto& p : s_pendingMaps)
		if (!p.res) {
			p = {res, (float*)data, size == 256 ? SCENE_BUFFER : CAM_PARAM_BUFFER};
			return;
		}
}

void StereoBeforeUnmap(ID3D11Resource* res) {
	if (!(s_frame.active || s_frame.observe)) return;
	for (auto& p : s_pendingMaps) {
		if (p.res != res) continue;
		// The mapped memory may be write-combined: read it once into a local copy.
		float local[64];
		UINT floatCount = p.kind == SCENE_BUFFER ? 64 : 8;
		memcpy(local, p.data, floatCount * 4);
		if (!s_frame.active) {
			if (p.kind == SCENE_BUFFER) ObserveScene(local);
		} else if (p.kind == SCENE_BUFFER && IsMainScene(local)) {
			PatchScene(local);
			memcpy(p.data, local, floatCount * 4);
		} else if (p.kind == CAM_PARAM_BUFFER && IsCamParam(local)) {
			PatchCamParam(local);
			memcpy(p.data, local, floatCount * 4);
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
