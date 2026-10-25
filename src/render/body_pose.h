// body_pose.h - first person's body (first_person.cpp): her upper body follows your head, her legs keep the game's animation. No Windows dependencies.
#pragma once
#include <stdint.h>
#include "core/vecmath.h"
#include "render/body_skeleton.h"
#include "render/head_bone.h"

namespace pure {

// A bone list's matrix (3x4, rows) as a 4x4, and a bone moved by `delta` (world): delta x matrix.
inline M4 m4_from_bone(const float m[12]) {
	M4 r = m4_identity();
	for (int i = 0; i < 12; i++) r.m[i] = m[i];
	return r;
}
inline void move_bone(const M4& delta, const float in[12], float out[12]) {
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			out[r * 4 + c] = delta.at(r, 0) * in[c] + delta.at(r, 1) * in[4 + c] + delta.at(r, 2) * in[8 + c] + (c == 3 ? delta.at(r, 3) : 0);
}

// `rotation` (its translation ignored) turning about `from`, which lands on `to`.
inline M4 m4_turn_about(const M4& rotation, V3 from, V3 to) {
	M4 r = rotation;
	V3 turned = {r.at(0, 0) * from.x + r.at(0, 1) * from.y + r.at(0, 2) * from.z, r.at(1, 0) * from.x + r.at(1, 1) * from.y + r.at(1, 2) * from.z,
				 r.at(2, 0) * from.x + r.at(2, 1) * from.y + r.at(2, 2) * from.z};
	r.at(0, 3) = to.x - turned.x;
	r.at(1, 3) = to.y - turned.y;
	r.at(2, 3) = to.z - turned.z;
	r.at(3, 0) = r.at(3, 1) = r.at(3, 2) = 0;
	r.at(3, 3) = 1;
	return r;
}

// The smallest rotation turning direction `a` towards `b`, by at most `maxRadians`.
inline Q q_towards(V3 a, V3 b, float maxRadians) {
	float la = length(a), lb = length(b);
	if (la < 1e-6f || lb < 1e-6f) return {0, 0, 0, 1};
	a = a / la;
	b = b / lb;
	V3 axis = cross(a, b);
	float sine = length(axis), cosine = dot(a, b);
	if (sine < 1e-6f) return {0, 0, 0, 1};  // already there (or exactly opposite: left alone)
	float angle = 2 * fatan(sine / (1 + cosine));  // tan(angle / 2)
	if (angle > maxRadians) angle = maxRadians;
	float s, c;
	fsincos(angle / 2, &s, &c);
	axis = axis / sine;
	return {axis.x * s, axis.y * s, axis.z * s, c};
}

inline constexpr int32_t slot_of(const uint8_t* slots, int32_t count, int32_t bone) {
	for (int32_t i = 0; i < count; i++)
		if (slots[i] == bone) return i;
	return -1;
}

inline constexpr float kLeanRadius = 0.25f;  // further from her neck than this (horizontally, metres): all of her slides along
inline constexpr float kMaxBend = 1.0f;      // her waist bends at most this far (radians) to reach your head

// What a pose did (the log's report): how far your head was from hers, how far she slid, how far her waist bent.
struct BodyPoseInfo {
	float headDistance, slide, bend;
};

// How each of her bones moves (world, delta x matrix) so her view point (kViewBind) is at `eyeTarget` and her head turned
// like `view` (camera-to-world, -z forward). Her head goes there exactly; her waist turns so her neck
// reaches it; when your head is further than kLeanRadius from her neck, all of her (legs too) slides
// along. `waist` and `head` are her bones as the game posed them.
inline void pose_upper_body(const float waist[12], const float head[12], V3 eyeTarget, const M4& view, M4 delta[kSkeletonBones],
							BodyPoseInfo* info = nullptr) {
	// head: bone +z forward, the camera's -z forward (turned half a turn about y)
	M4 headTurn = m4_identity();
	for (int r = 0; r < 3; r++) {
		headTurn.at(r, 0) = -view.at(r, 0);
		headTurn.at(r, 1) = view.at(r, 1);
		headTurn.at(r, 2) = -view.at(r, 2);
	}
	M4 gameHead = m4_identity();  // the game's head rotation, inverted (transposed)
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) gameHead.at(r, c) = head[c * 4 + r];
	V3 eye = bone_point(head, kViewBind);
	M4 headDelta = m4_turn_about(m4_mul(headTurn, gameHead), eye, eyeTarget);

	V3 neck = bone_point(head, kNeckPivotBind);
	V3 neckTarget = {headDelta.at(0, 0) * neck.x + headDelta.at(0, 1) * neck.y + headDelta.at(0, 2) * neck.z + headDelta.at(0, 3),
					 headDelta.at(1, 0) * neck.x + headDelta.at(1, 1) * neck.y + headDelta.at(1, 2) * neck.z + headDelta.at(1, 3),
					 headDelta.at(2, 0) * neck.x + headDelta.at(2, 1) * neck.y + headDelta.at(2, 2) * neck.z + headDelta.at(2, 3)};
	V3 away = {neckTarget.x - neck.x, 0, neckTarget.z - neck.z};
	float distance = length(away);
	V3 slide = distance > kLeanRadius ? away * (1 - kLeanRadius / distance) : V3{0, 0, 0};

	V3 waistPivot = bone_point(waist, kSkeleton[BONE_WAIST].bind), waistMoved = waistPivot + slide;
	Q bend = q_towards(neck - waistPivot, neckTarget - waistMoved, kMaxBend);
	M4 waistDelta = m4_turn_about(m4_from_pose(bend, {0, 0, 0}), waistPivot, waistMoved);
	if (info) *info = {length(eyeTarget - eye), length(slide), 2 * fatan(length(V3{bend.x, bend.y, bend.z}) / bend.w)};

	M4 slid = m4_identity();
	slid.at(0, 3) = slide.x;
	slid.at(2, 3) = slide.z;
	for (int32_t bone = 0; bone < kSkeletonBones; bone++) {
		if (bone == BONE_HEAD) delta[bone] = headDelta;
		else if (bone == BONE_WAIST) delta[bone] = waistDelta;
		else delta[bone] = kSkeleton[bone].parent < 0 ? slid : delta[kSkeleton[bone].parent];
	}
}

// ---------------------------------------------------------------------------------------------
// Her arms on your controllers
// ---------------------------------------------------------------------------------------------
inline V3 moved_point(const M4& m, V3 p) {
	return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
			m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}
inline V3 normalized(V3 v) {
	float size = length(v);
	return size > 1e-6f ? v / size : V3{0, 0, 0};
}
inline V3 m4_column(const M4& m, int c) { return {m.at(0, c), m.at(1, c), m.at(2, c)}; }

// A rotation (in a 4x4) whose columns are `forward`, `across` (made perpendicular to it) and their cross product.
inline M4 m4_basis(V3 forward, V3 across) {
	V3 f = normalized(forward), a = normalized(across - f * dot(across, f)), c = cross(f, a);
	M4 r = m4_identity();
	const V3 columns[3] = {f, a, c};
	for (int col = 0; col < 3; col++) {
		r.at(0, col) = columns[col].x;
		r.at(1, col) = columns[col].y;
		r.at(2, col) = columns[col].z;
	}
	return r;
}
inline M4 m4_rotation_transposed(const M4& m) {
	M4 r = m4_identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r.at(i, j) = m.at(j, i);
	return r;
}

// One arm's bones, and the knuckles that say how its hand is turned.
struct ArmBones {
	int32_t upper, fore, hand;
	int32_t middleKnuckle, indexKnuckle, littleKnuckle;
	float outward;  // her side: -1 left (-x), +1 right
};
inline constexpr ArmBones kLeftArm = {BONE_LEFT_UPPER_ARM, BONE_LEFT_FOREARM, BONE_LEFT_HAND, 45, 41, 53, -1};
inline constexpr ArmBones kRightArm = {BONE_RIGHT_UPPER_ARM, BONE_RIGHT_FOREARM, BONE_RIGHT_HAND, 73, 77, 65, 1};

inline constexpr float kGripTilt = 0.6f;       // her hand points this far (radians) above the controller's barrel
inline constexpr float kPalmToWrist = 0.08f;   // her wrist this far behind the controller's grip point (your palm)
inline constexpr float kStraightArm = 0.995f;  // an arm reaching further than this of its length stays this straight

// Where her wrist and hand go for a controller's grip pose (camera-to-world style: -z along the barrel,
// +y up, +x right): her hand points along the barrel tilted up by kGripTilt, index knuckle on top.
struct HandTarget {
	V3 wrist;
	V3 forward;  // wrist to middle knuckle
	V3 across;   // index knuckle to little knuckle
};
inline HandTarget hand_target(const M4& grip) {
	float s, c;
	fsincos(kGripTilt, &s, &c);
	V3 y = m4_column(grip, 1), z = m4_column(grip, 2);
	V3 forward = y * s - z * c, across = y * -c - z * s;  // -z and -y turned up about x
	return {m4_column(grip, 3) - forward * kPalmToWrist, forward, across};
}

// One arm on a controller: her upper arm and forearm turned (two-bone IK, the elbow down, out and back)
// so her wrist reaches the target, her hand turned like the controller. `delta` already holds the upper
// body's pose; the arm's bones and everything below them (fingers, twist helpers) are changed.
// `upper`, `fore`, `hand`, `chest` are her bones as the game posed them.
inline void pose_arm(const ArmBones& arm, const float upper[12], const float fore[12], const float hand[12], const float chest[12], const M4& grip,
					 M4 delta[kSkeletonBones]) {
	V3 shoulderBind = kSkeleton[arm.upper].bind, elbowBind = kSkeleton[arm.fore].bind, wristBind = kSkeleton[arm.hand].bind;
	float upperLength = length(elbowBind - shoulderBind), foreLength = length(wristBind - elbowBind);
	V3 shoulder = moved_point(delta[arm.upper], bone_point(upper, shoulderBind));
	V3 elbowNow = moved_point(delta[arm.upper], bone_point(fore, elbowBind));
	HandTarget target = hand_target(grip);

	// the elbow: in the plane through shoulder, wrist and a point down, out and behind her
	V3 reach = target.wrist - shoulder;
	float distance = length(reach), longest = (upperLength + foreLength) * kStraightArm, shortest = fabsf(upperLength - foreLength) + 0.01f;
	distance = distance > longest ? longest : (distance < shortest ? shortest : distance);
	V3 along = normalized(reach);
	V3 wrist = shoulder + along * distance;
	M4 chestNow = m4_mul(delta[BONE_CHEST], m4_from_bone(chest));
	V3 pole = m4_column(chestNow, 0) * (0.4f * arm.outward) - m4_column(chestNow, 1) - m4_column(chestNow, 2) * 0.3f;
	V3 bendTowards = normalized(pole - along * dot(pole, along));
	float toElbow = (distance * distance + upperLength * upperLength - foreLength * foreLength) / (2 * distance);
	float out = upperLength * upperLength - toElbow * toElbow;
	V3 elbow = shoulder + along * toElbow + bendTowards * (out > 0 ? fsqrt(out) : 0);

	M4 upperDelta = m4_mul(m4_turn_about(m4_from_pose(q_towards(elbowNow - shoulder, elbow - shoulder, 3.2f), {0, 0, 0}), shoulder, shoulder),
						   delta[arm.upper]);
	V3 wristNow = moved_point(upperDelta, bone_point(hand, wristBind));
	M4 foreDelta =
		m4_mul(m4_turn_about(m4_from_pose(q_towards(wristNow - elbow, wrist - elbow, 3.2f), {0, 0, 0}), elbow, elbow), upperDelta);

	// the hand: its bind pose turned onto the target's directions, its wrist on the arm's end
	V3 bindForward = kSkeleton[arm.middleKnuckle].bind - wristBind, bindAcross = kSkeleton[arm.littleKnuckle].bind - kSkeleton[arm.indexKnuckle].bind;
	M4 handPose = m4_mul(m4_basis(target.forward, target.across), m4_rotation_transposed(m4_basis(bindForward, bindAcross)));
	handPose = m4_turn_about(handPose, wristBind, wrist);  // the bone list's matrix she gets (bone pose x inverse bind)
	M4 handDelta = m4_mul(handPose, m4_rigid_inverse(m4_from_bone(hand)));

	bool moved[kSkeletonBones] = {};
	for (int32_t bone = 0; bone < kSkeletonBones; bone++) {
		int32_t parent = kSkeleton[bone].parent;
		if (bone == arm.upper) delta[bone] = upperDelta;
		else if (bone == arm.fore) delta[bone] = foreDelta;
		else if (bone == arm.hand) delta[bone] = handDelta;
		else if (parent >= 0 && moved[parent]) delta[bone] = delta[parent];
		else continue;
		moved[bone] = true;
	}
}

}  // namespace pure
