#pragma once
#include "Ancs.h"

// The Bluetooth side of the iPhone link: the display advertises, the iPhone pairs once, and the display then reads the
// iPhone's notification service over that same connection. Everything except the Bluetooth callbacks runs in poll().
namespace AncsClient {
void begin(const char *name);
void poll();
bool linked();          // subscribed to the iPhone's notifications
const char *stage();    // for the serial log
Ancs::Store &store();
void forgetPairing();
} // namespace AncsClient
