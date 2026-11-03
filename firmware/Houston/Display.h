#pragma once
#include <Adafruit_GFX.h>

// GC9A01A 240x240 round panel behind one RGB565 framebuffer. Draw into frame(),
// then present() sends the whole frame. The backlight stays dark until the
// first frame is on the panel.
namespace Display {
bool begin();
GFXcanvas16 &frame();
void present();
} // namespace Display
