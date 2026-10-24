// menu.h - the in-headset settings menu (menu.cpp).
#pragma once
#include <d3d11.h>

void MenuTick(bool foreground);
bool MenuRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h);
void MenuSetInfo(int line, const char* text);  // a line of the Info page
bool MenuIsOpen();
bool MenuTakeJustOpened();
bool MenuTakeRecenter();
