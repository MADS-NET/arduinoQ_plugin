[![Build and Release](https://github.com/MADS-NET/arduinoQ_plugin/actions/workflows/release.yml/badge.svg)](https://github.com/MADS-NET/arduinoQ_plugin/actions/workflows/release.yml)  ![MADS package](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FMADS-NET%2F.github%2Fmain%2Fprofile%2Fpackages.json&query=%24.packages.%5B'arduinoQ.plugin'%5D.type&label=mads%20package&logo=C%2B%2B)

# arduinoQ plugin for MADS

A collection of [MADS](https://mads-net.github.io) plugins for working with Arduino Uno Q devices.

> See the official [guide](https://mads-net.github.io/guides/arduino_uno_q.html).

*Required MADS version: 2.1.0.*

## The Arduino Uno Q

The [Uno Q](hhttps://www.arduino.cc/product-uno-q) is a new addition to the Arduino Uno family, featuring a powerful microcontroller and enhanced connectivity options. What sets it apart from its predecessors are the two onboard controllers: a CPU running Linux Debian, and a microcontroller (MCU) that can run Arduino code. This dual-controller architecture allows for more complex applications and seamless integration with various sensors and peripherals.

The communication between the Linux CPU and the Arduino MCU is facilitated through Linux service called [Arduino Router](https://docs.arduino.cc/tutorials/uno-q/user-manual/#bridge---remote-procedure-call-rpc-library), which tunnels Remote Procedure Calls (RPC) from the Linux side to the Arduino side and vice-versa. Via RPC, a function implemented on the MCU side (which has access to all physical pins and peripherals) can be called from the Linux side, and a function implemented on the Linux side (which has access to the network and more powerful processing capabilities) can be called from the MCU side.

> These plugins provide an interface for calling RPC functions on the Arduino Uno Q, allowing MADS users to easily integrate the capabilities of the Uno Q into their MADS applications.

## Supported platforms

The supported platforms for compiling the project are:

* **Linux** (Ubuntu for testing, Arduino Uno Q only for deployment)
* **MacOS** (testing only)

Windows is **not supported**, for the communication with the Uno Q relies on Unix domain sockets, which are not natively supported on Windows. 

## Build and Install

> Building on the Arduino Uno Q can be troublesome, for the device has a limited amount of memory and storage. For this reason, we suggest to download the release binaries and extract the content in the MADS prefix (see next).

To compile the binaries, follow the usual CMake build process:

```bash
cmake -Bbuild -DCMAKE_INSTALL_PREFIX="$(mads -p)"
cmake --build build -j4
sudo cmake --build build -t package
```

which produces a `.tar.gz` file in the `build` directory. Extract the content of the archive in the MADS prefix (e.g. `/usr/local/mads`):

```bash
sudo tar xzf path/to/arduinoq*.tar.gz -C "$(mads -p)" --strip-components=1
```

## Available binaries

The project provides the following binaries:

* `qrpc_client`: A command-line tool for sending RPC calls to the Arduino Uno Q and receiving responses (useful for testing)
* `qrpc_throughput`: A command-line tool that benchmarks the throughput of msgpack RPC calls from the CPU (Linux) side to the MCU side. It reads a JSON description of which RPC methods to exercise and how many times, runs each method one at a time, and reports per-method latency and throughput statistics (see [Throughput benchmarking](#throughput-benchmarking))
* `qrpc_dummy`: A dummy RPC router/server that simulates the behavior of the Arduino Uno Q for testing purposes (useful for development without the actual hardware). It implements the RPC functions `ping()` (which returns `pong`), `add(v1, v2)` (which takes two integers and returns their sum), `echo(arg1, arg2, arg3, ...)` (which echoes all received arguments), and `emit_notify(method, args...)` (which emits a local test notification to a registered method).
* `unoq_source`: A MADS source plugin that either routinely executes a given RPC call and publishes its output on a MADS topic, or registers a CPU-side RPC method and publishes MCU-to-CPU notifications received through the Arduino Router.
* `unoq_sink`: A MADS sink plugin that executes a given RPC call every time a message is received on a given MADS topic. RPC function name and arguments are encoded in the received message (see next section)
* `unoq_filter`: A MADS filter plugin that executes a given RPC call every time a message is received on a given MADS topic, and forwards the output of the RPC call to the output topic. RPC function name and arguments are encoded in the received message (see next section)

## Arduino sketches

The `arduino` directory contains example Arduino sketches that illustrate the usage of the RPC mechanism on the Arduino side. Look at the [documentation](https://docs.arduino.cc/tutorials/uno-q/user-manual/#bridge---remote-procedure-call-rpc-library) for more in-depth explanations.

Once uploaded the sketch, you can test it on the Linux side of the Uno Q with the command:

```bash
qrpc_client <function_name> [arg1 arg2 arg3 ...]
```

The sketch `rcp_server.ino` implements a few RPC functions:

* `set_led_state`: Accepts a boolean and togles the onboard LED
* `get_digital_pin`: Accepts a pin number and returns its digital value
* `get_analog_pin`: Accepts a pin number and returns its analog value
* `get_json`: Returns a JSON object
* `get_three_arrays`: Returns three arrays of different types

The sketch `rpc_server_modulino.ino` needs a **Modulino Movement** IMU board, and reads its acceleration values, accumulate into a buffer, and when the `get_fft` RPC function is invoked it returns the FFT of the acceleration values in the buffer.

The sketch `rpc_notify.ino` shows the opposite direction: the MCU calls
`Bridge.notify("mads_notify", ...)`, and `unoq_source` can publish those
notifications as MADS messages when configured in notify mode.

## Throughput benchmarking

The `qrpc_throughput` tool measures how fast msgpack RPC calls can be issued
from the CPU side of the Uno Q to the MCU side. It expects a JSON description
of the methods to test (read from a file argument or from `stdin`), runs each
method one at a time over a single shared connection, and reports statistics.

```bash
# from a file
qrpc_throughput throughput_example.json

# from stdin
qrpc_throughput < throughput_example.json

# machine-readable JSON results on stdout
qrpc_throughput --json throughput_example.json

# override the socket path declared in the JSON
qrpc_throughput --socket /tmp/mads-rpc.sock throughput_example.json
```

The input JSON has the following shape:

```json
{
  "socket": "/var/run/arduino-router.sock",
  "warmup": 50,
  "iterations": 2000,
  "tests": [
    { "rpc_func": "ping", "rpc_args": [] },
    { "rpc_func": "add", "rpc_args": [3, 4], "iterations": 5000 },
    { "rpc_func": "echo", "rpc_args": ["hello", 42, 3.14, true], "label": "echo-4" }
  ]
}
```

* `socket` (optional): path to the Arduino Router socket. Defaults to
  `/var/run/arduino-router.sock`; overridden by `--socket`.
* `warmup` (optional, default `0`): number of unmeasured calls performed before
  timing each test, to prime the connection.
* `iterations` (optional, default `1000`): default number of timed calls per
  test; can be overridden per test.
* `tests`: array of test specifications. Each entry needs `rpc_func` (the MCU
  method name) and may include `rpc_args` (same argument types as the other
  plugins), `iterations`, and a human-readable `label`.

For each test the tool reports the number of successful and failed calls, the
overall throughput (successful calls per second), and the latency distribution
(min, mean, median, p90, p95, p99, max, and standard deviation, in
microseconds). Failed calls are counted but excluded from the latency
statistics. By default a human-readable table is printed; with `--json` the
full statistics are emitted as JSON on `stdout` (progress is kept on `stderr`).

You can try it locally without hardware against the `qrpc_dummy` server, which
implements `ping`, `add`, `echo`, and `array`:

```bash
build/qrpc_dummy /tmp/mads-rpc.sock &
qrpc_throughput --socket /tmp/mads-rpc.sock throughput_example.json
```

## MADS Director file

The `director.toml` and the `mads.ini` file contain example configurations for testing the plugins on a local, development machine (Linux or macOS). The `director.toml` also runs the `qrpc_dummy` server, which simulates the behavior of the Arduino Uno Q for testing purposes.

## Input messages

The `unoq_sink` and `unoq_filter` plugins expect the input message to be a JSON object with the following format:

```json
{
  "rpc_func": "function_name",
  "rpc_args": [arg1, arg2, arg3, ...]
}
```

where `function_name` is the name of the RPC function to call on the Arduino Uno Q, and `rpc_args` is an array of arguments to pass to the function (if any). Supported types are:

* `string`
* `int64_t`
* `double`
* `bool`
* an array of any of the above types

## MCU-to-CPU notification messages

When `unoq_source` runs in notify mode, it registers one or more CPU-side
methods with the Arduino Router. Incoming MCU notifications are accumulated in
a queue. Each `get_output()` call drains the current queue, clears it, and
publishes one top-level key per configured RPC method:

```json
{
  "mads_notify": [
    [arg1, arg2, arg3],
    [arg1, arg2, arg3]
  ]
}
```

With multiple registered methods, the output is grouped by method name:

```json
{
  "rpc_1": [
    [arg1, arg2]
  ],
  "rpc_2": [
    [arg3, arg4]
  ]
}
```

On the MCU side, emit this message with:

```cpp
Bridge.notify("mads_notify", arg1, arg2, arg3);
```

Notifications are fire-and-forget. If the source queue is full, the plugin
applies the configured overflow policy and the MCU does not receive a delivery
confirmation. If confirmation is required, use `Bridge.call()` on the MCU side;
the source plugin acknowledges request frames after enqueueing them.

## INI settings

The plugin supports the following settings in the INI file:

```ini
[unoq_source]
pub_topic = "unoq_source"  # The topic on which the plugin will publish the output of the RPC call
mode = "call" # Optional; "call" is the default
rpc_call = "ping"  # The RPC call to execute (the function name on MCU)
rpc_args = [] # the list of its arguments, if any
period = 1000  # The period (in milliseconds) at which the RPC call will be executed

[unoq_source_notify]
pub_topic = "unoq_notify"  # The topic on which received MCU notifications are published
mode = "notify"
provided_rpc = "mads_notify" # Or an array of method names
max_queue = 256
overflow = "drop_oldest" # Or "drop_newest"

[unoq_sink]
sub_topic = ["unoq_sink"]  # The topic on which the plugin will publish the output of the RPC call
verbose = false  # Whether to print verbose output

[unoq_filter]
sub_topic = ["unoq_filter_in"]  # The topic on which the plugin will subscribe to receive the input message
pub_topic = "unoq_filter_out"  # The topic on which the plugin will publish the output of the RPC call
verbose = false  # Whether to print verbose output
```

Most settings are optional; if omitted, defaults are used. In call mode,
`rpc_call` or `rpc_func` must name the MCU function to call. In notify mode,
`provided_rpc` or `rpc_call` must name the CPU-side method to register.

For local notification testing without hardware:

```bash
build/qrpc_dummy /tmp/mads-rpc.sock
build/unoq_source.plugin notify mads_notify /tmp/mads-rpc.sock
build/qrpc_call /tmp/mads-rpc.sock emit_notify mads_notify 42 hello
```



---
