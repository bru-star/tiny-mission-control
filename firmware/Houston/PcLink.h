#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>
#include "Console.h"
#include "Panels.h"

// What the PC helper sends over Wi-Fi (pc_helper/houston_helper.py), and what it does to the screen. Everything from the
// PC goes unknown after 15 s of silence, so a stopped helper never leaves old numbers up.
namespace PcLink {

constexpr uint16_t PORT = 4210;
constexpr uint32_t QUIET_MS = 15000;

struct Report {
    bool game = false;
    int gpu = Console::UNKNOWN;
    bool printKnown = false;
    char printState[6] = "";
    int printPercent = 0, printMinutes = Console::UNKNOWN;
    char printFile[24] = "", printWhy[24] = "";
    bool claudeKnown = false;
    int claudeCount = 0;
    uint32_t askId = 0;
    char askTitle[24] = "";
    char nextAt[6] = "";
    bool nextSoon = false;
    uint32_t nextId = 0;
    char nextName[24] = "";
};

inline bool parse(const char *text, size_t length, Report &out) {
    JsonDocument doc;
    if (deserializeJson(doc, text, length) || doc["v"] != 1) return false;
    out = Report();
    out.game = (doc["game"] | 0) != 0;
    out.gpu = doc["gpu"] | Console::UNKNOWN;
    out.printKnown = (doc["pk"] | 0) != 0;
    Panels::copyText(out.printState, doc["ps"] | "", sizeof(out.printState));
    out.printPercent = doc["pp"] | 0;
    out.printMinutes = doc["pm"] | Console::UNKNOWN;
    Panels::copyText(out.printFile, doc["pf"] | "", sizeof(out.printFile));
    Panels::copyText(out.printWhy, doc["pw"] | "", sizeof(out.printWhy));
    out.claudeKnown = (doc["ck"] | 0) != 0;
    out.claudeCount = doc["cn"] | 0;
    out.askId = doc["ci"] | 0u;
    Panels::copyText(out.askTitle, doc["ca"] | "", sizeof(out.askTitle));
    Panels::copyText(out.nextAt, doc["na"] | "", sizeof(out.nextAt));
    out.nextSoon = (doc["ns"] | 0) != 0;
    out.nextId = doc["ni"] | 0u;
    Panels::copyText(out.nextName, doc["nn"] | "", sizeof(out.nextName));
    return true;
}

class Link {
public:
    // Panels fire on changes, so restarting the display or the helper doesn't replay old news.
    void received(const Report &r, uint32_t now, Panels::Arbiter &panels) {
        if (any) {
            if (!strcmp(last.printState, "run") && !strcmp(r.printState, "done"))
                panels.flash(Panels::Colour::GREEN, "YOUR MOVE", "PRINT DONE", r.printFile, "", now);
            if (r.askId != last.askId && r.askTitle[0])
                panels.flash(Panels::Colour::AMBER, "YOUR MOVE", "CLAUDE ASKS", r.askTitle, "NEEDS YOU", now);
        }
        if (r.nextSoon && r.nextId != soonId) {
            soonId = r.nextId;
            panels.flash(Panels::Colour::AMBER, "IN 10 MIN", "MEETING", r.nextName, r.nextAt, now);
        }
        panels.setPrintStopped(r.printKnown && !strcmp(r.printState, "stop"), r.printFile, r.printWhy);
        last = r;
        any = true;
        at = now;
    }

    bool live(uint32_t now) const { return any && now - at < QUIET_MS; }

    void fill(Console::Model &m, uint32_t now, Panels::Arbiter &panels) const {
        if (!live(now)) {
            panels.setPrintStopped(false, nullptr, nullptr);
            return;
        }
        m.gaming = last.game;
        m.gpuC = last.gpu;
        m.claude = last.claudeKnown ? last.claudeCount : Console::UNKNOWN;
        m.printKnown = last.printKnown;
        m.printing = !strcmp(last.printState, "run");
        m.printDone = !strcmp(last.printState, "done");
        m.printPercent = last.printPercent;
        m.printMinutes = last.printMinutes;
        Panels::copyText(m.next, last.nextAt, sizeof(m.next));
        m.nextSoon = last.nextSoon;
    }

private:
    Report last;
    bool any = false;
    uint32_t at = 0, soonId = 0;
};

} // namespace PcLink
