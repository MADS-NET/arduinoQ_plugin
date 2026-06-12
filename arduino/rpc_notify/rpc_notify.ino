/*
Arduino Uno Q sketch for MADS MCU-to-CPU notifications.

This sketch emits a small notification once per second. Configure
unoq_source in notify mode with provided_rpc = "mads_notify" to publish these
notifications as MADS messages.

REQUIRED LIBRARIES (exact names):
- Arduino_RouterBridge
- MsgPack
*/

#include <Arduino_RouterBridge.h>

uint32_t Counter = 0;
uint32_t LastNotifyMicros = 0;

void setup() {
  Monitor.begin();
  Monitor.print("Arduino Router starting... ");
  Bridge.begin();
  Monitor.println("done!");
}

void loop() {
  const uint32_t now = micros();
  if (now - LastNotifyMicros < 500) {
    return;
  }

  LastNotifyMicros = now;
  // Notify any number of parameters under the name "mads_notify"
  Bridge.notify("mads_notify", Counter++, now);
  // you might want to notify other values under a different name, just call:
  // Bridge.notify("mads_other", "other message");
  // And remember to add in the ini file under the key `provided_rpc` the list 
  // of notify names, eg `provided_rpc = ["mads_notify", "mads_other"]`

}
