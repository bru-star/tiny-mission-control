// Houston: a small status display for your monitor, a quick look while gaming. The iPhone connects over Bluetooth; the
// PC helper reports GPU, print, Claude Code and calendar over Wi-Fi, and without it those lamps show "--". Free memory
// goes to the serial log.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "secrets.h"
#include "AncsClient.h"
#include "Console.h"
#include "Display.h"
#include "Panels.h"
#include "PcLink.h"

namespace {

constexpr uint32_t MODEL_MS = 200, MEMORY_LOG_MS = 60000;

Console::Model shown;
Panels::Arbiter arbiter;
PcLink::Link pc;
WiFiUDP udp;
bool listening = false, anyFrame = false;
uint32_t frameMs = 0;

// Listens once Wi-Fi is up. The helper finds the display by name (houston.local) and sends to it directly.
void pollPc(uint32_t now) {
    if (!listening && WiFi.status() == WL_CONNECTED) {
        MDNS.begin("houston");
        udp.begin(PcLink::PORT);
        listening = true;
        Serial.println("PC LINK: listening");
    }
    if (!listening) return;
    while (udp.parsePacket() > 0) {
        char packet[512];
        const int length = udp.read(packet, sizeof(packet));
        PcLink::Report report;
        if (length > 0 && PcLink::parse(packet, size_t(length), report)) pc.received(report, now, arbiter);
    }
}

Console::Model buildModel(uint32_t now) {
    Console::Model m;
    if (AncsClient::linked()) {
        Ancs::Store &phone = AncsClient::store();
        m.phone = phone.phone();
        m.mail = phone.mail();
        Ancs::Store::Fresh fresh;
        if (phone.takeFresh(fresh)) arbiter.message(fresh.app, fresh.who, now);
        arbiter.setCall(phone.call(), phone.caller());
    } else {
        arbiter.setCall(false, nullptr);
    }
    tm t;
    if (getLocalTime(&t, 0)) {
        m.hour = t.tm_hour;
        m.minute = t.tm_min;
        m.weekday = t.tm_wday;
        m.day = t.tm_mday;
        m.month = t.tm_mon;
    }
    pc.fill(m, now, arbiter);
    m.panel = arbiter.show(now);
    return m;
}

void logMemory() {
    Serial.printf("MEMORY: free=%lu min=%lu largest=%u wifi=%d ble=%s phone=%d mail=%d pc=%d frame=%lums\n", static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)), WiFi.status() == WL_CONNECTED,
                  AncsClient::stage(), AncsClient::store().phone(), AncsClient::store().mail(), pc.live(millis()),
                  static_cast<unsigned long>(frameMs));
}

void handleSerial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "STATUS") logMemory();
    else if (line == "FORGET") {
        AncsClient::forgetPairing();
        Serial.println("ANCS: pairing forgotten; also forget Houston on the iPhone");
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    Display::begin();
    Console::draw(Display::frame().getBuffer(), shown);
    Display::present();
    logMemory();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTzTime(TIME_ZONE, "pool.ntp.org");
    AncsClient::begin("Houston");
    logMemory();
}

void loop() {
    const uint32_t now = millis();
    static uint32_t lastModel = 0, lastMemory = 0;
    AncsClient::poll();
    pollPc(now);
    handleSerial();
    if (now - lastModel >= MODEL_MS) {
        lastModel = now;
        const Console::Model m = buildModel(now);
        if (!anyFrame || !Console::same(m, shown)) {
            anyFrame = true;
            shown = m;
            const uint32_t started = millis();
            Console::draw(Display::frame().getBuffer(), shown);
            Display::present();
            frameMs = millis() - started;
        }
    }
    if (now - lastMemory >= MEMORY_LOG_MS) {
        lastMemory = now;
        logMemory();
    }
    delay(5);
}
