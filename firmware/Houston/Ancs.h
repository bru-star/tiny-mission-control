#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Apple Notification Center Service: what comes from the iPhone, with no Bluetooth code here so it can be tested
// on the PC. Only the app and the title (the sender) are requested, never the subtitle or the message.
namespace Ancs {

enum Category : uint8_t { OTHER = 0, INCOMING_CALL = 1, MISSED_CALL = 2, VOICEMAIL = 3, SOCIAL = 4, SCHEDULE = 5, EMAIL = 6 };
enum Event : uint8_t { ADDED = 0, MODIFIED = 1, REMOVED = 2 };
constexpr uint8_t FLAG_SILENT = 1 << 0, FLAG_PRE_EXISTING = 1 << 2;
constexpr uint8_t ATTR_APP = 0, ATTR_TITLE = 1;
constexpr uint16_t TITLE_MAX = 32;
constexpr size_t REQUEST_LEN = 9;

struct Source {
    uint8_t event, flags, category, count;
    uint32_t uid;
};

inline bool parseSource(const uint8_t *d, size_t n, Source &out) {
    if (n < 8) return false;
    out.event = d[0];
    out.flags = d[1];
    out.category = d[2];
    out.count = d[3];
    out.uid = d[4] | uint32_t(d[5]) << 8 | uint32_t(d[6]) << 16 | uint32_t(d[7]) << 24;
    return true;
}

// Get Notification Attributes: the app (no length follows it), then the title with its maximum length.
inline void buildRequest(uint32_t uid, uint8_t out[REQUEST_LEN]) {
    out[0] = 0;
    for (int i = 0; i < 4; ++i) out[1 + i] = uint8_t(uid >> (8 * i));
    out[5] = ATTR_APP;
    out[6] = ATTR_TITLE;
    out[7] = TITLE_MAX & 0xFF;
    out[8] = TITLE_MAX >> 8;
}

inline bool isMailApp(const char *app) {
    static const char *const MAIL[] = {"com.apple.mobilemail", "com.google.Gmail", "com.microsoft.Office.Outlook",
                                       "ch.protonmail.protonmail", "com.readdle.smartemail", "com.yahoo.Aerogram"};
    for (const char *m : MAIL)
        if (!strcmp(app, m)) return true;
    return false;
}

// A short name for the screen: known apps by name, others from the last part of the app's identifier.
inline void shortAppName(const char *app, char *out, size_t size) {
    static const char *const KNOWN[][2] = {{"com.apple.MobileSMS", "MESSAGES"}, {"com.apple.mobilemail", "MAIL"},
                                           {"com.google.Gmail", "GMAIL"}, {"net.whatsapp.WhatsApp", "WHATSAPP"},
                                           {"com.apple.mobilephone", "PHONE"}, {"com.apple.facetime", "FACETIME"},
                                           {"ph.telegra.Telegraph", "TELEGRAM"}, {"com.hammerandchisel.discord", "DISCORD"},
                                           {"com.microsoft.Office.Outlook", "OUTLOOK"}, {"com.burbn.instagram", "INSTAGRAM"}};
    const char *name = nullptr;
    for (auto &k : KNOWN)
        if (!strcmp(app, k[0])) name = k[1];
    if (!name) {
        const char *dot = strrchr(app, '.');
        name = dot ? dot + 1 : app;
    }
    size_t i = 0;
    for (; name[i] && i + 1 < size; ++i) out[i] = (name[i] >= 'a' && name[i] <= 'z') ? char(name[i] - 32) : name[i];
    out[i] = 0;
}

// Upper-case ASCII for the screen's font: accented letters fold to their base letter, other characters drop out.
inline void screenText(const char *utf8, char *out, size_t size) {
    static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTSAAAAAAACEEEEIIIIDNOOOOO/OUUUUYTY"; // U+00C0 to U+00FF
    size_t n = 0;
    for (const uint8_t *s = reinterpret_cast<const uint8_t *>(utf8); *s && n + 1 < size; ++s) {
        if (*s < 0x80) {
            if (*s >= ' ' && *s < 0x7F) out[n++] = (*s >= 'a' && *s <= 'z') ? char(*s - 32) : char(*s);
        } else if ((*s == 0xC3) && s[1] >= 0x80 && s[1] <= 0xBF) {
            out[n++] = LATIN1[*++s - 0x80];
        }
    }
    out[n] = 0;
}

// Reassembles one answer on the Data Source, which can span several Bluetooth packets. Text too long for the
// buffers is cut, and the rest of it is still consumed.
class Reader {
public:
    uint32_t uid = 0;
    char app[48] = "", title[TITLE_MAX + 1] = "";

    void reset() {
        state = COMMAND;
        attrs = 0;
    }

    // True when this byte completes an answer with both attributes.
    bool feed(uint8_t b) {
        switch (state) {
        case COMMAND:
            if (b != 0) return false; // not an answer to Get Notification Attributes
            uid = 0;
            at = 0;
            attrs = 0;
            app[0] = title[0] = 0;
            state = UID;
            return false;
        case UID:
            uid |= uint32_t(b) << (8 * at);
            if (++at == 4) state = ATTR;
            return false;
        case ATTR:
            attr = b;
            state = LEN_LOW;
            return false;
        case LEN_LOW:
            remaining = b;
            state = LEN_HIGH;
            return false;
        case LEN_HIGH:
            remaining |= uint16_t(b) << 8;
            at = 0;
            if (remaining == 0) return attributeDone();
            state = DATA;
            return false;
        case DATA:
            if (attr == ATTR_APP) store(app, sizeof(app), b);
            else if (attr == ATTR_TITLE) store(title, sizeof(title), b);
            ++at;
            if (--remaining == 0) return attributeDone();
            return false;
        }
        return false;
    }

private:
    enum State { COMMAND, UID, ATTR, LEN_LOW, LEN_HIGH, DATA } state = COMMAND;
    uint8_t attr = 0;
    uint16_t remaining = 0;
    size_t at = 0;
    int attrs = 0;

    void store(char *to, size_t size, uint8_t b) {
        if (at + 1 < size) {
            to[at] = char(b);
            to[at + 1] = 0;
        }
    }
    bool attributeDone() {
        state = ATTR;
        if (++attrs < 2) return false;
        state = COMMAND;
        return true;
    }
};

// What's waiting on the phone. Names aren't kept: the screen only needs counts, and each new arrival is handed to
// the panels once.
class Store {
public:
    static constexpr int MAX = 128;

    struct Fresh {
        char app[16], who[TITLE_MAX + 1];
        bool mail;
    };

    void clear() {
        n = 0;
        callUid = NO_CALL;
        callWho[0] = 0;
        hasFresh = false;
    }

    // Returns true when the notification's attributes should be fetched.
    bool apply(const Source &s) {
        if (s.event == REMOVED) {
            remove(s.uid);
            if (s.uid == callUid) {
                callUid = NO_CALL;
                callWho[0] = 0;
            }
            return false;
        }
        if (s.category == INCOMING_CALL) {
            callUid = s.uid;
            return s.event == ADDED || !callWho[0];
        }
        // iOS resends its whole Notification Centre on every connection, marked as old: only new arrivals count.
        // A modified notification isn't a new one either.
        if (s.event != ADDED || (s.flags & FLAG_PRE_EXISTING) || find(s.uid)) return false;
        if (n == MAX) remove(items[0].uid);
        items[n++] = {s.uid, s.category == EMAIL, !(s.flags & FLAG_SILENT)};
        return true;
    }

    void applyAttributes(uint32_t uid, const char *app, const char *who) {
        if (uid == callUid) {
            screenText(who, callWho, sizeof(callWho));
            return;
        }
        Item *it = find(uid);
        if (!it) return;
        if (isMailApp(app)) it->mail = true;
        if (it->announce) {
            it->announce = false;
            shortAppName(app, fresh.app, sizeof(fresh.app));
            screenText(who, fresh.who, sizeof(fresh.who));
            fresh.mail = it->mail;
            hasFresh = true;
        }
    }

    // After a failed fetch it still counts, it just can't be named.
    void attributesFailed(uint32_t uid) {
        if (Item *it = find(uid)) it->announce = false;
    }

    bool takeFresh(Fresh &out) {
        if (!hasFresh) return false;
        out = fresh;
        hasFresh = false;
        return true;
    }

    int phone() const { return count(false); }
    int mail() const { return count(true); }
    bool call() const { return callUid != NO_CALL; }
    const char *caller() const { return callWho; }

private:
    static constexpr uint32_t NO_CALL = 0xFFFFFFFF;
    struct Item {
        uint32_t uid;
        bool mail, announce;
    };
    Item items[MAX];
    int n = 0;
    uint32_t callUid = NO_CALL;
    char callWho[TITLE_MAX + 1] = "";
    Fresh fresh;
    bool hasFresh = false;

    Item *find(uint32_t uid) {
        for (int i = 0; i < n; ++i)
            if (items[i].uid == uid) return &items[i];
        return nullptr;
    }
    void remove(uint32_t uid) {
        for (int i = 0; i < n; ++i)
            if (items[i].uid == uid) {
                memmove(&items[i], &items[i + 1], sizeof(Item) * size_t(n - i - 1));
                --n;
                return;
            }
    }
    int count(bool wantMail) const {
        int c = 0;
        for (int i = 0; i < n; ++i) c += items[i].mail == wantMail;
        return c;
    }
};

} // namespace Ancs
