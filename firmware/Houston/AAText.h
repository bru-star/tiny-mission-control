#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "AAFonts.h"

// Anti-aliased text and shapes blended straight into the RGB565 framebuffer
// (240x240): 8-bit glyph masks, a 1-pixel coverage ramp on every curve and 4x4
// samples on straight edges. Text stays on whole pixels, which keeps the stems
// sharp at these sizes.
namespace AAText {

constexpr int W = 240, H = 240;

inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a) { // a: 0..255 of fg
    if (a == 0) return bg;
    if (a == 255) return fg;
    const uint32_t r = ((fg >> 11) * a + (bg >> 11) * (255 - a) + 127) / 255;
    const uint32_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a) + 127) / 255;
    const uint32_t b = ((fg & 31) * a + (bg & 31) * (255 - a) + 127) / 255;
    return static_cast<uint16_t>(r << 11 | g << 5 | b);
}

inline const AAFonts::Glyph &glyphOf(const AAFonts::Font &f, char ch) {
    uint8_t c = static_cast<uint8_t>(ch);
    if (c == 0xB0) c = 127;          // Latin-1 degree sign
    if (c < 32 || c > 127) c = '?';
    return f.glyphs[c - 32];
}

// Width in pixels of s; "\xB0" is the degree sign.
inline int width(const AAFonts::Font &f, const char *s) {
    int w = 0;
    for (; *s; ++s) w += glyphOf(f, *s).advance;
    return w;
}

inline void draw(uint16_t *fb, const AAFonts::Font &f, const char *s, int x, int baseline, uint16_t colour, int spacing = 0) {
    for (; *s; ++s) {
        const AAFonts::Glyph &g = glyphOf(f, *s);
        const uint8_t *mask = f.bits + g.offset;
        for (int j = 0; j < g.h; ++j) {
            const int py = baseline + g.y + j;
            if (py < 0 || py >= H) continue;
            for (int i = 0; i < g.w; ++i) {
                const int px = x + g.x + i;
                if (px < 0 || px >= W) continue;
                const uint8_t a = mask[j * g.w + i];
                if (a) fb[py * W + px] = blend(fb[py * W + px], colour, a);
            }
        }
        x += g.advance + spacing;
    }
}

inline void centered(uint16_t *fb, const AAFonts::Font &f, const char *s, int baseline, uint16_t colour, int cx = W / 2) {
    draw(fb, f, s, cx - width(f, s) / 2, baseline, colour);
}

inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

inline uint8_t alpha255(float coverage) { return static_cast<uint8_t>(clamp01(coverage) * 255 + 0.5f); }

// A 1-pixel coverage ramp centred on the ideal edge, measured at the pixel's centre.
inline float circleCoverage(int x, int y, float cx, float cy, float r) {
    const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
    return clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f);
}

// A filled circle, or the rows yMin..yMax of one. Rows are filled in one go; only the rim needs the ramp.
inline void disc(uint16_t *fb, float cx, float cy, float r, uint16_t colour, int yMin = 0, int yMax = H - 1) {
    const float in = r > 0.5f ? r - 0.5f : 0, out = r + 0.5f;
    int y0 = static_cast<int>(floorf(cy - out - 0.5f)), y1 = static_cast<int>(ceilf(cy + out - 0.5f));
    if (y0 < yMin) y0 = yMin;
    if (y1 > yMax) y1 = yMax;
    for (int y = y0; y <= y1; ++y) {
        const float dy = y + 0.5f - cy, ady = fabsf(dy);
        if (ady > out) continue;
        const float outX = sqrtf(out * out - dy * dy);
        int x0 = static_cast<int>(ceilf(cx - outX - 0.5f)), x1 = static_cast<int>(floorf(cx + outX - 0.5f));
        if (x0 < 0) x0 = 0;
        if (x1 > W - 1) x1 = W - 1;
        int fullL = 1, fullR = 0;
        if (ady <= in) {
            const float inX = sqrtf(in * in - dy * dy);
            fullL = static_cast<int>(ceilf(cx - inX - 0.5f));
            fullR = static_cast<int>(floorf(cx + inX - 0.5f));
        }
        for (int x = x0; x <= x1; ++x) {
            if (x >= fullL && x <= fullR) {
                fb[y * W + x] = colour;
            } else {
                const uint8_t a = alpha255(circleCoverage(x, y, cx, cy, r));
                if (a) fb[y * W + x] = blend(fb[y * W + x], colour, a);
            }
        }
    }
}

// One segment of a ring from angle a0 to a1 (radians, clockwise on screen from the right): the coverage
// between two radii by the same ramp, and the same ramp across both straight ends.
inline void arcSegment(uint16_t *fb, float cx, float cy, float r0, float r1, float a0, float a1, uint16_t colour) {
    const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
    const float xs[4] = {r0 * c0, r1 * c0, r0 * c1, r1 * c1}, ys[4] = {r0 * s0, r1 * s0, r0 * s1, r1 * s1};
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 4; ++i) {
        minx = xs[i] < minx ? xs[i] : minx; maxx = xs[i] > maxx ? xs[i] : maxx;
        miny = ys[i] < miny ? ys[i] : miny; maxy = ys[i] > maxy ? ys[i] : maxy;
    }
    int bx0 = static_cast<int>(floorf(cx + minx)) - 1, bx1 = static_cast<int>(ceilf(cx + maxx)) + 1;
    int by0 = static_cast<int>(floorf(cy + miny)) - 1, by1 = static_cast<int>(ceilf(cy + maxy)) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > W - 1) bx1 = W - 1;
    if (by1 > H - 1) by1 = H - 1;
    for (int y = by0; y <= by1; ++y)
        for (int x = bx0; x <= bx1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float e0 = dy * c0 - dx * s0 + 0.5f, e1 = dx * s1 - dy * c1 + 0.5f; // cheap, so before the square root
            if (e0 <= 0 || e1 <= 0) continue;
            const float d = sqrtf(dx * dx + dy * dy);
            const float radial = clamp01(clamp01(r1 - d + 0.5f) - clamp01(r0 - d + 0.5f));
            if (radial <= 0) continue;
            const uint8_t a = alpha255(radial * clamp01(e0) * clamp01(e1));
            if (a) fb[y * W + x] = blend(fb[y * W + x], colour, a);
        }
}

// A filled polygon (up to 8 points, even-odd rule) with anti-aliased edges: 4x4
// samples per pixel, by scanline. For each of the 4 sample rows of a pixel row the
// edge crossings are found once; a sample is inside when an odd number of crossings
// lie to its right. No division per sample, which matters on a CPU without an FPU.
inline void polygon(uint16_t *fb, const float *xs, const float *ys, int n, uint16_t colour) {
    if (n < 3 || n > 8) return;
    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    float slope[8];
    for (int i = 0, j = n - 1; i < n; j = i++) {
        minx = xs[i] < minx ? xs[i] : minx; maxx = xs[i] > maxx ? xs[i] : maxx;
        miny = ys[i] < miny ? ys[i] : miny; maxy = ys[i] > maxy ? ys[i] : maxy;
        slope[i] = ys[j] != ys[i] ? (xs[j] - xs[i]) / (ys[j] - ys[i]) : 0; // edge i -> j
    }
    int bx0 = static_cast<int>(minx) - 1, bx1 = static_cast<int>(maxx) + 1;
    if (bx0 < 0) bx0 = 0;
    if (bx1 > W - 1) bx1 = W - 1;
    if (bx1 < bx0 || bx1 - bx0 >= 128) return; // small shapes only
    uint8_t hits[128];
    for (int y = static_cast<int>(miny) - 1; y <= static_cast<int>(maxy) + 1; ++y) {
        if (y < 0 || y >= H) continue;
        memset(hits, 0, static_cast<size_t>(bx1 - bx0 + 1));
        bool any = false;
        for (int sy = 0; sy < 4; ++sy) {
            const float py = y + (sy + 0.5f) / 4;
            float cross[8];
            int nc = 0;
            for (int i = 0, j = n - 1; i < n; j = i++)
                if ((ys[i] > py) != (ys[j] > py)) cross[nc++] = xs[i] + (py - ys[i]) * slope[i];
            if (!nc) continue;
            for (int a = 1; a < nc; ++a) // insertion sort, nc <= 8
                for (int b = a; b > 0 && cross[b - 1] > cross[b]; --b) {
                    const float t = cross[b];
                    cross[b] = cross[b - 1];
                    cross[b - 1] = t;
                }
            for (int k = 0; k + 1 < nc; k += 2) { // inside spans [cross[k], cross[k+1])
                // sample sx of pixel x sits at x + (sx + 0.5) / 4: first and last sample inside
                int s0 = static_cast<int>(ceilf((cross[k] - 0.125f) * 4));
                int s1 = static_cast<int>(ceilf((cross[k + 1] - 0.125f) * 4)) - 1;
                if (s0 < bx0 * 4) s0 = bx0 * 4;
                if (s1 > bx1 * 4 + 3) s1 = bx1 * 4 + 3;
                for (int s = s0; s <= s1; ++s) ++hits[(s >> 2) - bx0];
                any |= s1 >= s0;
            }
        }
        if (!any) continue;
        for (int x = bx0; x <= bx1; ++x) {
            const uint8_t h = hits[x - bx0];
            if (h) fb[y * W + x] = h >= 16 ? colour : blend(fb[y * W + x], colour, static_cast<uint8_t>(h * 255 / 16));
        }
    }
}

} // namespace AAText
