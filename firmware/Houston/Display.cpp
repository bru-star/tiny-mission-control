#include "Display.h"
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GC9A01A.h>
#include "Backlight.h"

namespace {
// Chosen for the case: along each edge of the board the wires nest and none cross on the way to the screen's pin row.
constexpr int TFT_SCK = 19, TFT_MOSI = 18, TFT_CS = 3, TFT_DC = 6, TFT_RST = 7, TFT_BLK = 2;
constexpr uint32_t SPI_HZ = 40000000;
Adafruit_GC9A01A panel(&SPI, TFT_DC, TFT_CS, TFT_RST);
GFXcanvas16 canvas(240, 240);
bool lit = false;
} // namespace

namespace Display {

bool begin() {
    if (!canvas.getBuffer()) {
        Serial.println("DISPLAY: framebuffer allocation failed");
        return false;
    }
    if (ledcAttach(TFT_BLK, Backlight::FREQUENCY_HZ, Backlight::RESOLUTION_BITS)) ledcWrite(TFT_BLK, 0);
    SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
    panel.begin(SPI_HZ);
    panel.setRotation(3); // the case holds the panel 90 degrees to the left
    Serial.println("DISPLAY READY: GC9A01A 240x240 RGB565 SPI=40000000");
    return true;
}

GFXcanvas16 &frame() { return canvas; }

void present() {
    panel.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);
    if (!lit) {
        ledcWrite(TFT_BLK, Backlight::duty(100));
        lit = true;
    }
}

} // namespace Display
