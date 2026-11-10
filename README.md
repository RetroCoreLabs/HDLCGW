# HDLCGW

One ESP32-S2 board (WEMOS / LOLIN S2 Mini) on the synchronous HDLC line of a Norsk Data machine,
and the two firmwares that run on it.

| Folder | What is in it |
|---|---|
| `src/HDLCGW/` | **HDLCGW** - passes HDLC line frames between the ND machine and a PC (USB or TCP). See its [README](src/HDLCGW/README.md). |
| `src/XMSGGW/` | **XMSGGW** - an XMSG node of its own (node 101, D101 on the bench): telnet users log in and reach SINTRAN systems, and SINTRAN users reach it with `CONNECT-TO`. See its [README](src/XMSGGW/README.md). |
| `kicad/` | the board's schematic (KiCad): the S2 Mini, two MAX3490E line drivers (data and clock), and a DB-25 that plugs into the ND machine. No PCB layout yet. |
| `docs/` | the ESP32-S2 datasheets, the board wiring and the cable measurements. |

Each sketch is an ordinary Arduino sketch: open its folder in the Arduino IDE, or build it with
`arduino-cli compile --fqbn esp32:esp32:lolin_s2_mini .` from inside it (Arduino ESP32 core
3.3.12).

Both sketches need the **libxmsg** library. It holds the XMSG protocol and, under
`src/libhdlc`, the line driver, the HDLC decoder and the USB control frames. This repository
holds it at one exact version as the git submodule `src/external/libxmsg`. Fetch it with
`git submodule update --init`, then make it visible to the Arduino IDE with one link named
`libxmsg` in the Arduino `libraries` folder that points at `src/external/libxmsg`.
