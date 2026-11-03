#include "AncsClient.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>

namespace {

const char *const ANCS_SERVICE = "7905F431-B5CE-4E99-A40F-4B1E122D00D0";
const char *const NOTIFICATION_SOURCE = "9FBF120D-6301-42D9-8C58-25E699A21DBD";
const char *const CONTROL_POINT = "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9";
const char *const DATA_SOURCE = "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB";
// 128-bit service solicitation, the ANCS UUID reversed. iOS still doesn't list the device in Settings: see the README's Pairing.
const uint8_t SOLICIT_ANCS[] = {0x11, 0x15, 0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
                                0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79};
constexpr uint16_t APPEARANCE_WATCH = 0x00C0; // Bluetooth SIG: Generic Watch, the closest class to a notification display

enum class Stage : uint8_t { ADVERTISING, CONNECTED, SECURED, SUBSCRIBED };

constexpr uint32_t RETRY_MS = 3000, ANSWER_TIMEOUT_MS = 3000;

NimBLEServer *server = nullptr;
NimBLERemoteCharacteristic *control = nullptr;
volatile Stage current = Stage::ADVERTISING;
volatile uint16_t connHandle = 0;
volatile bool serviceChanged = false;
uint32_t nextTry = 0;

// Bluetooth callbacks run on their own task; they only hand bytes to poll().
QueueHandle_t sources = nullptr;
StreamBufferHandle_t answers = nullptr;

Ancs::Store notifications;
Ancs::Reader reader;
uint32_t pending[Ancs::Store::MAX];
int pendingHead = 0, pendingCount = 0;
bool waitingAnswer = false;
uint32_t askedUid = 0, askedAt = 0;

void pendingPush(uint32_t uid) {
    if (pendingCount == Ancs::Store::MAX) return;
    pending[(pendingHead + pendingCount++) % Ancs::Store::MAX] = uid;
}

void resetLink() {
    control = nullptr;
    pendingHead = pendingCount = 0;
    waitingAnswer = false;
    reader.reset();
    notifications.clear();
    if (sources) xQueueReset(sources);
    if (answers) xStreamBufferReset(answers);
}

class ServerEvents : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
        connHandle = info.getConnHandle();
        current = Stage::CONNECTED;
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override { current = Stage::ADVERTISING; }
    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        if (info.isEncrypted()) current = Stage::SECURED;
        else current = Stage::CONNECTED; // pairing refused: ask again on the next poll
    }
} serverEvents;

void onSource(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    if (length >= 8) xQueueSend(sources, data, 0);
}

void onAnswer(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    xStreamBufferSend(answers, data, length, 0);
}

void onServiceChanged(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool) { serviceChanged = true; }

bool subscribeToIphone() {
    NimBLEClient *client = server->getClient(connHandle);
    if (!client) return false;
    if (NimBLERemoteService *gatt = client->getService("1801")) {
        if (NimBLERemoteCharacteristic *changed = gatt->getCharacteristic("2A05")) changed->subscribe(false, onServiceChanged);
    }
    NimBLERemoteService *service = client->getService(ANCS_SERVICE);
    if (!service) {
        Serial.println("ANCS: service not published yet");
        return false;
    }
    NimBLERemoteCharacteristic *source = service->getCharacteristic(NOTIFICATION_SOURCE);
    NimBLERemoteCharacteristic *data = service->getCharacteristic(DATA_SOURCE);
    control = service->getCharacteristic(CONTROL_POINT);
    if (!source || !data || !control) {
        Serial.println("ANCS: characteristics missing");
        return false;
    }
    // Answers first: subscribing to the source makes the iPhone replay everything waiting.
    if (!data->subscribe(true, onAnswer) || !source->subscribe(true, onSource)) {
        Serial.println("ANCS: subscribe refused");
        return false;
    }
    return true;
}

void drainSources() {
    uint8_t raw[8];
    Ancs::Source s;
    while (xQueueReceive(sources, raw, 0) == pdTRUE) {
        if (!Ancs::parseSource(raw, sizeof(raw), s)) continue;
        Serial.printf("ANCS: event=%u flags=%u category=%u uid=%lu\n", s.event, s.flags, s.category, static_cast<unsigned long>(s.uid));
        if (notifications.apply(s)) pendingPush(s.uid);
    }
}

void drainAnswers() {
    uint8_t chunk[64];
    size_t got;
    while ((got = xStreamBufferReceive(answers, chunk, sizeof(chunk), 0)) > 0) {
        for (size_t i = 0; i < got; ++i) {
            if (!reader.feed(chunk[i])) continue;
            // The sender's name stays on the screen: the log only says how long it is.
            Serial.printf("ANCS: uid=%lu app=%s who=%u chars\n", static_cast<unsigned long>(reader.uid), reader.app,
                          static_cast<unsigned>(strlen(reader.title)));
            notifications.applyAttributes(reader.uid, reader.app, reader.title);
            waitingAnswer = false;
        }
    }
}

void askNext(uint32_t now) {
    if (waitingAnswer && now - askedAt > ANSWER_TIMEOUT_MS) {
        notifications.attributesFailed(askedUid);
        reader.reset();
        waitingAnswer = false;
    }
    if (waitingAnswer || !pendingCount || !control) return;
    askedUid = pending[pendingHead];
    pendingHead = (pendingHead + 1) % Ancs::Store::MAX;
    --pendingCount;
    uint8_t request[Ancs::REQUEST_LEN];
    Ancs::buildRequest(askedUid, request);
    if (control->writeValue(request, sizeof(request), true)) {
        waitingAnswer = true;
        askedAt = now;
    } else {
        notifications.attributesFailed(askedUid); // the notification may already be gone
    }
}

} // namespace

namespace AncsClient {

void begin(const char *name) {
    sources = xQueueCreate(64, 8);
    answers = xStreamBufferCreate(1024, 1);
    NimBLEDevice::init(name);
    NimBLEDevice::setSecurityAuth(true, false, true); // bonded, no passkey (no buttons here), secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    server = NimBLEDevice::createServer();
    server->setCallbacks(&serverEvents, false);
    server->advertiseOnDisconnect(true);
    // Solicitation and appearance fill the first packet's 31 bytes; the name goes in the scan response.
    NimBLEAdvertisementData ad;
    ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    ad.setAppearance(APPEARANCE_WATCH);
    ad.addData(SOLICIT_ANCS, sizeof(SOLICIT_ANCS));
    NimBLEAdvertisementData response;
    response.setName(name);
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisementData(ad);
    advertising->enableScanResponse(true);
    advertising->setScanResponseData(response);
    advertising->start();
    Serial.println("ANCS: advertising; pair by connecting from a scanner app");
}

void poll() {
    const uint32_t now = millis();
    static Stage seen = Stage::ADVERTISING;
    // The source of truth for a drop: a disconnect can land while poll() is busy subscribing and be overwritten.
    if (current != Stage::ADVERTISING && server->getConnectedCount() == 0) current = Stage::ADVERTISING;
    const Stage stageNow = current;
    if (stageNow != seen) {
        if (stageNow == Stage::ADVERTISING) resetLink();
        seen = stageNow;
        Serial.printf("ANCS: stage %s\n", stage());
        nextTry = now;
    }
    switch (stageNow) {
    case Stage::ADVERTISING:
        break;
    case Stage::CONNECTED:
        if (now - nextTry < 0x80000000u) {
            nextTry = now + RETRY_MS;
            NimBLEDevice::startSecurity(connHandle);
        }
        break;
    case Stage::SECURED:
        if (now - nextTry < 0x80000000u) {
            nextTry = now + RETRY_MS;
            if (subscribeToIphone()) current = Stage::SUBSCRIBED;
        }
        break;
    case Stage::SUBSCRIBED:
        drainSources();
        drainAnswers();
        askNext(now);
        if (serviceChanged) { // the iPhone can withdraw and republish the service at any time
            serviceChanged = false;
            if (NimBLEClient *client = server->getClient(connHandle)) client->deleteServices();
            resetLink();
            current = Stage::SECURED;
        }
        break;
    }
}

bool linked() { return current == Stage::SUBSCRIBED; }

const char *stage() {
    switch (current) {
    case Stage::ADVERTISING: return "advertising";
    case Stage::CONNECTED: return "connected";
    case Stage::SECURED: return "secured";
    case Stage::SUBSCRIBED: return "subscribed";
    }
    return "?";
}

Ancs::Store &store() { return notifications; }

void forgetPairing() { NimBLEDevice::deleteAllBonds(); }

} // namespace AncsClient
