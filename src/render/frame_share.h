// frame_share.h - hand-off of finished game frames from the game's device to the VR thread's device
// (g_share is defined in frame_handoff.cpp). The VR thread owns the shared textures (created on its
// own device); the game thread asks for a size/format and opens whatever the VR thread publishes.
#pragma once
#include <d3d11.h>

struct FrameTag {  // what a captured game frame shows
	int eye;        // 0 left, 1 right, -1 mono
	bool patched;   // the camera was rewritten for that eye
	bool hasScene;  // a scene-only (no UI) image was captured too
	float pos[3], rot[4];  // eye pose (LOCAL) it was rendered from
	float tanHalfW, tanHalfH;
	unsigned pairId;     // Synchronized: left and right image of one game frame share it (0 = single)
	bool noUI;           // drawn without UI (the replayed second eye): don't key the HUD from it
	bool hasWorld;       // worldRot is valid (the scene camera was patched)
	float worldRot[4];   // game-world orientation of LOCAL space for this frame (eye sync)
	long long poseBase;  // StereoShared::poseBase of the pose it was rendered with (0 = unknown)
};

const int kRingSize = 8;  // capture ring slots (Synchronized pushes two images per game frame)

struct FrameShare {
	CRITICAL_SECTION cs;
	// request: written by the game thread (under cs)
	LONG reqGen;  // bumps whenever the request changes
	UINT reqW, reqH;
	UINT reqSceneW, reqSceneH;  // the scene-only images: larger than the frame when upscaling
	bool reqKey;                // upscaling: also the scene images before upscaling (ringKey)
	// written by the VR thread: the game-image size with the detail the headset recommends (0 = not
	// known yet): the upscaler's output follows it
	UINT nativeW, nativeH;
	DXGI_FORMAT reqFmt;
	// reply: written by the VR thread (under cs): a ring of shared textures
	HANDLE ring[kRingSize];       // legacy shared handles: full frames
	HANDLE ringScene[kRingSize];  // and scene-only (no UI) frames
	HANDLE ringKey[kRingSize];    // upscaling: the scene-only frames before upscaling (the HUD is cut out
								  // against these, which match the full frames pixel for pixel); else null
	LONG gen;        // bumps whenever a new ring is published
	LONG forReqGen;  // the request this ring answers
	UINT w, h;
	UINT sceneW, sceneH;
	DXGI_FORMAT fmt;
	// completed frames, published by the game thread (under cs) once the GPU finished the copy
	struct Published {
		LONG seq;  // 0 = none yet
		int slot;  // ring index
		FrameTag tag;
	} latest[3];  // [0] left eye, [1] right eye, [2] mono
	LONG seq;     // last sequence number handed out
	// GPU that runs the game
	LUID gameLuid;
	bool gameLuidValid;
};
extern FrameShare g_share;
