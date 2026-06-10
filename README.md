[![Build and Release](https://github.com/MADS-NET/arduinoQ_plugin/actions/workflows/release.yml/badge.svg)](https://github.com/MADS-NET/arduinoQ_plugin/actions/workflows/release.yml)

# arduinoq plugin for MADS

A collection of [MADS](https://mads-net.github.io) plugins for working with Arduino Uno Q devices.

*Required MADS version: 2.1.0.*

## The Arduino Uno Q

The [Uno Q](https://store.arduino.cc/uno-q) is a new addition to the Arduino Uno family, featuring a powerful microcontroller and enhanced connectivity options. What sets it apart from its predecessors are the two onboard controllers: a CPU running Linux Debian, and a microcontroller (MCU) that can run Arduino code. This dual-controller architecture allows for more complex applications and seamless integration with various sensors and peripherals.

The communication between the Linux CPU and the Arduino MCU is facilitated through Linux service called [Arduino Router](https://docs.arduino.cc/tutorials/uno-q/user-manual/#bridge---remote-procedure-call-rpc-library), which tunnels Remote Procedure Calls (RPC) from the Linux side to the Arduino side and vice-versa. Via RPC, a function implemented on the MCU side (which has access to all physical pins and peripherals) can be called from the Linux side, and a function implemented on the Linux side (which has access to the network and more powerful processing capabilities) can be called from the MCU side.

> These plugins provide an interface for calling RPC functions on the Arduino Uno Q, allowing MADS users to easily integrate the capabilities of the Uno Q into their MADS applications.

## Supported platforms

The supported platforms for compiling the project are:

* **Linux** 
* **MacOS**

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
sudo tar -xzf build/arduinoq_plugin-*.tar.gz -C "$(mads -p)"
```

## Available binaries

The project provides the following binaries:

* `qrpc_client`: A command-line tool for sending RPC calls to the Arduino Uno Q and receiving responses (useful for testing)
* `qrpc_dummy`: A dummy RPC server that simulates the behavior of the Arduino Uno Q for testing purposes (useful for development without the actual hardware). It implements the RPC functions `ping()` (which returns `pong`), and `add(v1, v2)` (which takes two integers and returns their sum), and `echo(arg1, arg2, arg3, ...)` (which echoes all received arguments)
* `unoq_source`: A MADS source plugin that routinely executes a given RPC call publishing its output on a MADS topic. The RPC call is defined in the INI file (see next section).
* `unoq_sink`: A MADS sink plugin that executes a given RPC call every time a message is received on a given MADS topic. RPC function name and arguments are encoded in the received message (see next section)
* `unoq_filter`: A MADS filter plugin that executes a given RPC call every time a message is received on a given MADS topic, and forwards the output of the RPC call to the output topic. RPC function name and arguments are encoded in the received message (see next section)


## INI settings

The plugin supports the following settings in the INI file:

```ini
[unoq_source]
pub_topic = "unoq_source"  # The topic on which the plugin will publish the output of the RPC call
rpc_call = "ping"  # The RPC call to execute (the function name on MCU)
rpc_args = [] # the list of its arguments, if any
period = 1000  # The period (in milliseconds) at which the RPC call will be executed

[unoq_sink]
sub_topic = ["unoq_sink"]  # The topic on which the plugin will publish the output of the RPC call

[unoq_filter]
sub_topic = ["unoq_filter_in"]  # The topic on which the plugin will subscribe to receive the input message
pub_topic = "unoq_filter_out"  # The topic on which the plugin will publish the output of the RPC call
rpc_call = "ping"  # The RPC call to execute (the function name on MCU)
rpc_args = [] # the list of its arguments, if any
```

All settings are optional; if omitted, the default values are used.



---