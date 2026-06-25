/*
Arduino Uno Q sketch for MADS

This sketch is an example to develop the MCU counterpart for the 
https://github.com/mads-net/arduinoQ_plugin set of plugins.

Funcions here provided can be called via RPS from a MADS agent.

REQUIRED LIBRARIES (exact names):
- Arduino_RouterBridge
- MsgPack
- ArduinoJson

Use the command:
> qrpc_call <function_name> [args]
on the Linux sidefor testing this sketch.

Author: Paolo Bosetti, University of Trento
*/

// Required libraries
#include <Arduino_RouterBridge.h>
#include <ArduinoJson.h>
#include <MsgPack.h>
// Standard library
#include <array>

static constexpr size_t N = 8;

// This is the proper way to pack multiple data
// so that they can be returned by a RPC call
struct ThreeArrays {
  std::array<float, N> a;
  std::array<float, N> b;
  std::array<float, N> c;
  MSGPACK_DEFINE(a, b, c);
};

JsonDocument Document;

// using ActualSerial = SerialPort;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  Serial.println("Arduino Router starting... ");
  Bridge.begin();
  // Look at the docs for the difference between provide() and provide_safe()
  Bridge.provide("set_led_state", set_led_state);
  Serial.println("Providing RPC call set_led_state(bool)");

  Bridge.provide("get_digital_pin", get_digital_pin);
  Serial.println("Providing RPC call get_digital_pin(int)");

  Bridge.provide("get_analog_pin", get_analog_pin);
  Serial.println("Providing RPC call get_analog_pin(int)");

  Bridge.provide("get_json", get_json);
  Serial.println("Providing RPC call get_json()");

  Bridge.provide_safe("get_three_arrays", get_three_arrays);
  Serial.println("Providing RPC call get_three_arrays()");

  Serial.println("done!");
}

void loop() {
}

// RPC call with no return (sink)
void set_led_state(bool state) {
  // LOW state means LED is ON
  digitalWrite(LED_BUILTIN, state ? LOW : HIGH);
}

// RPC call with in and out (filter)
bool get_digital_pin(int pin) {
  return digitalRead(pin);
}

// RPC call with in and out (filter)
int get_analog_pin(int pin) {
  return analogRead(pin);
}

// RPC call that returns compund data (source)
ThreeArrays get_three_arrays() {
  ThreeArrays out;
  for (size_t i = 0; i < N; ++i) {
    out.a[i] = static_cast<float>(i);
    out.b[i] = static_cast<float>(10 * i);
    out.c[i] = static_cast<float>(i * i);
  }
  return out;
}

// You can also return a JSON string
// This is simpler than the above case, but computationally heavier
String get_json() {
  Document["test"] = "Hello, world!";
  JsonArray ary = Document["array"].to<JsonArray>();
  ary.add(digitalRead(2));
  ary.add(digitalRead(3));
  ary.add(digitalRead(4));
  String result;
  serializeJson(Document, result);
  return result;
}