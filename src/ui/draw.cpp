// draw.cpp - CPU drawing for the flat panels (see draw.h).
#include "core/common.h"
#include "ui/draw.h"
#include "gen/font.h"

const Font kFontBody = {kBodyW, kBodyH, kBodyLine, kBodyAscent, kBodyGlyph, kBodyAtlas};
const Font kFontSmall = {kSmallW, kSmallH, kSmallLine, kSmallAscent, kSmallGlyph, kSmallAtlas};
const Font kFontTitle = {kTitleW, kTitleH, kTitleLine, kTitleAscent, kTitleGlyph, kTitleAtlas};

int TextWidth(const Font& f, const char* s) {
    int w = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        w += f.glyph[c - 32][6];
    }
    return w;
}

bool Canvas::Alloc(int width, int height) {
    if (px) return true;
    px = (uint32_t*)VirtualAlloc(nullptr, (SIZE_T)width * height * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!px) return false;
    w = width;
    h = height;
    return true;
}

uint32_t Canvas::Pack(Rgb c, int alpha) const {
    uint32_t a = (uint32_t)alpha << 24;
    return bgra ? (a | (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | (uint32_t)c.b)
                : (a | (uint32_t)c.b << 16 | (uint32_t)c.g << 8 | (uint32_t)c.r);
}

void Canvas::Fill(int x, int y, int fw, int fh, Rgb c, int alpha) {
    uint32_t v = Pack(c, alpha);
    for (int yy = y < 0 ? 0 : y; yy < y + fh && yy < h; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + fw && xx < w; xx++) px[yy * w + xx] = v;
}

void Canvas::Blend(int x, int y, Rgb c, int a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    uint32_t d = px[y * w + x];
    int dr = bgra ? (d >> 16) & 255 : d & 255, dg = (d >> 8) & 255, db = bgra ? d & 255 : (d >> 16) & 255;
    int da = (int)(d >> 24);
    Rgb o = {dr + (c.r - dr) * a / 255, dg + (c.g - dg) * a / 255, db + (c.b - db) * a / 255};
    px[y * w + x] = Pack(o, da + (255 - da) * a / 255);  // text on a see-through background gets more opaque
}

int Canvas::Text(const Font& f, int x, int y, const char* s, Rgb col, int maxW) {
    int x0 = x;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const short* g = f.glyph[c - 32];
        if (x + g[6] - x0 > maxW) break;
        for (int yy = 0; yy < g[3]; yy++)
            for (int xx = 0; xx < g[2]; xx++)
                Blend(x + g[4] + xx, y + g[5] + yy, col, f.atlas[(g[1] + yy) * f.w + g[0] + xx]);
        x += g[6];
    }
    return x - x0;
}

int Canvas::Wrapped(const Font& f, int x, int y, int maxW, const char* s, Rgb col, int maxLines) {
    char line[256];
    int lines = 0;
    while (*s && lines < maxLines) {
        int n = 0, lastSpace = -1;
        while (s[n] && n < 255) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (TextWidth(f, line) > maxW) break;
            if (s[n] == ' ') lastSpace = n;
            n++;
        }
        int take = (s[n] && lastSpace > 0) ? lastSpace : n;
        if (take <= 0) take = n > 0 ? n : 1;
        memcpy(line, s, take);
        line[take] = 0;
        Text(f, x, y, line, col);
        y += f.line;
        s += take;
        while (*s == ' ') s++;
        lines++;
    }
    return y;
}
