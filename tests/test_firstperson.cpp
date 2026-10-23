// First person (pure::first_person_pos): eyes at the camera's look-at target, plus offsets.
#include <cstdio>
#include <cmath>
#include "core/pure.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
static bool near3(const float* a, float x, float y, float z) {
	return std::fabs(a[0] - x) < 1e-4f && std::fabs(a[1] - y) < 1e-4f && std::fabs(a[2] - z) < 1e-4f;
}
int main() {
	float p[3];
	const float eye[3] = {0, 2, 3}, target[3] = {0, 1.5f, 0};  // camera 3 behind and 0.5 above, looking -Z
	check(pure::first_person_pos(eye, target, 0, 0, p) && near3(p, 0, 1.5f, 0), "no offsets: at the target");
	check(pure::first_person_pos(eye, target, 0.2f, 0, p) && near3(p, 0, 1.7f, 0), "height is straight up");
	check(pure::first_person_pos(eye, target, 0, 0.3f, p) && near3(p, 0, 1.5f, -0.3f),
		  "forward is the camera's horizontal facing (its downward tilt ignored)");
	const float eye2[3] = {3, 1.5f, 0};  // camera to the side, looking -X
	check(pure::first_person_pos(eye2, target, 0, 0.5f, p) && near3(p, -0.5f, 1.5f, 0), "forward follows the camera's yaw");
	const float above[3] = {0, 5, 0};
	check(!pure::first_person_pos(above, target, 0, 0.3f, p), "camera straight above: no facing");
	if (!fails) std::printf("first person tests passed\n");
	return fails ? 1 : 0;
}
