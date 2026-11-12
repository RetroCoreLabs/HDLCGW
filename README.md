# HDLCGW

One small board that connects a Norsk Data machine's synchronous HDLC line to the modern world,
and the two firmwares that run on it.

The board is a WEMOS / LOLIN S2 Mini (ESP32-S2) with two MAX3490E RS-422 line drivers (data and
clock). It has a male DB-25 that plugs straight into the HDLC socket on the back of the ND machine
(the 3015 HDLC card or the 3023 MEGALINK card). The line runs at 307200 bit/s.

## What is in this repository

| Folder | What it is |
|---|---|
| [`src/HDLCGW/`](src/HDLCGW/README.md) | **HDLCGW firmware.** Passes HDLC line frames between the ND machine and a PC, over USB or WiFi/TCP. The PC does the protocol work. |
| [`src/XMSGGW/`](src/XMSGGW/README.md) | **XMSGGW firmware.** The board becomes an XMSG node of its own (node 101, called D101 on the bench). Telnet users log in to it and reach SINTRAN systems; SINTRAN users reach it with `CONNECT-TO`. |
| [`kicad/`](kicad/README.md) | **The board drawing** in KiCad 9: the schematic, the S2 Mini symbol and footprint, the wiring with the real ND pin numbers, and what is known and still open ([todo](kicad/todo.md)). There is no PCB layout yet. |
| [`docs/`](docs/README.md) | Datasheets (ESP32-S2, MAX3483-MAX3491), the [cable measurements](docs/HDLC-Cable-Measurements.md), the [3015 vs MEGALINK 3023](docs/HDLC-3015-vs-MEGALINK-3023.md) comparison, and the wiring drawings for the [PCB](docs/HDLC-PCB-ESP32-Wiring.svg) and the [old breadboard](docs/HDLC-Breadboard-Wiring.svg). |
| [`src/common/`](src/common/board.h) | Code both firmwares build in: switching between them, the button, the LEDs and the settings they share. |
| `src/external/libxmsg/` | The **libxmsg** library as a git submodule held at one exact version. It holds the XMSG protocol and, under `src/libhdlc`, the line driver, the HDLC decoder and the USB control frames. |

The board holds both firmwares at once, HDLCGW in slot 1 and XMSGGW in slot 2, and runs one of
them: HDLCGW to bridge the line to a PC program, XMSGGW to let the board be an XMSG node by
itself. `FIRMWARE SWITCH`, or the button held while the board starts, changes which one runs. The
WiFi network and the line speed are shared; each firmware keeps its other settings. How to
install them, from a release, from the browser or from source:
[Installing the firmware](docs/INSTALL.md).

## Building

1. Clone with the library: `git clone --recurse-submodules https://github.com/RetroCoreLabs/HDLCGW.git`
   (or `git submodule update --init` in an existing clone).
2. Make the library visible to Arduino: one link named `libxmsg` in your Arduino `libraries`
   folder, pointing at `src/external/libxmsg`. On Windows:
   `mklink /J libxmsg <clone>\src\external\libxmsg` run inside that folder.
3. Install the Arduino ESP32 core 3.3.12.
4. Open the sketch folder in the Arduino IDE, or from inside it:
   `arduino-cli compile --fqbn esp32:esp32:lolin_s2_mini .`

Each firmware's README explains its commands and how it was tested.
[Installing the firmware](docs/INSTALL.md) says where each one goes: **an upload from the
Arduino IDE always writes slot 1**, so XMSGGW goes to slot 2 with esptool.
