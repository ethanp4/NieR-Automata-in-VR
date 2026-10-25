- [ ] rewrite waggle controls

- [ ] cleanup everything really (in progress)

- [ ] "dlss not supported on this graphics card" message in overlay

- [ ] check every menu string 

- [ ] reprojection edges

- [ ] investigate vrinputsync vrinputmotion focused / foreground variables

- [ ] synchronized mode after a device switch: D3dHooksFollowDevice moves d3d_hooks and the frame capture to the presenting device's context, but replay's g_gameContext keeps the old one (ReplayInstall runs once), so probably nothing gets recorded

- [ ] capture ring race (frame_handoff.cpp PushToRing / xr_device.cpp UpdateContent): the game reuses a ring slot as soon as the ring comes round to it (4-8 game frames), without knowing whether the VR thread has finished copying it; a VR thread stalled that long could show a half-overwritten frame

- [ ] dlss "not supported on this graphics card" (dlss.cpp DlssInit, the "this is busted" TODO): the GetI() results are ignored, so if the Get call itself fails (wrong vtable slot, or NGX rejecting it) `avail` stays 0 and it reports "not supported"; log what GetI returns to tell "not supported" from "couldn't ask"

- [ ] performance

- [ ] camera angle in flying mode (very first section of the game) CONSTANTLY jumps around randomly



