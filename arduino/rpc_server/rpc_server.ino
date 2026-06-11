/*
Arduino Uno Q sketch for MADS

This sketch is an example to develop the MCU counterpart for the 
https://github.com/mads-net/arduinoQ_plugin set of plugins.

Funcions here provided can be called via RPS from a MADS agent.

Author: Paolo Bosetti, University of Trento
*/

#include "Arduino_RouterBridge.h"

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    Monitor.begin();
    Monitor.print("Arduino Router starting... ");
    Bridge.begin();
    Bridge.provide("set_led_state", set_led_state);
    Bridge.provide("get_digital_pin", get_digital_pin);
    Bridge.provide("get_analog_pin", get_analog_pin);
    Monitor.println("done!");
}

void loop() {
}

void set_led_state(bool state) {
    // LOW state means LED is ON
    digitalWrite(LED_BUILTIN, state ? LOW : HIGH);
}

bool get_digital_pin(int pin) {
    return digitalRead(pin);
}

int get_analog_pin(int pin) {
    return analogRead(pin);
}