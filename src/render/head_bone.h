// head_bone.h - first person's maths (first_person.cpp, camera.cpp): 2B's parts' fingerprint, her head
// and feet from her bones, the steady height. No Windows dependencies (tests/test_firstperson.cpp).
#pragma once
#include <iterator>
#include <math.h>
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

// Each skinned mesh part uploads its own bone list (skinningBuffer at VS b11): row-major 3x4 matrices
// (bone pose x inverse bind pose), 48 bytes each, in a buffer exactly that size. 2B's parts are first
// bound in this order each frame (found in F11 captures; 9S and the pods differ). Her clothing parts
// are missing after she self-destructs.
inline constexpr int32_t kBodyPartBones[] = {59, 1, 15, 63, 63, 25, 62, 63, 2, 23, 20, 14, 11, 14, 8};
inline constexpr int32_t kBodyPartCount = std::size(kBodyPartBones);
inline constexpr bool is_clothing_part(int32_t part) { return part == 2 || part == 3 || part == 4 || part == 6 || part == 7; }
// Her head-only meshes: face (with the eyes), hair (two parts), face details, and small lists outside the
// fingerprint of her head and eye bones (her eyes).
inline constexpr int32_t kHeadOnlyList = 100;
inline constexpr bool is_head_only_part(int32_t part) { return part == 1 || part == 9 || part == 10 || part == 13 || part == kHeadOnlyList; }
inline constexpr int32_t kBoneBytes = 48;
inline constexpr int32_t kPartsToConfirm = 3;  // her parts in a row before they count as hers

// The head bone, and her eyes in its bind pose (metres, +y up, +z forward), in front of the neck pivot
// at (0, 1.49, -0.02).
inline constexpr int32_t kHeadPart = 0, kHeadSlot = 10;
inline constexpr V3 kEyeBind = {0, 1.56f, 0.07f};
// Where the camera sits on her head: 6 cm above her eyes, so looking down you see past her dress's collar.
// Her head is posed so this point is at your head.
inline constexpr V3 kViewBind = {0, 1.62f, 0.07f};
inline constexpr V3 kNeckPivotBind = {0, 1.49f, -0.019f};
// Her ankles (in her boots), and how high they are when she stands.
inline constexpr int32_t kFeetPart = 14, kLeftAnkleSlot = 0, kRightAnkleSlot = 4;
inline constexpr V3 kLeftAnkleBind = {-0.169f, 0.174f, -0.020f}, kRightAnkleBind = {0.169f, 0.174f, -0.020f};

// Says which of 2B's parts each bone list is, from the sizes of the lists as they are first bound in a
// frame (a list bound again keeps the part it was given: first_person.cpp remembers them).
struct BodyPartWalker {
	int32_t next = 0;     // her part expected next (0: looking for her first part)
	int32_t matched = 0;  // her parts in a row so far

	// A bone list not seen yet this frame (`bones` = its size / 48). Returns which of her parts it is,
	// or -1. Clothing parts may be missing.
	int32_t identify(int32_t bones) {
		if (next > 0)
			for (int32_t part = next; part < kBodyPartCount; part++) {
				if (bones == kBodyPartBones[part]) {
					next = part + 1;
					matched++;
					return part;
				}
				if (!is_clothing_part(part)) break;
			}
		if (bones == kBodyPartBones[0]) {
			next = 1;
			matched = 1;
			return 0;
		}
		next = matched = 0;
		return -1;
	}
	bool confirmed() const { return matched >= kPartsToConfirm; }
};

// A point in a bone's bind pose, moved by its 3x4 matrix (each row: rotation then translation).
inline V3 bone_point(const float m[12], V3 p) {
	return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
			m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

// A bone matrix that still turns without stretching (not collapsed or garbage).
inline bool plausible_bone(const float m[12]) {
	for (int row = 0; row < 3; row++) {
		float squared = m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2];
		if (!(squared > 0.81f && squared < 1.21f)) return false;
	}
	return true;
}

// Where her eyes are and which way her head faces, in the game world.
struct HeadPose {
	V3 eye;
	V3 forward;  // unit length
};

inline HeadPose head_pose(const float head[12], V3 eyeBind) {
	V3 forward = {head[2], head[6], head[10]};  // the bone's +z
	float size = length(forward);
	return {bone_point(head, eyeBind), size > 1e-6f ? forward / size : V3{0, 0, 1}};
}

// The ground under her: below the lower ankle by the ankles' standing height.
inline float ground_height(const float leftAnkleBone[12], const float rightAnkleBone[12]) {
	float left = bone_point(leftAnkleBone, kLeftAnkleBind).y, right = bone_point(rightAnkleBone, kRightAnkleBind).y;
	return (left < right ? left : right) - kLeftAnkleBind.y;
}

// Steady eye height (FirstPersonSteadyHeight): the ground under her, quickly followed (jumps, stairs),
// plus her eyes' height above it, slowly followed (leaning into a sprint) so running bob and quick dips
// don't move you.
struct SteadyHeight {
	static constexpr float kGroundSeconds = 0.15f, kHeightSeconds = 0.8f;
	bool valid = false;
	float ground = 0, height = 0;

	float update(float eyeY, float groundY, float seconds) {
		if (!valid || seconds > 0.5f) {  // first time, or after a pause: start where she is
			valid = true;
			ground = groundY;
			height = eyeY - groundY;
		} else if (seconds > 0) {
			ground += (groundY - ground) * (1 - expf(-seconds / kGroundSeconds));
			height += (eyeY - groundY - height) * (1 - expf(-seconds / kHeightSeconds));
		}
		return ground + height;
	}
};

// Camera-to-world at `position`, looking along the horizontal part of `forward` with a level horizon.
// False when `forward` is (nearly) vertical.
inline bool m4_level_camera_facing(V3 forward, V3 position, M4* out) {
	Q yaw;
	if (!yaw_from_forward(forward, &yaw)) return false;
	*out = m4_from_pose(yaw, position);
	return true;
}

}  // namespace pure
