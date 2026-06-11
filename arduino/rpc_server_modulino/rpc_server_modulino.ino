/*
Arduino Uno Q sketch for MADS

This sketch is an example to develop the MCU counterpart for the 
https://github.com/mads-net/arduinoQ_plugin set of plugins.

Funcions here provided can be called via RPS from a MADS agent.

THIS VERSION REQUIRES A MODULINO MOVEMENT QWIIK MODULE CONNECTED

REQUIRED LIBRARIES (exact names):
- Arduino_RouterBridge
- Arduino_Modulino
- MsgPack
- TaskScheduler
- KickFFT

Author: Paolo Bosetti, University of Trento
*/

#include <Arduino_RouterBridge.h>
#include <Arduino_Modulino.h>
#define _TASK_MICRO_RES // TaskScheduler uses microseconds
#include <TaskScheduler.h>
#include <MsgPack.h>
#include <KickFFT.h>
#include <array>
#include <cmath>

// Customizable parameters for the FFT
static constexpr size_t RATE = 10'000; // samples/sec
static constexpr size_t SIZE = 512; // MUST be power of 2 and <=512

// Struct that implements a fixed size ring-array
template <typename T, size_t N>
struct RingArray {
  T data[N]{};          // value-initialize elements
  std::size_t head = 0; // next write index

  void push(const T& val) {
    data[head] = val;
    ++head;
    if (head == N) head = 0;
  }
};

// Struct that will contain the MSGPACK published data
template <typename T, size_t R, size_t N>
struct FFTData {
  size_t rate_hz = R;
  size_t size = N/2;
  std::array<uint32_t, N/2> mag;
  MSGPACK_DEFINE(rate_hz, size, mag); // only these fields are published

  void calculate(T values[]) {
    KickFFT<float>::fft(R, 0, R/2.0, N, values, mag.data());
  }
};

// Types
typedef FFTData<float, RATE, SIZE> MyFFTData;

// Global vars are Capitalized
ModulinoMovement Movement;
RingArray<float, SIZE> Buffer;

// Task scheduler:
Scheduler Runner;
// accumulates data at RATE Hz
void task_acquire() {
  Movement.update();
  Buffer.push(std::hypot(Movement.getX(), Movement.getY(), Movement.getZ()));
}
Task AcquireTask(1e6/RATE, TASK_FOREVER, &task_acquire);

// blinks the LED every second
Task WatchdogTask(1e6, TASK_FOREVER, &task_watchdog);
void task_watchdog() {
  static auto state = HIGH;
  digitalWrite(LED_BUILTIN, state ? LOW : HIGH);
  state = (state == LOW ? HIGH : LOW);
  // You might also print con Monitor console...
  // Monitor.println(Buffer.head);
}


void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Monitor.begin();
  Monitor.print("Arduino Router starting... ");
  // Movement setup
  Modulino.begin();
  Movement.begin();

  // Scheduler setup
  Runner.init();
  Runner.addTask(AcquireTask);
  AcquireTask.enable();
  Runner.addTask(WatchdogTask);
  WatchdogTask.enable();

  // Bridge setup
  Bridge.begin();
  Bridge.provide("get_fft", get_fft);

  Monitor.println("done!");
}

void loop() {
  Runner.execute(); // remember to enable scheduler!
}

// RPC Call, gets no input and returns a MyFFTData struct
MyFFTData get_fft() {
  MyFFTData out{};
  out.calculate(Buffer.data);
  return out;
}