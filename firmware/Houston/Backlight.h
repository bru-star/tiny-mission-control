#pragma once
#include <stdint.h>

// Display backlight (the module's BLK pin) as PWM: brightness (0-100 %) to a duty
// cycle.
namespace Backlight {
constexpr uint32_t FREQUENCY_HZ = 20000; // above hearing and camera banding
constexpr uint8_t RESOLUTION_BITS = 8;
constexpr uint32_t MAX_DUTY = (1u << RESOLUTION_BITS) - 1;

// Perceived brightness follows roughly the square root of light output, so the
// percentage is squared: 50 % looks about half as bright as 100 %. 0 is off;
// any other setting stays lit. 100 % is full on, as with BLK tied to 3.3 V.
constexpr uint32_t duty(uint8_t percent) {
    if (percent == 0) return 0;
    if (percent > 100) percent = 100;
    const uint32_t d = (static_cast<uint32_t>(percent) * percent * MAX_DUTY + 5000) / 10000;
    return d ? d : 1;
}
} // namespace Backlight
