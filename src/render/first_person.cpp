// first_person.cpp - first person: 2B's head from her bone uploads, her head hidden from your eyes, her body following your head.
//
// Skinned meshes upload their bone lists (skinningBuffer, VS b11) with Map, one buffer per mesh part,
// sized bones x 48 bytes. The game may hand a part a different buffer each frame, so nothing here
// remembers buffers from one frame to the next.
// Labels: the first time each list is bound in a frame, the sizes say which of 2B's parts it is
// (pure::BodyPartWalker; her parts come first in the depth pass, in a fixed order). The list keeps that
// label for the rest of the frame, whatever order later passes draw in. Her head bone (part 0) gives her
// eyes and facing, her ankles (part 14) the ground under her; camera.cpp puts the camera there, one game
// frame behind (her bones are uploaded after the camera). Her head-only parts are left out of the main
// view while you look through her eyes; the shadow map still gets them.
// Copies: every list that could be hers is copied as it is uploaded. When a list her body's size is first
// bound, her lists are found among the copies by their contents (a bone in two of her lists has the same
// matrix in both), and so are the tiny lists of her head and eye bones (her eyes, outside the
// fingerprint). With FirstPersonBody her bones are then moved (pure::pose_upper_body) and all her lists
// uploaded again, before she is drawn (through the hooks, so the synchronized recording gets them too).
// Her bones are uploaded before the frame's camera, so posing follows whether the last frame was in her eyes.
// All of this runs on the game's render thread.
#include "render/first_person.h"
#include "render/body_pose.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include <math.h>
#include <string.h>

namespace {
constexpr UINT kSkinningSlot = 11;
constexpr DWORD kFreshMs = 250;  // her head or feet older than this aren't used (cutscene, other outfit, ...)
constexpr int32_t kMaxPartBones = 63, kMaxTinyBones = 4;
constexpr float kMaxReach = 1.0f;  // her eyes follow your head at most this far (metres; further: a recentre, a glitch)
uint32_t s_frame = 1;

// ---------------------------------------------------------------------------------------------
// This frame's copies of the lists that could be hers (the mapped memory may be write-combined: read once)
// ---------------------------------------------------------------------------------------------
struct ListCopy {
	ID3D11Resource* list;
	int32_t bones;
	float* data;
};
float s_partData[128][kMaxPartBones * 12];   // lists the size of one of her parts
float s_tinyData[1024][kMaxTinyBones * 12];  // lists of 1 to 4 bones (some 350 a frame)
ListCopy s_copies[std::size(s_partData) + std::size(s_tinyData)];
int32_t s_copyCount, s_partCopies, s_tinyCopies;
bool s_loggedCopiesFull;
// Each list's copy this frame, by the buffer's address (open addressing; 0 = none)
struct CopyIndex {
	const void* list;
	uint32_t frame;
	int32_t copy;
};
CopyIndex s_copyIndex[4096];
struct PendingMap {
	ID3D11Resource* list;
	float* data;
	UINT byteWidth;
} s_pending[4];
int32_t s_pendingNext;

bool CouldBeHers(int32_t bones) {
	if (bones >= 1 && bones <= kMaxTinyBones) return true;
	for (int32_t size : pure::kBodyPartBones)
		if (bones == size) return true;
	return false;
}

CopyIndex* IndexOf(const void* list) {
	uintptr_t hash = ((uintptr_t)list >> 4) * 0x9E3779B97F4A7C15ull;
	for (size_t i = 0; i < std::size(s_copyIndex); i++) {
		CopyIndex& entry = s_copyIndex[(hash + i) % std::size(s_copyIndex)];
		if (entry.frame != s_frame || entry.list == list) return &entry;
	}
	return nullptr;
}

ListCopy* CopyOf(const void* list) {
	CopyIndex* entry = IndexOf(list);
	return entry && entry->frame == s_frame ? &s_copies[entry->copy] : nullptr;
}

void TakeCopy(ID3D11Resource* list, const float* data, int32_t bones) {
	ListCopy* copy = CopyOf(list);
	if (!copy || copy->bones != bones) {
		bool tiny = bones <= kMaxTinyBones;
		if (s_copyCount >= (int32_t)std::size(s_copies) || (tiny ? s_tinyCopies >= (int32_t)std::size(s_tinyData) : s_partCopies >= (int32_t)std::size(s_partData))) {
			if (!s_loggedCopiesFull) {
				s_loggedCopiesFull = true;
				Log("first person: too many bone lists in a frame to copy them all (%d tiny, %d her parts' sizes); some of 2B's may be missed",
					s_tinyCopies, s_partCopies);
			}
			return;
		}
		CopyIndex* entry = IndexOf(list);
		if (!entry) return;
		*entry = {list, s_frame, s_copyCount};
		copy = &s_copies[s_copyCount++];
		*copy = {list, bones, tiny ? s_tinyData[s_tinyCopies++] : s_partData[s_partCopies++]};
	}
	memcpy(copy->data, data, (size_t)bones * pure::kBoneBytes);
}

// ---------------------------------------------------------------------------------------------
// Labels (per frame)
// ---------------------------------------------------------------------------------------------
struct Label {
	const void* list;
	uint32_t frame;
	int32_t part;  // which of her parts (-1: not hers)
	bool hers;     // the walk that labelled it was confirmed
};
Label s_labels[1024];
pure::BodyPartWalker s_walker;
Label* s_walkHead;  // this walk's part 0 and 1 labels, made hers once the walk is confirmed
Label* s_walkFace;
int32_t s_boundPart = -1;  // her part bound at b11 now (-1: none, or not a bone list)

Label* LabelOf(const void* list) {
	uintptr_t hash = ((uintptr_t)list >> 4) * 0x9E3779B97F4A7C15ull;
	for (size_t i = 0; i < std::size(s_labels); i++) {
		Label& label = s_labels[(hash + i) % std::size(s_labels)];
		if (label.frame != s_frame || label.list == list) return &label;  // free this frame, or this list's
	}
	return nullptr;  // full: over 1024 bone lists in a frame
}

// ---------------------------------------------------------------------------------------------
// Her head and the ground under her (for the camera)
// ---------------------------------------------------------------------------------------------
struct Tracked {
	bool valid;
	pure::HeadPose head;
	DWORD tick;
	bool groundValid;
	float ground;  // the ground under her (game units)
	DWORD groundTick;
	pure::SteadyHeight steady;
	double steadyMs;
	float steadyEyeY;
} s_tracked;

UINT s_gameW, s_gameH;
bool s_mainTarget;  // the render target is game-sized (not the shadow map)
bool s_hiding;      // this frame's eyes are in her head
bool s_wasHiding;   // ... and the last frame's
bool s_loggedFound, s_loggedHiding, s_loggedOutfit, s_loggedHeadOnly;

void TakeHead(const void* headList) {
	const ListCopy* copy = CopyOf(headList);
	if (!copy || copy->bones != pure::kBodyPartBones[pure::kHeadPart]) return;
	const float* head = copy->data + pure::kHeadSlot * 12;
	if (!pure::plausible_bone(head)) return;
	s_tracked.valid = true;
	s_tracked.head = pure::head_pose(head, pure::kViewBind);  // the camera's place on her head
	s_tracked.tick = GetTickCount();
	if (!s_loggedFound) {
		s_loggedFound = true;
		Log("first person: found 2B's bones; eyes at %.2f %.2f %.2f, facing %.2f %.2f %.2f", (double)s_tracked.head.eye.x,
			(double)s_tracked.head.eye.y, (double)s_tracked.head.eye.z, (double)s_tracked.head.forward.x,
			(double)s_tracked.head.forward.y, (double)s_tracked.head.forward.z);
	}
}

void TakeGround(const void* feetList) {
	const ListCopy* copy = CopyOf(feetList);
	if (!copy || copy->bones != pure::kBodyPartBones[pure::kFeetPart] || !s_tracked.valid) return;
	const float *left = copy->data + pure::kLeftAnkleSlot * 12, *right = copy->data + pure::kRightAnkleSlot * 12;
	if (!pure::plausible_bone(left) || !pure::plausible_bone(right)) return;
	s_tracked.groundValid = true;
	s_tracked.ground = pure::ground_height(left, right);
	s_tracked.groundTick = GetTickCount();
	double now = NowMs();
	s_tracked.steadyEyeY = s_tracked.steady.update(s_tracked.head.eye.y, s_tracked.ground, (float)((now - s_tracked.steadyMs) / 1000));
	s_tracked.steadyMs = now;
}

// A bone list bound for the first time this frame: which of her parts is it?
void Identify(Label* label, const void* list, int32_t bones) {
	int32_t part = s_walker.identify(bones);
	*label = {list, s_frame, part, s_walker.confirmed()};
	if (part == pure::kHeadPart) {
		s_walkHead = label;
		s_walkFace = nullptr;
	}
	if (part == 1) s_walkFace = label;
	if (part >= 0 && s_walker.matched == pure::kPartsToConfirm) {  // just confirmed
		if (s_walkHead) {
			s_walkHead->hers = true;
			TakeHead(s_walkHead->list);
		}
		if (s_walkFace) s_walkFace->hers = true;
		if (part != pure::kPartsToConfirm - 1 && !s_loggedOutfit) {  // her clothing parts were skipped
			s_loggedOutfit = true;
			Log("first person: 2B without her dress (after self-destructing): her head and feet are still found");
		}
	}
	if (part == pure::kFeetPart && label->hers) TakeGround(list);
}

// ---------------------------------------------------------------------------------------------
// Her lists among this frame's copies, by their contents
// ---------------------------------------------------------------------------------------------
struct HerFrame {
	bool found;
	int32_t parts;
	const ListCopy* part[pure::kBodyPartCount];
	const uint8_t* slots[pure::kBodyPartCount];
	const ListCopy* headOnly[8];  // tiny lists of her head and eye bones (her eyes)
	int32_t headOnlyCount;
} s_her;
float s_known[pure::kSkeletonBones][12];  // her bones' matrices found so far this frame
bool s_have[pure::kSkeletonBones];
constexpr int32_t kMinParts = 6;  // her parts found before they count (10 after self-destructing)
struct Feet {
	pure::V3 at;  // the ground under her hips
	DWORD tick;
} s_feet;

bool SameBone(const float* a, const float* b) {
	for (int i = 0; i < 12; i++)
		if (fabsf(a[i] - b[i]) > 1e-4f) return false;
	return true;
}

// Does this copy fit `slots` (bone per slot) with the bones known so far: at least one shared, all equal?
bool Fits(const ListCopy& copy, const uint8_t* slots) {
	int32_t shared = 0;
	for (int32_t slot = 0; slot < copy.bones; slot++) {
		if (!s_have[slots[slot]]) continue;
		if (!SameBone(copy.data + slot * 12, s_known[slots[slot]])) return false;
		shared++;
	}
	return shared > 0;
}

void Learn(const ListCopy& copy, const uint8_t* slots) {
	for (int32_t slot = 0; slot < copy.bones; slot++) {
		memcpy(s_known[slots[slot]], copy.data + slot * 12, sizeof(s_known[0]));
		s_have[slots[slot]] = true;
	}
}

// A tiny list of her head: one slot is her head bone, and the others stay at her eyes (her eye bones
// turn as she looks around, in place).
constexpr float kAtHerEyes = 0.03f;
bool RidesHerHead(const ListCopy& copy, const float* head) {
	pure::V3 eyes = pure::bone_point(head, pure::kEyeBind);
	bool hasHead = false;
	for (int32_t slot = 0; slot < copy.bones; slot++) {
		const float* bone = copy.data + slot * 12;
		if (SameBone(bone, head)) hasHead = true;
		else if (pure::length(pure::bone_point(bone, pure::kEyeBind) - eyes) > kAtHerEyes) return false;
	}
	return hasHead;
}

bool Assigned(const ListCopy* copy) {
	for (const ListCopy* part : s_her.part)
		if (part == copy) return true;
	return false;
}

// Her lists, starting from `body` (a list the size of her part 0): every list whose bones agree with
// hers so far joins, until no more do.
void FindHer(const ListCopy* body) {
	using namespace pure;
	s_her = {};
	memset(s_have, 0, sizeof(s_have));
	s_her.part[kHeadPart] = body;
	s_her.slots[kHeadPart] = kPart0Slots;
	s_her.parts = 1;
	Learn(*body, kPart0Slots);
	for (bool grew = true; grew;) {
		grew = false;
		for (int32_t i = 0; i < s_copyCount; i++) {
			const ListCopy& copy = s_copies[i];
			if (Assigned(&copy)) continue;
			for (int32_t part = 1; part < kBodyPartCount; part++) {
				if (s_her.part[part] || copy.bones != kBodyPartBones[part]) continue;
				const uint8_t* slots = kPartSlots[part].bones;
				if (!Fits(copy, slots) && part == 13 && Fits(copy, kPart13SeriousSlots)) slots = kPart13SeriousSlots;
				if (!Fits(copy, slots)) continue;
				s_her.part[part] = &copy;
				s_her.slots[part] = slots;
				s_her.parts++;
				Learn(copy, slots);
				grew = true;
				break;
			}
		}
	}
	const float* head = s_known[BONE_HEAD];
	for (int32_t i = 0; i < s_copyCount && s_her.headOnlyCount < (int32_t)std::size(s_her.headOnly); i++) {
		const ListCopy& copy = s_copies[i];
		if (copy.bones > kMaxTinyBones || Assigned(&copy)) continue;
		if (RidesHerHead(copy, head)) s_her.headOnly[s_her.headOnlyCount++] = &copy;
	}
	s_her.found = s_her.parts >= kMinParts;
	if (s_her.found && plausible_bone(s_known[BONE_HIPS]) && plausible_bone(s_known[BONE_LEFT_ANKLE]) && plausible_bone(s_known[BONE_RIGHT_ANKLE])) {
		V3 hips = bone_point(s_known[BONE_HIPS], kSkeleton[BONE_HIPS].bind);
		s_feet = {{hips.x, ground_height(s_known[BONE_LEFT_ANKLE], s_known[BONE_RIGHT_ANKLE]), hips.z}, GetTickCount()};
	}
	if (s_her.found && s_her.headOnlyCount > 1 && !s_loggedHeadOnly) {
		s_loggedHeadOnly = true;
		Log("first person: %d of 2B's lists found by their bones, and %d tiny lists of her head and eye bones (her eyes): "
			"hidden like her head",
			s_her.parts, s_her.headOnlyCount);
	}
}

// Which of her parts a list is by its contents (pure::kHeadOnlyList: a tiny list of her head bone; -1:
// not hers, or her lists not found this frame).
int32_t HerPartOf(const void* list) {
	if (!s_her.found) return -1;
	for (int32_t part = 0; part < pure::kBodyPartCount; part++)
		if (s_her.part[part] && s_her.part[part]->list == list) return part;
	for (int32_t i = 0; i < s_her.headOnlyCount; i++)
		if (s_her.headOnly[i]->list == list) return pure::kHeadOnlyList;
	return -1;
}

// ---------------------------------------------------------------------------------------------
// Her body following your head
// ---------------------------------------------------------------------------------------------
bool s_uploading;  // our own upload of her posed lists
struct Viewer {
	bool valid;
	pure::M4 view;          // your head (between your eyes), camera-to-world
	pure::V3 fromHerEyes;   // ... from her eyes then (world)
	DWORD tick;
} s_viewer;
bool s_loggedBody, s_loggedArms;
struct Hands {
	bool valid[2];
	pure::M4 grip[2];  // world, at the same moment as s_viewer
} s_hands;
struct BodyReport {
	DWORD tick;
	uint32_t posed, offsetSet, notInEyes, noViewer, notFound;
	int32_t parts, headOnly;
	pure::BodyPoseInfo last;
} s_report;

void ReportBody() {
	if (GetTickCount() - s_report.tick < 30000) return;
	s_report.tick = GetTickCount();
	if (s_report.posed + s_report.offsetSet + s_report.notInEyes + s_report.noViewer + s_report.notFound)
		Log("first person body: last 30 s: posed %u frames (%d of her lists, %d head-only); not posed: %u with a camera offset "
			"(FirstPersonHeight / FirstPersonForward), %u not in her eyes, %u no headset pose, %u her lists not found. Last: your "
			"head %.2f m from hers, she slid %.2f m, her waist bent %.0f deg",
			s_report.posed, s_report.parts, s_report.headOnly, s_report.offsetSet, s_report.notInEyes, s_report.noViewer, s_report.notFound,
			(double)s_report.last.headDistance, (double)s_report.last.slide, (double)(s_report.last.bend * 57.29578f));
	s_report.posed = s_report.offsetSet = s_report.notInEyes = s_report.noViewer = s_report.notFound = 0;
}

void Upload(ID3D11DeviceContext* c, ID3D11Resource* list, const float* bones, int32_t count) {
	D3D11_MAPPED_SUBRESOURCE mapped;
	s_uploading = true;
	if (SUCCEEDED(c->Map(list, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		memcpy(mapped.pData, bones, (size_t)count * pure::kBoneBytes);
		c->Unmap(list, 0);
	}
	s_uploading = false;
}

// Before she is first drawn this frame: her bones moved so her upper body is at your head, and her
// lists uploaded again.
void PoseBody(ID3D11DeviceContext* c) {
	using namespace pure;
	if (!g_cfg.fpBody) return;
	ReportBody();
	if (g_cfg.fpHeight != 0 || g_cfg.fpForward != 0) {  // the camera isn't at her eyes: her body would chase it
		s_report.offsetSet++;
		return;
	}
	if (!s_wasHiding) {
		s_report.notInEyes++;
		return;
	}
	if (!s_viewer.valid || GetTickCount() - s_viewer.tick > kFreshMs) {
		s_report.noViewer++;
		return;
	}
	const float *waist = s_known[BONE_WAIST], *head = s_known[BONE_HEAD];
	if (!s_her.found || !plausible_bone(waist) || !plausible_bone(head)) {
		s_report.notFound++;
		return;
	}
	// your head from her eyes (a frame old), from where her eyes are now; at most kMaxReach away
	V3 fromHer = s_viewer.fromHerEyes;
	float reach = length(fromHer);
	if (reach > kMaxReach) fromHer = fromHer * (kMaxReach / reach);
	V3 eyeTarget = bone_point(head, kViewBind) + fromHer;
	const M4& view = s_viewer.view;
	static M4 delta[kSkeletonBones];
	pose_upper_body(waist, head, eyeTarget, view, delta, &s_report.last);
	if (g_cfg.fpArms) {
		// the controllers kept where they are from your head: your head is at eyeTarget now
		M4 headNow = view;
		headNow.at(0, 3) = eyeTarget.x;
		headNow.at(1, 3) = eyeTarget.y;
		headNow.at(2, 3) = eyeTarget.z;
		M4 fromViewer = m4_mul(headNow, m4_rigid_inverse(view));
		const ArmBones* arms[2] = {&kLeftArm, &kRightArm};
		for (int hand = 0; hand < 2; hand++) {
			const ArmBones& arm = *arms[hand];
			if (!s_hands.valid[hand] || !plausible_bone(s_known[arm.upper]) || !plausible_bone(s_known[arm.fore]) ||
				!plausible_bone(s_known[arm.hand]) || !plausible_bone(s_known[BONE_CHEST]))
				continue;
			pose_arm(arm, s_known[arm.upper], s_known[arm.fore], s_known[arm.hand], s_known[BONE_CHEST], m4_mul(fromViewer, s_hands.grip[hand]), delta);
			if (!s_loggedArms) {
				s_loggedArms = true;
				Log("first person: her arms follow your controllers");
			}
		}
	}
	s_report.posed++;
	s_report.parts = s_her.parts;
	s_report.headOnly = s_her.headOnlyCount;
	float posed[kMaxPartBones * 12];
	for (int32_t part = 0; part < kBodyPartCount; part++) {
		const ListCopy* copy = s_her.part[part];
		if (!copy) continue;
		for (int32_t slot = 0; slot < copy->bones; slot++) move_bone(delta[s_her.slots[part][slot]], copy->data + slot * 12, posed + slot * 12);
		Upload(c, copy->list, posed, copy->bones);
	}
	for (int32_t i = 0; i < s_her.headOnlyCount; i++) {
		const ListCopy* copy = s_her.headOnly[i];
		for (int32_t slot = 0; slot < copy->bones; slot++) move_bone(delta[BONE_HEAD], copy->data + slot * 12, posed + slot * 12);
		Upload(c, copy->list, posed, copy->bones);
	}
	if (!s_loggedBody) {
		s_loggedBody = true;
		Log("first person: her upper body follows your head");
	}
}

UINT TextureWidthHeight(ID3D11View* view, UINT* height) {
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return 0;
	D3D11_TEXTURE2D_DESC desc;
	((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
	*height = desc.Height;
	return desc.Width;
}
}  // namespace

void FirstPersonNewFrame() {
	s_frame++;
	s_wasHiding = s_hiding;
	s_walker = {};
	s_walkHead = s_walkFace = nullptr;
	s_boundPart = -1;
	s_copyCount = s_partCopies = s_tinyCopies = 0;
	s_her = {};
}

void FirstPersonOnSetVSConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT count, ID3D11Buffer* const* buffers) {
	if (!g_cfg.firstPerson || kSkinningSlot < start || kSkinningSlot >= start + count) return;
	ID3D11Buffer* list = buffers[kSkinningSlot - start];
	s_boundPart = -1;
	if (!list) return;
	D3D11_BUFFER_DESC desc;
	list->GetDesc(&desc);
	if (desc.ByteWidth % pure::kBoneBytes != 0) return;  // another kind of buffer (worldMtxBuffer, instancing)
	int32_t bones = (int32_t)(desc.ByteWidth / pure::kBoneBytes);
	if (!s_her.found && bones == pure::kBodyPartBones[pure::kHeadPart]) {  // her body? (all her uploads are done by now)
		const ListCopy* body = CopyOf(list);
		if (body && body->bones == bones) {
			FindHer(body);
			if (s_her.found) PoseBody(c);
		}
	}
	Label* label = LabelOf(list);
	if (!label) return;
	if (label->frame != s_frame) Identify(label, list, bones);
	int32_t byContents = HerPartOf(list);  // known from her first part on (the walk confirms her only at her third)
	s_boundPart = byContents >= 0 ? byContents : (label->hers ? label->part : -1);
}

void FirstPersonOnMap(ID3D11Resource* res, void* data, UINT byteWidth) {
	if (!g_cfg.firstPerson || !data || s_uploading || byteWidth % pure::kBoneBytes || !CouldBeHers((int32_t)(byteWidth / pure::kBoneBytes))) return;
	PendingMap* slot = nullptr;
	for (auto& pending : s_pending)
		if (pending.list == res || (!pending.list && !slot)) slot = &pending;
	if (!slot) {  // a map never unmapped: its place is taken
		slot = &s_pending[s_pendingNext];
		s_pendingNext = (s_pendingNext + 1) % (int32_t)std::size(s_pending);
	}
	*slot = {res, (float*)data, byteWidth};
}

void FirstPersonBeforeUnmap(ID3D11Resource* res) {
	if (s_uploading) return;
	for (auto& pending : s_pending) {
		if (pending.list != res) continue;
		TakeCopy(res, pending.data, (int32_t)(pending.byteWidth / pure::kBoneBytes));
		pending.list = nullptr;
		return;
	}
}

void FirstPersonOnSetRenderTargets(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
	if (!g_cfg.firstPerson) return;
	ID3D11View* view = count && rtvs && rtvs[0] ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
	UINT height = 0, width = view ? TextureWidthHeight(view, &height) : 0;
	s_mainTarget = width && width == s_gameW && height == s_gameH;
}

bool FirstPersonSkipDraw() {
	if (!s_hiding || g_cfg.fpShowHead || !s_mainTarget || s_boundPart < 0 || !pure::is_head_only_part(s_boundPart)) return false;
	if (!s_loggedHiding) {
		s_loggedHiding = true;
		Log("first person: hiding 2B's head in the main view (her shadow keeps it)");
	}
	return true;
}

void FirstPersonSetGameSize(UINT width, UINT height) {
	s_gameW = width;
	s_gameH = height;
}

bool FirstPersonHead(pure::HeadPose* out) {
	if (!s_tracked.valid || GetTickCount() - s_tracked.tick > kFreshMs) return false;
	*out = s_tracked.head;
	return true;
}

bool FirstPersonSteadyEyeHeight(float* out) {
	if (!s_tracked.groundValid || GetTickCount() - s_tracked.groundTick > kFreshMs) return false;
	*out = s_tracked.steadyEyeY;
	return true;
}

void FirstPersonSetHiding(bool hide) { s_hiding = hide; }

void FirstPersonSetHands(const bool valid[2], const pure::M4 grips[2]) {
	for (int hand = 0; hand < 2; hand++) {
		s_hands.valid[hand] = valid[hand];
		if (valid[hand]) s_hands.grip[hand] = grips[hand];
	}
}

bool FirstPersonFeet(pure::V3* out) {
	if (!s_feet.tick || GetTickCount() - s_feet.tick > kFreshMs) return false;
	*out = s_feet.at;
	return true;
}

void FirstPersonSetViewer(const pure::M4& view, pure::V3 fromHerEyes) { s_viewer = {true, view, fromHerEyes, GetTickCount()}; }
