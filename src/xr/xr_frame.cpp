// xr_frame.cpp - one headset frame: hotkeys, screen placement, the eye poses handed to the game,
// and the layers submitted (stereo eyes, flat screen, HUD, stats overlay, settings menu).
#include "xr/xr_thread.h"
#include "core/config.h"
#include "core/log.h"
#include "render/cull.h"
#include "render/replay.h"
#include "render/stereo.h"
#include "render/upscale.h"
#include "ui/menu.h"
#include "xr/vrinput.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Screen placement & hotkeys
// ---------------------------------------------------------------------------------------------
bool GameIsForeground() {
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

bool KeyPressed(int vk) {
	if (vk <= 0 || vk > 255) return false;
	bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool pressed = down && !g_vr.keyDown[vk];
	g_vr.keyDown[vk] = down;
	return pressed;
}

static void HandleHotkeys() {
	bool fg = GameIsForeground();
	bool changed = false;
	// Always poll so edge detection stays correct, only act while the game has focus.
	bool rc = KeyPressed(g_cfg.keyRecenter), sm = KeyPressed(g_cfg.keySmaller), lg = KeyPressed(g_cfg.keyLarger),
		 cl = KeyPressed(g_cfg.keyCloser), fa = KeyPressed(g_cfg.keyFarther), st = KeyPressed(g_cfg.keyStereo),
		 f11 = KeyPressed(VK_F11), f5 = KeyPressed(VK_F5), fp = KeyPressed(g_cfg.keyFirstPerson);
	if (!fg) return;
	if (fp) {
		g_cfg.firstPerson = !g_cfg.firstPerson;
		Log("first person %s", g_cfg.firstPerson ? "ON (prototype)" : "OFF");
		ConfigSave(&g_cfg.firstPerson);
	}
	if (f5) {
		bool c = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, s = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		if (c && s) CullingToggleHeadCulling();  // Ctrl+Shift+F5: head-direction culling on/off
		else if (c) CullingWatch();               // Ctrl+F5: diagnostic watch
		else if (s) CullingCycleTest();           // Shift+F5: wide-fov test steps
		else {                                    // F5: both eyes every frame on/off
			g_cfg.bothEyes = !g_cfg.bothEyes;
			Log("both eyes every frame %s", !g_cfg.bothEyes     ? "OFF (alternate eyes)"
											: ReplayAvailable() ? "ON (the game's frame is drawn again for the right eye)"
																: "ON, but not available (see the log above)");
			ConfigSave(&g_cfg.bothEyes);
		}
	}
	if (f11 && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
			// Ctrl+Shift+F11: shadows on/off (test switch)
			g_cfg.shadows = !g_cfg.shadows;
			Log("shadows %s", g_cfg.shadows ? "ON" : "OFF");
		} else {
			// Ctrl+F11: NieR's ambient occlusion on/off (half-resolution SAO looks blotchy up close)
			g_cfg.ambientOcclusion = !g_cfg.ambientOcclusion;
			Log("ambient occlusion %s", g_cfg.ambientOcclusion ? "ON" : "OFF");
		}
		ConfigSave(&g_cfg.shadows);
		ConfigSave(&g_cfg.ambientOcclusion);
	}
	if (st && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
		// Shift+F12: TopDownFlat on/off (steep top-down views on the flat screen)
		g_cfg.topDownFlat = !g_cfg.topDownFlat;
		Log("top-down views: %s", g_cfg.topDownFlat ? "flat screen (TopDownFlat=1)" : "stereo (TopDownFlat=0)");
		ConfigSave(&g_cfg.topDownFlat);
		st = false;
	}
	if (st && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
		// Ctrl+F12: diagnostic single-eye mode (the left eye's image every frame, shown to both
		// eyes: no 3D, but also no alternating between eyes)
		AcquireSRWLockExclusive(&g_stereo.lock);
		g_stereo.singleEye = !g_stereo.singleEye;
		bool on = g_stereo.singleEye;
		ReleaseSRWLockExclusive(&g_stereo.lock);
		Log("single-eye test %s", on ? "ON (left eye every frame, no 3D)" : "OFF");
		st = false;
	}
	if (st) {
		AcquireSRWLockExclusive(&g_stereo.lock);
		g_stereo.enabled = !g_stereo.enabled;
		bool on = g_stereo.enabled;
		ReleaseSRWLockExclusive(&g_stereo.lock);
		Log("stereo %s", on ? "ON" : "OFF (flat screen)");
	}
	if (rc) {
		g_vr.recenterRequested = true;
		Log("recenter requested");
	}
	bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	if (ctrl && (sm || lg)) {
		// Ctrl+F9: world bigger (less eye separation per game unit), Ctrl+F10: world smaller
		// (10% per press: 0.3.1/0.3.2 used 2%, too small a step to see)
		float f = sm ? 1 / 1.1f : 1.1f;
		float v = g_cfg.worldScale * f;
		g_cfg.worldScale = v < 0.002f ? 0.002f : (v > 50 ? 50 : v);
		Log("world scale %.3f (the world now looks %.2fx its true size)", (double)g_cfg.worldScale,
			(double)(1 / g_cfg.worldScale));
		ConfigSave(&g_cfg.worldScale);
		sm = lg = false;
	}
	if (ctrl && (cl || fa)) {
		// Ctrl+F6: camera closer to the character, Ctrl+F7: farther
		float v = g_cfg.cameraDolly + (cl ? 0.25f : -0.25f);
		g_cfg.cameraDolly = v < -5 ? -5 : (v > 5 ? 5 : v);
		Log("camera dolly %.2f m", (double)g_cfg.cameraDolly);
		ConfigSave(&g_cfg.cameraDolly);
		cl = fa = false;
	}
	if (sm) {
		g_cfg.width = g_cfg.width / 1.1f < 0.2f ? 0.2f : g_cfg.width / 1.1f;
		changed = true;
	}
	if (lg) {
		g_cfg.width = g_cfg.width * 1.1f > 100.f ? 100.f : g_cfg.width * 1.1f;
		changed = true;
	}
	if (cl) {
		g_cfg.distance = g_cfg.distance - 0.25f < 0.3f ? 0.3f : g_cfg.distance - 0.25f;
		changed = true;
		g_vr.recenterRequested = true;
	}
	if (fa) {
		g_cfg.distance = g_cfg.distance + 0.25f > 50.f ? 50.f : g_cfg.distance + 0.25f;
		changed = true;
		g_vr.recenterRequested = true;
	}
	if (changed) {
		Log("screen: distance %.2f m, width %.2f m", (double)g_cfg.distance, (double)g_cfg.width);
		ConfigSave(&g_cfg.distance);
		ConfigSave(&g_cfg.width);
	}
}

// The runtime may change its tracking space without the REFERENCE_SPACE_CHANGE_PENDING event the
// spec asks for (0.5.2 log, Virtual Desktop through SteamVR: the space went from upside down to normal
// after a dashboard visit; the old correction then turned the picture upside down until a manual
// recenter). Each headset frame: a head whose up points against the reference's up for a third of a
// second, or a head position jump of more than 0.75 m between two frames, re-levels everything.
static void WatchSpace() {
	if (!g_vr.placed || !g_vr.dispValid) {
		g_vr.lastHeadValid = false;
		return;
	}
	const XrPosef& e0 = g_vr.disp[0].pose;
	const XrPosef& e1 = g_vr.disp[1].pose;
	float head[3] = {(e0.position.x + e1.position.x) * 0.5f, (e0.position.y + e1.position.y) * 0.5f,
					 (e0.position.z + e1.position.z) * 0.5f};
	pure::V3 up = pure::rotate({e0.orientation.x, e0.orientation.y, e0.orientation.z, e0.orientation.w}, {0, 1, 0});
	float along = g_vr.spaceFlipped ? -up.y : up.y;  // head up against the reference's up
	g_vr.upsideFrames = along < -0.5f ? g_vr.upsideFrames + 1 : 0;
	bool jump = false;
	if (g_vr.lastHeadValid) {
		float dx = head[0] - g_vr.lastHead[0], dy = head[1] - g_vr.lastHead[1], dz = head[2] - g_vr.lastHead[2];
		jump = dx * dx + dy * dy + dz * dz > 0.75f * 0.75f;
	}
	memcpy(g_vr.lastHead, head, sizeof(head));
	g_vr.lastHeadValid = true;
	if (g_vr.upsideFrames >= 30 || jump) {
		Log("tracking space changed without notice (%s); re-levelling the view and the screen",
			jump ? "the head jumped more than 0.75 m in one frame" : "the head's up now points the other way");
		g_vr.upsideFrames = 0;
		g_vr.recenterRequested = true;
	}
}

static void PlaceScreen(XrTime time) {
	XrSpaceLocation loc = {};
	loc.type = XR_TYPE_SPACE_LOCATION;
	if (XR_FAILED(g_vr.xrLocateSpace(g_vr.view, g_vr.local, time, &loc))) return;
	const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if ((loc.locationFlags & need) != need) return;
	pure::Q head{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
	pure::Q yaw;  // level reference: heading only, upright for the player (pure::level_reference)
	bool wasFlipped = g_vr.spaceFlipped;
	if (!pure::level_reference(head, &g_vr.spaceFlipped, &yaw)) yaw = g_vr.placed ? g_vr.lastYaw : pure::Q{0, 0, 0, 1};
	if (g_vr.spaceFlipped != wasFlipped || (!g_vr.placed && g_vr.spaceFlipped))
		Log(g_vr.spaceFlipped ? "tracking space: the runtime's up axis points down (its +Y is the floor); the view, screen "
								"and menu are turned upright for you"
							  : "tracking space: normal (+Y up)");
	pure::V3 off = pure::rotate(yaw, {0, g_cfg.heightOffset, -g_cfg.distance});
	g_vr.quadPose.orientation = {yaw.x, yaw.y, yaw.z, yaw.w};
	g_vr.quadPose.position = {loc.pose.position.x + off.x, loc.pose.position.y + off.y, loc.pose.position.z + off.z};
	g_vr.lastYaw = yaw;
	g_vr.placed = true;
	g_vr.recenterRequested = false;
	// The stereo camera treats this (yaw-only) head pose as "looking along the game camera".
	AcquireSRWLockExclusive(&g_stereo.lock);
	g_stereo.refPos[0] = loc.pose.position.x;
	g_stereo.refPos[1] = loc.pose.position.y;
	g_stereo.refPos[2] = loc.pose.position.z;
	g_stereo.refRot[0] = yaw.x;
	g_stereo.refRot[1] = yaw.y;
	g_stereo.refRot[2] = yaw.z;
	g_stereo.refRot[3] = yaw.w;
	g_stereo.refValid = true;
	ReleaseSRWLockExclusive(&g_stereo.lock);
	pure::V3 hu = pure::rotate(head, {0, 1, 0});
	Log("screen placed at (%.2f, %.2f, %.2f), %.2f m ahead; head up vector (%.2f, %.2f, %.2f)%s",
		(double)g_vr.quadPose.position.x, (double)g_vr.quadPose.position.y, (double)g_vr.quadPose.position.z,
		(double)g_cfg.distance, (double)hu.x, (double)hu.y, (double)hu.z, g_vr.spaceFlipped ? " (space flipped)" : "");
}

static void LogStats() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	double secs = (double)(now.QuadPart - g_vr.statStart.QuadPart) / (double)g_vr.qpcFreq.QuadPart;
	if (secs < 10) return;
	unsigned got = g_vr.statGameFrames[0] + g_vr.statGameFrames[1] + g_vr.statGameFrames[2];
	Log("stats: headset %.1f fps (stereo %.1f, flat %.1f); game frames received: left %.1f, right %.1f, mono %.1f fps; "
		"HUD-free image in %.0f%% of them; UI coverage avg %.0f%% max %.0f%%",
		g_vr.statFrames / secs, g_vr.statStereoFrames / secs, g_vr.statQuadFrames / secs, g_vr.statGameFrames[0] / secs,
		g_vr.statGameFrames[1] / secs, g_vr.statGameFrames[2] / secs, got ? 100.0 * g_vr.statSceneFrames / got : 0.0,
		g_vr.statCovN ? 100.0 * g_vr.statCovSum / g_vr.statCovN : 0.0, 100.0 * g_vr.statCovMax);
	{
		char info[160];
		snprintf(info, sizeof(info), "Mode: %s", !g_vr.statStereoFrames ? "flat screen" : g_cfg.bothEyes ? "stereo, both eyes every frame"
																										 : "stereo, alternate eyes");
		MenuSetInfo(0, info);
		snprintf(info, sizeof(info), "Headset %.0f fps; game images per second: left %.0f, right %.0f, flat %.0f",
				 g_vr.statFrames / secs, g_vr.statGameFrames[0] / secs, g_vr.statGameFrames[1] / secs, g_vr.statGameFrames[2] / secs);
		MenuSetInfo(1, info);
	}
	if (g_vr.statLagN || g_vr.statTurnN) {
		Log("stats: head pose: game images were on screen on average %.1f ms after the time their pose was predicted "
			"from (prediction now %.1f ms ahead, PosePrediction=%d); the head had turned on average %.2f deg (max %.1f) "
			"from the shown image's pose, beyond the rendered margin of %.1f deg in %.1f%% of headset frames",
			g_vr.statLagN ? g_vr.statLagSum / g_vr.statLagN : 0.0, (double)g_vr.predictNs / 1e6, g_cfg.posePrediction,
			g_vr.statTurnN ? g_vr.statTurnSum / g_vr.statTurnN : 0.0, (double)g_vr.statTurnMax, (double)g_vr.marginDeg,
			g_vr.statTurnN ? 100.0 * g_vr.statTurnOver / g_vr.statTurnN : 0.0);
		char info[160];
		snprintf(info, sizeof(info), "Head: images shown %.0f ms after their pose; predicting %.0f ms; past margin %.1f%%",
				 g_vr.statLagN ? g_vr.statLagSum / g_vr.statLagN : 0.0, (double)g_vr.predictNs / 1e6,
				 g_vr.statTurnN ? 100.0 * g_vr.statTurnOver / g_vr.statTurnN : 0.0);
		MenuSetInfo(13, info);
	}
	static UINT resLoggedW, resLoggedH;
	if (g_vr.resPct > 0 && g_vr.statStereoFrames && (resLoggedW != g_vr.contentW || resLoggedH != g_vr.contentH)) {
		resLoggedW = g_vr.contentW;
		resLoggedH = g_vr.contentH;
		Log("stats: resolution: the game renders %ux%u%s: %.0f%% of the sharpness the headset recommends across, %.0f%% "
			"up-down; about %ux%u matches it in both directions",
			g_vr.contentW, g_vr.contentH, g_vr.sceneW != g_vr.contentW ? " (upscaled)" : "", (double)g_vr.resPct, (double)g_vr.resPctH,
			g_vr.resSuggestW, g_vr.resSuggestH);
		char info[160];
		snprintf(info, sizeof(info), "Resolution %ux%u: %.0f%% / %.0f%% of recommended (across / up-down); ~%ux%u matches",
				 g_vr.contentW, g_vr.contentH, (double)g_vr.resPct, (double)g_vr.resPctH, g_vr.resSuggestW, g_vr.resSuggestH);
		MenuSetInfo(14, info);
	}
	g_vr.statLagSum = g_vr.statTurnSum = 0;
	g_vr.statLagN = g_vr.statTurnN = g_vr.statTurnOver = 0;
	g_vr.statTurnMax = 0;
	g_vr.statSceneFrames = 0;
	g_vr.statCovSum = g_vr.statCovMax = 0;
	g_vr.statCovN = 0;
	g_vr.statFrames = g_vr.statQuadFrames = g_vr.statStereoFrames = 0;
	g_vr.statGameFrames[0] = g_vr.statGameFrames[1] = g_vr.statGameFrames[2] = 0;
	g_vr.statStart = now;
}

// Hands the latest predicted eye poses and fields of view to the game thread.
static void UpdateEyePoses(XrTime time) {
	XrViewLocateInfo li = {};
	li.type = XR_TYPE_VIEW_LOCATE_INFO;
	li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	li.space = g_vr.local;
	// where the eyes are when this VR frame is shown (diagnostic: how far the shown image is off)
	{
		li.displayTime = time;
		XrViewState ds = {};
		ds.type = XR_TYPE_VIEW_STATE;
		XrView dv[2] = {};
		dv[0].type = dv[1].type = XR_TYPE_VIEW;
		uint32_t dn = 0;
		g_vr.dispValid = XR_SUCCEEDED(g_vr.xrLocateViews(g_vr.session, &li, &ds, 2, &dn, dv)) && dn == 2 &&
						 (ds.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
		if (g_vr.dispValid && (ds.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
			g_vr.disp[0] = dv[0];
			g_vr.disp[1] = dv[1];
			g_vr.dispHave = true;
		}
		if (g_vr.dispValid)
			for (int e = 0; e < 2; e++) {
				g_vr.dispRot[e][0] = dv[e].pose.orientation.x;
				g_vr.dispRot[e][1] = dv[e].pose.orientation.y;
				g_vr.dispRot[e][2] = dv[e].pose.orientation.z;
				g_vr.dispRot[e][3] = dv[e].pose.orientation.w;
			}
	}
	// the game renders with the pose predicted for when its frame will really be shown
	g_vr.predictNs = g_cfg.posePrediction && g_vr.lagHave ? (long long)(g_vr.lagMs * 1e6) : 0;
	li.displayTime = time + g_vr.predictNs;
	li.space = g_vr.local;
	XrViewState vs = {};
	vs.type = XR_TYPE_VIEW_STATE;
	XrView v[2] = {};
	v[0].type = v[1].type = XR_TYPE_VIEW;
	uint32_t n = 0;
	const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	bool ok = XR_SUCCEEDED(g_vr.xrLocateViews(g_vr.session, &li, &vs, 2, &n, v)) && n == 2 && (vs.viewStateFlags & need) == need;
	AcquireSRWLockExclusive(&g_stereo.lock);
	g_stereo.valid = ok;
	if (ok) g_stereo.poseBase = time;
	if (ok) {
		for (int e = 0; e < 2; e++) {
			g_stereo.eyePos[e][0] = v[e].pose.position.x;
			g_stereo.eyePos[e][1] = v[e].pose.position.y;
			g_stereo.eyePos[e][2] = v[e].pose.position.z;
			g_stereo.eyeRot[e][0] = v[e].pose.orientation.x;
			g_stereo.eyeRot[e][1] = v[e].pose.orientation.y;
			g_stereo.eyeRot[e][2] = v[e].pose.orientation.z;
			g_stereo.eyeRot[e][3] = v[e].pose.orientation.w;
			g_stereo.eyeTan[e][0] = pure::ftan(v[e].fov.angleLeft);
			g_stereo.eyeTan[e][1] = pure::ftan(v[e].fov.angleRight);
			g_stereo.eyeTan[e][2] = pure::ftan(v[e].fov.angleUp);
			g_stereo.eyeTan[e][3] = pure::ftan(v[e].fov.angleDown);
		}
	}
	ReleaseSRWLockExclusive(&g_stereo.lock);
	static bool logged = false;
	if (ok && !logged) {
		logged = true;
		for (int e = 0; e < 2; e++)
			Log("eye %d: fov left %.1f right %.1f up %.1f down %.1f deg, position (%.4f, %.4f, %.4f)", e,
				(double)(v[e].fov.angleLeft * 57.29578f), (double)(v[e].fov.angleRight * 57.29578f),
				(double)(v[e].fov.angleUp * 57.29578f), (double)(v[e].fov.angleDown * 57.29578f),
				(double)v[e].pose.position.x, (double)v[e].pose.position.y, (double)v[e].pose.position.z);
		float dx = v[1].pose.position.x - v[0].pose.position.x, dy = v[1].pose.position.y - v[0].pose.position.y,
			  dz = v[1].pose.position.z - v[0].pose.position.z;
		Log("IPD %.1f mm", (double)(pure::fsqrt(dx * dx + dy * dy + dz * dz) * 1000));
	}
}

static XrFovf FovFromTag(const FrameTag& t) {
	XrFovf f;
	f.angleLeft = -pure::fatan(t.tanHalfW);
	f.angleRight = pure::fatan(t.tanHalfW);
	f.angleUp = pure::fatan(t.tanHalfH);
	f.angleDown = -pure::fatan(t.tanHalfH);
	return f;
}

static bool UpdateMenu(XrTime time);
static bool UpdateOverlay();

void Frame() {
	XrFrameState fs = {};
	fs.type = XR_TYPE_FRAME_STATE;
	XrFrameWaitInfo wi = {};
	wi.type = XR_TYPE_FRAME_WAIT_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrWaitFrame(g_vr.session, &wi, &fs))) {
		Sleep(5);
		return;
	}
	XrFrameBeginInfo bi = {};
	bi.type = XR_TYPE_FRAME_BEGIN_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrBeginFrame(g_vr.session, &bi))) return;

	const XrCompositionLayerBaseHeader* layers[8];
	uint32_t layerCount = 0;
	XrCompositionLayerQuad quad = {}, hud = {}, menu = {}, overlay = {};
	XrCompositionLayerProjection proj = {};
	XrCompositionLayerProjectionView pv[2] = {};

	HandleHotkeys();
	VrInputSync(g_vr.state == XR_SESSION_STATE_FOCUSED, GameIsForeground());
	VrInputMotion(g_vr.local, g_vr.view, fs.predictedDisplayTime, g_vr.state == XR_SESSION_STATE_FOCUSED, g_vr.spaceFlipped);
	MenuTick(GameIsForeground());
	if (MenuTakeRecenter()) g_vr.recenterRequested = true;
	UpdateEyePoses(fs.predictedDisplayTime);
	WatchSpace(); // TODO: i dont think this function works perfectly
	if (fs.shouldRender) {
		if (!g_vr.placed || g_vr.recenterRequested) PlaceScreen(fs.predictedDisplayTime);
		UpdateContent();
		MeasureUICoverage();
		AcquireSRWLockShared(&g_stereo.lock);
		bool stereoOn = g_stereo.enabled;
		ReleaseSRWLockShared(&g_stereo.lock);
		auto& L = g_vr.views[0];
		AcquireSRWLockShared(&g_stereo.lock);
		bool single = g_stereo.singleEye;
		ReleaseSRWLockShared(&g_stereo.lock);
		auto& R = single ? g_vr.views[0] : g_vr.views[1];  // single-eye test: the left image for both eyes
		auto& M = g_vr.views[2];
		// Stereo when both eyes have rendered recently and are newer than any flat frame.
		bool stereo = stereoOn && !g_vr.menuMode && L.has && R.has && L.tag.patched && R.tag.patched && L.seq > M.seq && R.seq > M.seq;
		// Late warp: both eyes are submitted with the headset's own pose pair and field
		// of view for this frame, each image re-aimed to it here. Runtimes that stream to the headset
		// (Pico Connect, Steam Link, ...) assume one head pose per frame and images that cover exactly
		// the eye's field of view: 0.5.0 sent each eye with its render pose (different moments in
		// alternate-eye mode: the right eye stuttered) and a symmetric, wider field of view (Quest 3,
		// 54/40 deg per eye: squeezed sideways, cross-eyed).
		bool late = stereo && g_cfg.lateWarp && g_vr.psWarp && g_vr.dispHave;
		// the image the eyes show: the scene-only one (upscaled or not) when both eyes have it
		const bool scn = L.tag.hasScene && R.tag.hasScene;
		const UINT imgW = scn ? g_vr.sceneW : g_vr.contentW, imgH = scn ? g_vr.sceneH : g_vr.contentH;
		uint32_t ew[2] = {imgW, imgW}, eh[2] = {imgH, imgH};
		if (late)
			for (int e = 0; e < 2; e++) {
				// same pixel density as the rendered image, over the eye's own field of view
				const FrameTag& t = (e == 0 ? L : R).tag;
				const XrFovf& f = g_vr.disp[e].fov;
				float w = (pure::ftan(f.angleRight) - pure::ftan(f.angleLeft)) * imgW / (2 * t.tanHalfW);
				float h = (pure::ftan(f.angleUp) - pure::ftan(f.angleDown)) * imgH / (2 * t.tanHalfH);
				ew[e] = ((uint32_t)w + 1) & ~1u;
				eh[e] = ((uint32_t)h + 1) & ~1u;
				// a few pixels' change doesn't need a new swapchain
				if (g_vr.eyeSwap[e].sc && ew[e] + 16 > g_vr.eyeSwap[e].w && ew[e] < g_vr.eyeSwap[e].w + 16 && eh[e] + 16 > g_vr.eyeSwap[e].h &&
					eh[e] < g_vr.eyeSwap[e].h + 16) {
					ew[e] = g_vr.eyeSwap[e].w;
					eh[e] = g_vr.eyeSwap[e].h;
				}
			}
		stereo = stereo && EnsureSwap(g_vr.eyeSwap[0], ew[0], eh[0], "left eye") && EnsureSwap(g_vr.eyeSwap[1], ew[1], eh[1], "right eye");
		if (stereo) {
			bool ok = true;
			bool bothScene = L.tag.hasScene && R.tag.hasScene;
			for (int e = 0; e < 2 && ok; e++) {
				auto& V = e == 0 ? L : R;
				const FrameTag& t = V.tag;
				// the 3D scene without HUD/menus when both eyes have one (never mix the two kinds:
				// that would show the UI in one eye and on the HUD layer at the same time)
				ID3D11ShaderResourceView* img = bothScene ? V.sceneSrv : V.srv;
				pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				if (late) {
					const XrView& d = g_vr.disp[e];
					WarpParams w;
					w.outTan[0] = pure::ftan(d.fov.angleLeft);
					w.outTan[1] = pure::ftan(d.fov.angleRight);
					w.outTan[2] = pure::ftan(d.fov.angleUp);
					w.outTan[3] = pure::ftan(d.fov.angleDown);
					w.srcTan[0] = t.tanHalfW;
					w.srcTan[1] = t.tanHalfH;
					w.srcTan[2] = w.srcTan[3] = 0;
					pure::M4 rr = pure::m4_from_pose({t.rot[0], t.rot[1], t.rot[2], t.rot[3]}, {0, 0, 0});
					pure::M4 rd = pure::m4_from_pose({d.pose.orientation.x, d.pose.orientation.y, d.pose.orientation.z,
													  d.pose.orientation.w},
													 {0, 0, 0});
					for (int r = 0; r < 3; r++) {
						for (int c = 0; c < 3; c++)
							w.m[r][c] = rr.at(0, r) * rd.at(0, c) + rr.at(1, r) * rd.at(1, c) + rr.at(2, r) * rd.at(2, c);
						w.m[r][3] = 0;
					}
					ok = RenderInto(g_vr.eyeSwap[e], img, nullptr, &w);
					pv[e].pose = d.pose;
					pv[e].fov = d.fov;
				} else {
					ok = RenderInto(g_vr.eyeSwap[e], img);
					pv[e].pose.position = {t.pos[0], t.pos[1], t.pos[2]};
					pv[e].pose.orientation = {t.rot[0], t.rot[1], t.rot[2], t.rot[3]};
					pv[e].fov = FovFromTag(t);
				}
				pv[e].subImage.swapchain = g_vr.eyeSwap[e].sc;
				pv[e].subImage.imageRect.offset = {0, 0};
				pv[e].subImage.imageRect.extent = {(int32_t)g_vr.eyeSwap[e].w, (int32_t)g_vr.eyeSwap[e].h};
			}
			// resolution hint: the game picture's pixel density against the headset's recommended one
			if (g_vr.recW && g_vr.dispHave) {
				const XrFovf& f = g_vr.disp[0].fov;
				float span = pure::ftan(f.angleRight) - pure::ftan(f.angleLeft);
				float need = g_vr.recW / span * 2 * L.tag.tanHalfW;  // game width with the recommended density
				float spanH = pure::ftan(f.angleUp) - pure::ftan(f.angleDown);
				float needH = g_vr.recH / spanH * 2 * L.tag.tanHalfH;
				if (need > 64 && needH > 64) {
					// the 16:9 picture is stretched over the eye's field of view (Anamorphic): the two
					// directions can differ a lot (0.5.1 Quest log: 144% across, 79% up-down)
					g_vr.resPct = 100.f * imgW / need;
					g_vr.resPctH = 100.f * imgH / needH;
					// keep the game's 16:9 shape: the larger of the two needs decides (the game's
					// resolution: when upscaling, the image shown is that much larger)
					float scale = need / imgW > needH / imgH ? need / imgW : needH / imgH;
					g_vr.resSuggestW = ((UINT)(g_vr.contentW * scale) + 4) & ~7u;
					g_vr.resSuggestH = ((UINT)(g_vr.contentH * scale) + 4) & ~7u;
					// the upscaler's output follows this: the game image's shape with the headset's detail
					float ns = need / g_vr.contentW > needH / g_vr.contentH ? need / g_vr.contentW : needH / g_vr.contentH;
					UINT nw = ((UINT)(g_vr.contentW * ns) + 1) & ~1u, nh = ((UINT)(g_vr.contentH * ns) + 1) & ~1u;
					EnterCriticalSection(&g_share.cs);
					g_share.nativeW = nw;
					g_share.nativeH = nh;
					LeaveCriticalSection(&g_share.cs);
				}
			}
			static int lateLogged = -1;
			if (ok && lateLogged != (int)late) {
				lateLogged = (int)late;
				if (late)
					Log("late warp: eyes submitted with the headset's pose and field of view (left %.1f/%.1f, up %.1f/%.1f "
						"deg), %ux%u per eye from a %ux%u image rendered %.1f x %.1f deg",
						(double)(g_vr.disp[0].fov.angleLeft * 57.29578f), (double)(g_vr.disp[0].fov.angleRight * 57.29578f),
						(double)(g_vr.disp[0].fov.angleUp * 57.29578f), (double)(g_vr.disp[0].fov.angleDown * 57.29578f),
						g_vr.eyeSwap[0].w, g_vr.eyeSwap[0].h, g_vr.contentW, g_vr.contentH,
						(double)(2 * pure::fatan(L.tag.tanHalfW) * 57.29578f), (double)(2 * pure::fatan(L.tag.tanHalfH) * 57.29578f));
				else
					Log("late warp off: eyes submitted with the pose and field of view they were rendered with");
			}
			// head pose prediction: how long after the pose's base time the images are on screen
			for (int e = 0; e < 2 && ok; e++) {
				const FrameTag& t = (e == 0 ? L : R).tag;
				if (!t.poseBase) continue;
				double ms = (double)(fs.predictedDisplayTime - t.poseBase) / 1e6;
				if (ms <= 0 || ms > 250) continue;
				g_vr.lagMs = g_vr.lagHave ? g_vr.lagMs + (ms - g_vr.lagMs) * 0.01 : ms;
				if (g_vr.lagMs > 120) g_vr.lagMs = 120;  // runtimes extrapolate the head only so far
				g_vr.lagHave = true;
				g_vr.statLagSum += ms;
				g_vr.statLagN++;
			}
			// how far the head has turned from the shown image's pose (the black edge, if beyond the margin)
			if (ok && g_vr.dispValid) {
				const float* r = L.tag.rot;
				float deg = pure::q_angle_deg({r[0], r[1], r[2], r[3]},
											  {g_vr.dispRot[0][0], g_vr.dispRot[0][1], g_vr.dispRot[0][2], g_vr.dispRot[0][3]});
				g_vr.statTurnSum += deg;
				g_vr.statTurnN++;
				if (deg > g_vr.statTurnMax) g_vr.statTurnMax = deg;
				// margin: rendered half-width beyond the headset's own field of view (horizontal)
				AcquireSRWLockShared(&g_stereo.lock);
				float own = -g_stereo.eyeTan[0][0] > g_stereo.eyeTan[0][1] ? -g_stereo.eyeTan[0][0] : g_stereo.eyeTan[0][1];
				ReleaseSRWLockShared(&g_stereo.lock);
				g_vr.marginDeg = (pure::fatan(L.tag.tanHalfW) - pure::fatan(own)) * 57.29578f;
				if (deg > g_vr.marginDeg) g_vr.statTurnOver++;
			}
			if (ok) {
				proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
				proj.space = g_vr.local;
				proj.viewCount = 2;
				proj.views = pv;
				layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
				g_vr.statStereoFrames++;
				// HUD: keyed out of the newest eye frame, on a transparent flat layer in front.
				// (BothEyes: the right eye is replayed without UI, so the HUD comes from the left)
				auto& H = (L.seq > R.seq && !L.tag.noUI) || R.tag.noUI ? L : R;
				if (bothScene && g_vr.placed && EnsureSwap(g_vr.hudSwap, g_vr.contentW, g_vr.contentH, "HUD") &&
					RenderInto(g_vr.hudSwap, H.srv, KeySrv(H))) {
					hud.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
					hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
					hud.space = g_vr.local;
					hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
					hud.subImage.swapchain = g_vr.hudSwap.sc;
					hud.subImage.imageRect.offset = {0, 0};
					hud.subImage.imageRect.extent = {(int32_t)g_vr.hudSwap.w, (int32_t)g_vr.hudSwap.h};
					hud.pose = g_vr.quadPose;
					hud.size = {g_cfg.width, g_cfg.width * (float)g_vr.contentH / (float)g_vr.contentW};
					layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
				}
			}
		} else {
			// Flat screen: the most recent frame of any kind.
			int k = -1;
			for (int i = 0; i < 3; i++)
				if (g_vr.views[i].has && (k < 0 || g_vr.views[i].seq > g_vr.views[k].seq)) k = i;
			if (g_vr.placed && k >= 0 && EnsureSwap(g_vr.quadSwap, g_vr.contentW, g_vr.contentH, "screen") &&
				RenderInto(g_vr.quadSwap, g_vr.views[k].srv)) {
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.space = g_vr.local;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = g_vr.quadSwap.sc;
				quad.subImage.imageRect.offset = {0, 0};
				quad.subImage.imageRect.extent = {(int32_t)g_vr.quadSwap.w, (int32_t)g_vr.quadSwap.h};
				quad.pose = g_vr.quadPose;
				quad.size = {g_cfg.width, g_cfg.width * (float)g_vr.contentH / (float)g_vr.contentW};
				layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quad;
				g_vr.statQuadFrames++;
			}
		}
	}

	// the stats overlay: moves with your head (view space), upper left of your view
	if (!g_cfg.statsOverlay) g_vr.ovStart.QuadPart = 0;  // counting starts again when it is switched on
	if (fs.shouldRender && g_cfg.statsOverlay && UpdateOverlay()) {
		// 0.32 m wide at 0.8 m; elsewhere scaled so it keeps its size and place in your view
		const float k = g_cfg.overlayDistance / 0.8f;
		const float width = 0.32f * k;
		overlay.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		overlay.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // ImGui's output is premultiplied
		overlay.space = g_vr.view;
		overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		overlay.subImage.swapchain = g_vr.overlay.swap.sc;
		overlay.subImage.imageRect.offset = {0, 0};
		overlay.subImage.imageRect.extent = {(int32_t)g_vr.overlay.swap.w, (int32_t)g_vr.overlay.swap.h};
		overlay.pose.orientation = {0, 0, 0, 1};
		overlay.pose.position = {g_cfg.overlayX * k, g_cfg.overlayY * k, -g_cfg.overlayDistance};
		overlay.size = {width, width * (float)g_vr.overlay.swap.h / (float)g_vr.overlay.swap.w};
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&overlay;
	}

	// the settings menu, on top of everything
	if (fs.shouldRender && MenuIsOpen() && UpdateMenu(fs.predictedDisplayTime)) {
		menu.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		menu.space = g_vr.local;
		menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		menu.subImage.swapchain = g_vr.menu.swap.sc;
		menu.subImage.imageRect.offset = {0, 0};
		menu.subImage.imageRect.extent = {(int32_t)g_vr.menu.swap.w, (int32_t)g_vr.menu.swap.h};
		menu.pose = g_vr.menuPose;
		const float mw = 1.0f * g_cfg.menuDistance / 1.1f;  // 1 m wide at 1.1 m
		menu.size = {mw, mw * (float)g_vr.menu.swap.h / (float)g_vr.menu.swap.w};
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&menu;
	}

	XrFrameEndInfo ei = {};
	ei.type = XR_TYPE_FRAME_END_INFO;
	ei.displayTime = fs.predictedDisplayTime;
	ei.environmentBlendMode = g_vr.blend;
	ei.layerCount = layerCount;
	ei.layers = layerCount ? layers : nullptr;
	XR_CHECK_FRAME(g_vr.xrEndFrame(g_vr.session, &ei));
	g_vr.statFrames++;
	g_vr.ovFrames++;
	{  // headset frame time, for the overlay's graph
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (g_vr.ovLastFrame.QuadPart) {
			g_vr.ovFrameMs[g_vr.ovFrameNext] = (float)(1000.0 * (now.QuadPart - g_vr.ovLastFrame.QuadPart) / g_vr.qpcFreq.QuadPart);
			g_vr.ovFrameNext = (g_vr.ovFrameNext + 1) % (int)(sizeof(g_vr.ovFrameMs) / sizeof(g_vr.ovFrameMs[0]));
		}
		g_vr.ovLastFrame = now;
	}
	LogStats();
}

// Shows an ImGui panel (the menu, the stats overlay): draw() renders it into the panel's own render
// target, which is then copied into the swapchain. The render target uses the swapchain's format
// without sRGB, so the copy keeps ImGui's colours as they are (they are already sRGB values).
// True = the panel can be shown.
typedef bool (*PanelDraw)(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv, int w, int h);
static bool RenderPanel(Panel& p, int w, int h, const char* what, PanelDraw draw) {
	if (!EnsureSwap(p.swap, (uint32_t)w, (uint32_t)h, what, true)) return false;
	if (p.swap.w != (uint32_t)w || p.swap.h != (uint32_t)h) return false;  // runtime limits: not supported
	if (!p.tex) {
		DXGI_FORMAT f = (DXGI_FORMAT)g_vr.swapFormat;
		if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) f = DXGI_FORMAT_R8G8B8A8_UNORM;
		if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) f = DXGI_FORMAT_B8G8R8A8_UNORM;
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = (UINT)w;
		d.Height = (UINT)h;
		d.MipLevels = d.ArraySize = 1;
		d.Format = f;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (FAILED(g_vr.dev->CreateTexture2D(&d, nullptr, &p.tex)) || FAILED(g_vr.dev->CreateRenderTargetView(p.tex, nullptr, &p.rtv))) {
			SafeRelease(p.tex);
			return false;
		}
		p.shown = false;
	}
	if (!draw(g_vr.dev, g_vr.ctx, p.rtv, w, h)) return p.shown;
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {};
	ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrAcquireSwapchainImage(p.swap.sc, &ai, &idx))) return p.shown;
	XrSwapchainImageWaitInfo wi = {};
	wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	wi.timeout = XR_INFINITE_DURATION;
	if (!XR_CHECK_FRAME(g_vr.xrWaitSwapchainImage(p.swap.sc, &wi))) return p.shown;  // no release without a wait
	if (idx < p.swap.count) {
		ID3D11Resource* img = nullptr;
		p.swap.rtv[idx]->GetResource(&img);
		if (img) {
			g_vr.ctx->CopyResource(img, p.tex);
			img->Release();
		}
	}
	XrSwapchainImageReleaseInfo ri = {};
	ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK_FRAME(g_vr.xrReleaseSwapchainImage(p.swap.sc, &ri));
	p.shown = true;
	return true;
}

// The settings menu: placed in front of your head when it opens ([Screen] MenuDistance, which moves
// it at once when changed), drawn every headset frame while open (ImGui's navigation runs per frame).
static bool UpdateMenu(XrTime time) {
	if (MenuTakeJustOpened()) {
		XrSpaceLocation loc = {};
		loc.type = XR_TYPE_SPACE_LOCATION;
		if (XR_SUCCEEDED(g_vr.xrLocateSpace(g_vr.view, g_vr.local, time, &loc)) &&
			(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			pure::Q head{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
			pure::Q yaw;
			bool flipped = g_vr.spaceFlipped;
			if (!pure::level_reference(head, &flipped, &yaw)) yaw = g_vr.lastYaw;
			g_vr.menuYaw = yaw;
			g_vr.menuHead = loc.pose.position;
			g_vr.menuPlaced = true;
		}
	}
	if (g_vr.menuPlaced) {
		float k = g_cfg.menuDistance / 1.1f;
		pure::V3 off = pure::rotate(g_vr.menuYaw, {0, -0.12f * k, -g_cfg.menuDistance});
		g_vr.menuPose.orientation = {g_vr.menuYaw.x, g_vr.menuYaw.y, g_vr.menuYaw.z, g_vr.menuYaw.w};
		g_vr.menuPose.position = {g_vr.menuHead.x + off.x, g_vr.menuHead.y + off.y, g_vr.menuHead.z + off.z};
	}
	return RenderPanel(g_vr.menu, 1280, 960, "menu", MenuRender);
}

// The stats overlay: new numbers twice a second, redrawn ten times a second (the graph).
static bool UpdateOverlay() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!g_vr.ovStart.QuadPart) {
		g_vr.ovStart = now;
		g_vr.ovFrames = 0;
		g_vr.ovGameFrames[0] = g_vr.ovGameFrames[1] = g_vr.ovGameFrames[2] = 0;
		g_vr.ovDrawn.QuadPart = 0;
	}
	double secs = (double)(now.QuadPart - g_vr.ovStart.QuadPart) / (double)g_vr.qpcFreq.QuadPart;
	OverlayStats& st = g_vr.ovStats;
	if (secs >= 0.5) {
		AcquireSRWLockShared(&g_stereo.lock);
		bool stereoOn = g_stereo.enabled;
		ReleaseSRWLockShared(&g_stereo.lock);
		snprintf(g_vr.ovMode, sizeof(g_vr.ovMode), "%s",
				 !stereoOn || g_vr.menuMode ? "flat screen" : g_cfg.bothEyes ? "both eyes"
																			 : "alternate eyes");
		st.headsetFps = (float)(g_vr.ovFrames / secs);
		for (int i = 0; i < 3; i++) st.gameFps[i] = (float)(g_vr.ovGameFrames[i] / secs);
		st.gameW = g_vr.contentW;
		st.gameH = g_vr.contentH;
		st.sceneW = g_vr.sceneW;
		st.sceneH = g_vr.sceneH;
		UpscaleStatus(g_vr.ovUpscaler, sizeof(g_vr.ovUpscaler));
		st.sharpX = g_vr.resPct;
		st.sharpY = g_vr.resPctH;
		st.poseDelayMs = g_vr.lagHave ? (float)g_vr.lagMs : -1.0f;
		st.predictMs = (float)(g_vr.predictNs / 1e6);
		g_vr.ovFrames = 0;
		g_vr.ovGameFrames[0] = g_vr.ovGameFrames[1] = g_vr.ovGameFrames[2] = 0;
		g_vr.ovStart = now;
	}
	st.mode = g_vr.ovMode;
	st.upscaler = g_vr.ovUpscaler;
	st.frameMs = g_vr.ovFrameMs;
	st.frameCount = (int)(sizeof(g_vr.ovFrameMs) / sizeof(g_vr.ovFrameMs[0]));
	st.frameOffset = g_vr.ovFrameNext;
	bool redraw = !g_vr.overlay.shown || (double)(now.QuadPart - g_vr.ovDrawn.QuadPart) / (double)g_vr.qpcFreq.QuadPart >= 0.1;
	if (!redraw) return true;
	g_vr.ovDrawn = now;
	return RenderPanel(g_vr.overlay, 640, 330, "stats overlay",
					   [](ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv, int w, int h) {
						   return OverlayRender(dev, dc, rtv, w, h, g_vr.ovStats);
					   });
}
