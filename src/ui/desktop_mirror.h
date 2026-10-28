// desktop_mirror.h - the game's window in stereo: the left eye's 3D image filling the window, the game's UI unsqueezed on top, and the mod's menu and stats overlay (desktop_mirror.cpp).
#pragma once
#include <d3d11.h>

// After the left eye's frame went to the headset: builds the window's image from the frame (`backbuffer`,
// UI included) and its HUD-free `scene` (alpha: UI coverage)
void DesktopMirrorCompose(ID3D11DeviceContext* c, ID3D11Texture2D* backbuffer, ID3D11Texture2D* scene, HWND window);
// At the end of a stereo frame: the window shows the last image built (false: none yet, the frame is left as it is)
bool DesktopMirrorShow(ID3D11DeviceContext* c, ID3D11Texture2D* backbuffer);
