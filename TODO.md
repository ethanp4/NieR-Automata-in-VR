rewrite waggle controls

cleanup everything really

"dlss not supported on this graphics card" message in overlay

check every menu string 

reprojection edges

investigate vrinputsync vrinputmotion focused / foreground variables

synchronized mode after a device switch: D3dHooksFollowDevice moves d3d_hooks and the frame capture to the presenting device's context, but replay's g_gameContext keeps the old one (ReplayInstall runs once), so probably nothing gets recorded