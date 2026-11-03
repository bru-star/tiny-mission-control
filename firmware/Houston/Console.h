#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "AAText.h"
#include "Panels.h"

// The mission-console face: everything on one screen, drawn into the 240x240 RGB565 framebuffer.
namespace Console {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
}
constexpr uint16_t BG = rgb(0x0A, 0x0B, 0x09), GREEN = rgb(0x39, 0xFF, 0x7A), GREEN_DIM = rgb(0x9D, 0xF5, 0xBC),
                   GREEN_CLOCK = rgb(0x2F, 0xB8, 0x66), GHOST = rgb(0x14, 0x2A, 0x1B), RING_OFF = rgb(0x1E, 0x2B, 0x22),
                   AMBER = rgb(0xFF, 0xB0, 0x00), RED = rgb(0xFF, 0x3B, 0x2F), LAMP_OFF = rgb(0x14, 0x16, 0x0F),
                   LAMP_TEXT = rgb(0x6E, 0x76, 0x64), INK_AMBER = rgb(0x1A, 0x10, 0x00), INK_GREEN = rgb(0x06, 0x20, 0x0F),
                   INK_RED = rgb(0x26, 0x03, 0x00), DATE = rgb(0x5E, 0x8A, 0x6D);

constexpr int UNKNOWN = -1;

struct Model {
    int phone = UNKNOWN, mail = UNKNOWN, claude = UNKNOWN; // UNKNOWN: the source has gone quiet
    bool printKnown = false, printing = false, printDone = false;
    int printPercent = 0, printMinutes = UNKNOWN;
    bool gaming = false;
    int gpuC = UNKNOWN;
    int hour = UNKNOWN, minute = 0, weekday = 0, day = 0, month = 0; // weekday 0 = Sunday, month 0 = January
    char next[6] = "";                                               // next event's start, "22:30"
    bool nextSoon = false;
    Panels::Panel panel;
};

inline bool same(const Panels::Panel &a, const Panels::Panel &b) {
    return a.colour == b.colour && !strcmp(a.top, b.top) && !strcmp(a.main, b.main) && !strcmp(a.sub, b.sub) && !strcmp(a.bottom, b.bottom);
}

// Field by field: the structs have padding, so comparing their bytes would see changes that aren't there.
inline bool same(const Model &a, const Model &b) {
    return a.phone == b.phone && a.mail == b.mail && a.claude == b.claude && a.printKnown == b.printKnown && a.printing == b.printing &&
           a.printDone == b.printDone && a.printPercent == b.printPercent && a.printMinutes == b.printMinutes && a.gaming == b.gaming &&
           a.gpuC == b.gpuC && a.hour == b.hour && a.minute == b.minute && a.weekday == b.weekday && a.day == b.day && a.month == b.month &&
           !strcmp(a.next, b.next) && a.nextSoon == b.nextSoon && same(a.panel, b.panel);
}

inline void fill(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) fb[y * AAText::W + x] = c;
}

// One seven-segment digit, 30 x 54 units before scaling: a b c d e f clockwise from the top, g in the middle.
inline void segment(uint16_t *fb, int which, float x, float y, float s, uint16_t c) {
    static const float P[7][6][2] = {
        {{5, 0}, {25, 0}, {21, 5}, {9, 5}},
        {{30, 2}, {30, 25}, {27, 27}, {25, 24}, {25, 7}},
        {{30, 52}, {30, 29}, {27, 27}, {25, 30}, {25, 47}},
        {{5, 54}, {25, 54}, {21, 49}, {9, 49}},
        {{0, 52}, {0, 29}, {3, 27}, {5, 30}, {5, 47}},
        {{0, 2}, {0, 25}, {3, 27}, {5, 24}, {5, 7}},
        {{3, 27}, {7, 24.5f}, {23, 24.5f}, {27, 27}, {23, 29.5f}, {7, 29.5f}},
    };
    static const int N[7] = {4, 5, 5, 4, 5, 5, 6};
    float xs[6], ys[6];
    for (int i = 0; i < N[which]; ++i) {
        xs[i] = x + P[which][i][0] * s;
        ys[i] = y + P[which][i][1] * s;
    }
    AAText::polygon(fb, xs, ys, N[which], c);
}

// A digit with its unlit segments showing behind it; d < 0 shows only the unlit ones.
inline void digit(uint16_t *fb, int d, float x, float y, float s, uint16_t lit, uint16_t ghost) {
    static const uint8_t MAP[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const uint8_t on = d >= 0 && d <= 9 ? MAP[d] : 0;
    for (int i = 0; i < 7; ++i) segment(fb, i, x, y, s, on >> i & 1 ? lit : ghost);
}

// The outer ring, 60 segments clockwise from the top; the first `lit` of them in `on`.
inline void ring(uint16_t *fb, int lit, uint16_t on, uint16_t off) {
    constexpr float R0 = 102, R1 = 113, TAU = 6.2831853f;
    const float step = TAU / 60, width = step * 0.76f;
    for (int i = 0; i < 60; ++i) {
        const float a0 = i * step - TAU / 4 + (step - width) / 2;
        AAText::arcSegment(fb, 120, 120, R0, R1, a0, a0 + width, i < lit ? on : off);
    }
}

// Lamps stay inside the ring: their corners sit 99 from the centre, the ring starts at 102.
constexpr int LAMP_W = 70, LAMP_H = 24, LAMP_L = 48, LAMP_R = 122, LAMP_T = 52, LAMP_B = 164;

// The font is monospaced: "CLAUDE 12" and "PRINT 100%" only fit with the letters closer together.
inline void lamp(uint16_t *fb, int x, int y, const char *text, uint16_t fillColour, uint16_t ink) {
    fill(fb, x, y, x + LAMP_W, y + LAMP_H, fillColour);
    const int n = int(strlen(text)), room = LAMP_W - 4, advance = AAFonts::AA_LAMP.glyphs['0' - 32].advance;
    const int spacing = n * advance > room ? (room - advance) / (n - 1) - advance : 0; // the last letter keeps its full width
    const int inked = n * advance + (n - 1) * spacing;
    AAText::draw(fb, AAFonts::AA_LAMP, text, x + LAMP_W / 2 - inked / 2, y + 17, ink, spacing);
}

inline void countLamp(uint16_t *fb, int x, int y, const char *label, int count) {
    char text[24];
    if (count == UNKNOWN) {
        snprintf(text, sizeof(text), "%s --", label);
        lamp(fb, x, y, text, LAMP_OFF, LAMP_TEXT);
    } else if (count == 0) {
        lamp(fb, x, y, label, LAMP_OFF, LAMP_TEXT);
    } else {
        snprintf(text, sizeof(text), "%s %d", label, count > 99 ? 99 : count);
        lamp(fb, x, y, text, AMBER, INK_AMBER);
    }
}

inline void printLamp(uint16_t *fb, int x, int y, const Model &m) {
    char text[40];
    if (!m.printKnown) {
        lamp(fb, x, y, "PRINT --", LAMP_OFF, LAMP_TEXT);
    } else if (m.printing) {
        if (m.printMinutes == UNKNOWN) snprintf(text, sizeof(text), "PRINT %d%%", m.printPercent);
        else if (m.printMinutes >= 600) snprintf(text, sizeof(text), "PRINT %dH", (m.printMinutes + 30) / 60);
        else if (m.printMinutes >= 60) snprintf(text, sizeof(text), "PRINT %dH%02d", m.printMinutes / 60, m.printMinutes % 60);
        else snprintf(text, sizeof(text), "PRINT %dM", m.printMinutes);
        lamp(fb, x, y, text, GREEN, INK_GREEN);
    } else if (m.printDone) {
        lamp(fb, x, y, "DONE", GREEN, INK_GREEN);
    } else {
        lamp(fb, x, y, "PRINT", LAMP_OFF, LAMP_TEXT);
    }
}

inline uint16_t tempColour(int c) { return c >= 88 ? RED : c >= 80 ? AMBER : GREEN; }

// The line under the digits: gaming shows the time, otherwise the date; the next event in both.
inline void statusLine(char *line, size_t size, const Model &m) {
    static const char *const DAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const bool haveTime = m.hour != UNKNOWN;
    const bool hasNext = m.next[0] != 0;
    if (!haveTime) {
        if (hasNext) snprintf(line, size, "NEXT %s", m.next);
        else snprintf(line, size, "NO TIME");
    } else if (m.gaming && m.gpuC != UNKNOWN) {
        if (hasNext) snprintf(line, size, "%02d:%02d NEXT %s", m.hour, m.minute, m.next);
        else snprintf(line, size, "%02d:%02d", m.hour, m.minute);
    } else if (hasNext) {
        snprintf(line, size, "%s %d NEXT %s", DAYS[m.weekday % 7], m.day, m.next);
    } else {
        snprintf(line, size, "%s %02d %s", DAYS[m.weekday % 7], m.day, MONTHS[m.month % 12]);
    }
}

inline void centre(uint16_t *fb, const Model &m) {
    const bool haveTime = m.hour != UNKNOWN;
    const bool showTemp = m.gaming && m.gpuC != UNKNOWN;
    if (showTemp) {
        const uint16_t c = tempColour(m.gpuC);
        const float s = 1.05f, w = 30 * s, gap = 9, x0 = 120 - w - gap / 2, y = 80;
        digit(fb, m.gpuC / 10 % 10, x0, y, s, c, GHOST);
        digit(fb, m.gpuC % 10, x0 + w + gap, y, s, c, GHOST);
        AAText::centered(fb, AAFonts::AA_SMALL, "GPU", 113, GREEN_DIM, 58);
        AAText::centered(fb, AAFonts::AA_SMALL, "\xB0" "C", 113, GREEN_DIM, 182);
    } else {
        const float s = 0.8f, w = 30 * s, gap = 7, colon = 13, total = 4 * w + 2 * gap + colon + gap;
        const float x0 = 120 - total / 2, y = 88;
        const uint16_t c = haveTime ? GREEN_CLOCK : GHOST;
        digit(fb, haveTime ? m.hour / 10 : -1, x0, y, s, c, GHOST);
        digit(fb, haveTime ? m.hour % 10 : -1, x0 + w + gap, y, s, c, GHOST);
        const float cx = x0 + 2 * w + gap + (colon + 2 * gap) / 2;
        AAText::disc(fb, cx, y + 13, 2.6f, c);
        AAText::disc(fb, cx, y + 31, 2.6f, c);
        const float x2 = x0 + 2 * w + 2 * gap + colon;
        digit(fb, haveTime ? m.minute / 10 : -1, x2, y, s, c, GHOST);
        digit(fb, haveTime ? m.minute % 10 : -1, x2 + w + gap, y, s, c, GHOST);
    }
    char line[24];
    statusLine(line, sizeof(line), m);
    AAText::centered(fb, AAFonts::AA_SMALL, line, 155, m.nextSoon ? AMBER : showTemp ? GREEN_DIM : DATE);
}

// Text in the largest of the given fonts that fits, cut with ".." if none does.
inline void fitted(uint16_t *fb, const AAFonts::Font *const *fonts, int nFonts, const char *s, int baseline, uint16_t c, int maxW) {
    for (int i = 0; i < nFonts; ++i)
        if (AAText::width(*fonts[i], s) <= maxW) {
            AAText::centered(fb, *fonts[i], s, baseline, c);
            return;
        }
    const AAFonts::Font &f = *fonts[nFonts - 1];
    char cut[32];
    size_t n = strlen(s);
    if (n > sizeof(cut) - 3) n = sizeof(cut) - 3;
    memcpy(cut, s, n);
    strcpy(cut + n, "..");
    while (n > 0 && AAText::width(f, cut) > maxW) {
        --n;
        strcpy(cut + n, "..");
    }
    AAText::centered(fb, f, cut, baseline, c);
}

inline void panel(uint16_t *fb, const Panels::Panel &p) {
    const uint16_t c = p.colour == Panels::Colour::RED ? RED : p.colour == Panels::Colour::GREEN ? GREEN : AMBER;
    const uint16_t ink = p.colour == Panels::Colour::RED ? INK_RED : p.colour == Panels::Colour::GREEN ? INK_GREEN : INK_AMBER;
    ring(fb, 60, c, c);
    AAText::disc(fb, 120, 120, 98, c, 74, 169); // the block, cut by a circle inside the ring
    AAText::centered(fb, AAFonts::AA_BODY, p.top, 62, c);
    static const AAFonts::Font *const MAIN[] = {&AAFonts::AA_TITLE, &AAFonts::AA_SUB};
    static const AAFonts::Font *const SUB[] = {&AAFonts::AA_SUB, &AAFonts::AA_BODY};
    fitted(fb, MAIN, 2, p.main, 112, ink, 184);
    fitted(fb, SUB, 2, p.sub, 146, ink, 180);
    AAText::centered(fb, AAFonts::AA_SMALL, p.bottom, 191, c);
}

inline void draw(uint16_t *fb, const Model &m) {
    fill(fb, 0, 0, AAText::W, AAText::H, BG);
    if (m.panel.colour != Panels::Colour::NONE) {
        panel(fb, m.panel);
        return;
    }
    ring(fb, m.printKnown && m.printing ? (m.printPercent * 60 + 50) / 100 : 0, GREEN, RING_OFF);
    countLamp(fb, LAMP_L, LAMP_T, "PHONE", m.phone);
    countLamp(fb, LAMP_R, LAMP_T, "MAIL", m.mail);
    centre(fb, m);
    countLamp(fb, LAMP_L, LAMP_B, "CLAUDE", m.claude);
    printLamp(fb, LAMP_R, LAMP_B, m);
}

} // namespace Console
