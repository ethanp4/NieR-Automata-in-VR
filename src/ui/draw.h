// draw.h - CPU drawing for the flat panels the VR thread shows (the settings menu, the stats overlay).
//
// A Canvas is an RGBA8 image in memory (sRGB-encoded, straight alpha); xr_main.cpp copies it into a
// swapchain and shows it on a quad layer. Text uses the fonts pre-rendered into src/gen/font.h.
#pragma once
#include <stdint.h>

struct Rgb {
    int r, g, b;
};

struct Font {
    int w, h, line, ascent;     // atlas size, line height, baseline below the line top
    const short (*glyph)[7];    // per character 32..126: x, y, w, h, xoff, yoff, advance
    const unsigned char* atlas; // coverage, 0..255
};
extern const Font kFontBody, kFontSmall, kFontTitle;

int TextWidth(const Font& f, const char* s);

struct Canvas {
    uint32_t* px;
    int w, h;
    bool bgra;  // byte order of the swapchain it goes to

    bool Alloc(int width, int height);  // once; false if out of memory

    // Sets every pixel of the rectangle (alpha 255 = opaque).
    void Fill(int x, int y, int fw, int fh, Rgb c, int alpha = 255);
    // Mixes c into one pixel by a (0..255).
    void Blend(int x, int y, Rgb c, int a);
    // One line of text, y = top of the line; stops before maxW. Returns the width drawn.
    int Text(const Font& f, int x, int y, const char* s, Rgb col, int maxW = 100000);
    // Word-wrapped text; returns the y after the last line.
    int Wrapped(const Font& f, int x, int y, int maxW, const char* s, Rgb col, int maxLines);

    uint32_t Pack(Rgb c, int alpha = 255) const;
};
