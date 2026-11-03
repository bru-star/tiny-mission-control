#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Decides which big panel shows. A call or a stopped print stays until it's over; the rest show for 15 s, one at a
// time, and then only their lamps or counts remain.
namespace Panels {

enum class Colour : uint8_t { NONE, AMBER, GREEN, RED };

struct Panel {
    Colour colour = Colour::NONE;
    char top[12] = "", main[24] = "", sub[24] = "", bottom[20] = "";
};

constexpr uint32_t SHOW_MS = 15000;

inline void copyText(char *to, const char *from, size_t size) {
    size_t n = 0;
    for (; from && from[n] && n + 1 < size; ++n) to[n] = from[n];
    to[n] = 0;
}

class Arbiter {
public:
    void setCall(bool active, const char *who) {
        call = active;
        if (active) set(callPanel, Colour::RED, "IPHONE", "CALL", who && who[0] ? who : "UNKNOWN", "RINGING");
    }

    void setPrintStopped(bool active, const char *file, const char *detail) {
        stopped = active;
        if (active) set(stopPanel, Colour::RED, "GO LOOK", "PRINT STOPPED", file, detail);
    }

    // A new message or mail: while one shows, the newest takes its place and the rest are counted.
    void message(const char *app, const char *who, uint32_t now) {
        step(now);
        if (active && isMessage) {
            ++more;
            snprintf(current.bottom, sizeof(current.bottom), "+%d MORE", more);
            copyText(current.main, app, sizeof(current.main));
            copyText(current.sub, who, sizeof(current.sub));
            since = now;
            return;
        }
        Panel p;
        set(p, Colour::AMBER, "NEW", app, who, "");
        push(p, true);
        step(now);
    }

    void flash(Colour colour, const char *top, const char *main, const char *sub, const char *bottom, uint32_t now) {
        Panel p;
        set(p, colour, top, main, sub, bottom);
        push(p, false);
        step(now);
    }

    const Panel &show(uint32_t now) {
        step(now);
        if (call) return callPanel;
        if (stopped) return stopPanel;
        return active ? current : none;
    }

private:
    static constexpr int QUEUE = 4;
    Panel callPanel, stopPanel, current, none;
    Panel queue[QUEUE];
    bool queueMessage[QUEUE] = {};
    int queued = 0;
    bool call = false, stopped = false, active = false, isMessage = false;
    int more = 0;
    uint32_t since = 0;

    static void set(Panel &p, Colour c, const char *top, const char *main, const char *sub, const char *bottom) {
        p.colour = c;
        copyText(p.top, top, sizeof(p.top));
        copyText(p.main, main, sizeof(p.main));
        copyText(p.sub, sub, sizeof(p.sub));
        copyText(p.bottom, bottom, sizeof(p.bottom));
    }
    void shiftQueue() {
        for (int i = 1; i < queued; ++i) {
            queue[i - 1] = queue[i];
            queueMessage[i - 1] = queueMessage[i];
        }
        --queued;
    }
    void push(const Panel &p, bool isMsg) {
        if (queued == QUEUE) shiftQueue(); // full: the oldest waiting panel gives way, its lamp still shows it
        queue[queued] = p;
        queueMessage[queued++] = isMsg;
    }
    void step(uint32_t now) {
        if (active && now - since < SHOW_MS) return;
        active = false;
        if (!queued) return;
        current = queue[0];
        isMessage = queueMessage[0];
        shiftQueue();
        more = 0;
        active = true;
        since = now;
    }
};

} // namespace Panels
