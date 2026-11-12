# HDLCGW

HDLCGW is firmware for the LOLIN ESP32-S2 Mini that connects a PC, over USB or WiFi, to a
synchronous HDLC/SDLC serial line.

<!-- Badges: there is no GitHub repository for HDLCGW yet. Add the four house badges
     (build, release, licence, downloads) once RetroCoreLabs/HDLCGW exists. -->

[What it does](#what-you-can-use-it-for) · [Quick start](#quick-start) ·
[Commands](#commands) · [WiFi and TCP](#wifi-and-tcp) · [Building](#building-from-source)

An Arduino sketch, one board, two clock wires and two data wires to the line. It is the ESP32
sibling of [RC422Dev](https://github.com/RetroCoreLabs/RC422Dev) (the Raspberry Pi Pico version)
and speaks the same host protocol, so the same PC tools drive both.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [Why it exists](#why-it-exists)
- [What you can use it for](#what-you-can-use-it-for)
- [Status](#status)
- [Prerequisites](#prerequisites)
- [Wiring](#wiring)
- [Quick start](#quick-start)
- [Building from source](#building-from-source)
- [Talking to the board](#talking-to-the-board)
- [Commands](#commands)
- [WiFi and TCP](#wifi-and-tcp)
- [Current limitations](#current-limitations)
- [About HDLC](#about-hdlc)
- [Documentation](#documentation)
- [Origins](#origins)
- [Contributing](#contributing)
- [Licence](#licence)

<!-- /contents -->

## Why it exists

Old minicomputers and terminals (Norsk Data, IBM SNA and others) talk HDLC or SDLC over a
synchronous line: every bit is clocked, frames are bit-stuffed and closed with a checksum. A PC
has no such port. HDLCGW is that port: it clocks frames onto the line and takes frames off it,
and hands them to the PC as byte-stuffed frames on a USB serial port, or on a TCP socket over
WiFi so an emulator such as nd100x can reach a real line across the network.

## What you can use it for

- **USB gateway**: byte-stuffed frames (RFC 1662) from the PC go onto the line, frames off the
  line with a good checksum come back to the PC. The PC supplies and receives the checksum.
- **WiFi gateway**: the same frames on TCP port 1362 (server) or to a host you name (client).
  nd100x dials `hdlcgw.local:1362`.
- **Any bit rate from 9600 to 983000 bit/s** on the transmit clock; the receiver follows the far
  end's clock.
- **Control from a program without leaving the frame channel**: frames addressed to 0xFE carry
  commands, replies, log lines, counters and events, and never reach the line.
- **A text console** for a person with a terminal: ESC ESC ESC, then the commands below.
- **Diagnostics**: frame and byte counters, ring buffer high water, task CPU time, the last
  frames that arrived bad, a raw dump of the receive DMA ring.

## Status

Version 0.2.0 (`HDLCGW_VERSION` in `console_task.h`). No tagged release.

Measured on the bench, 2026-11-09, HDLCGW wired to an RC422Dev Pico at 983000 bit/s, 60 s per run:

- A sweep of every rate from 19200 to 983000 bit/s and frame sizes from 10 to 626 bytes over
  USB: 376 of 376 runs passed, nothing lost or damaged on either board.
- 626-byte frames both ways at once over USB: about 350 kbit/s of payload each way.
- 10-byte frames: up to 480 kbit/s Pico to ESP; about 78 kbit/s ESP to Pico (the cause of that
  difference has not been measured; nothing is lost).
- Over TCP at 983000, 626-byte frames both ways at once: 873.9 + 301.0 kbit/s, nothing lost.
  Over TCP at 307200 every 626-byte case was clean; 10-byte frames both ways (about 7000 frames
  a second) lose 0.4 % as whole frames when the CPU runs out (the host resends them).

The design rate of this phase is 307200 bit/s. 983000 works on the bench but a real link at
that rate needs the BUSY signal of the PCB 3023 MegaLink, which this phase does not have.

Open defects are listed in [BUGS.md](BUGS.md).

## Prerequisites

| # | Requirement | Needed for | How to check |
|---|---|---|---|
| 1 | LOLIN ESP32-S2 Mini | everything | shows as USB 303A:80C2 |
| 2 | Arduino IDE 2 with the ESP32 core 3.3.12 | building | Boards Manager, "esp32 by Espressif" |
| 3 | Four wires to the line (and ground) | the line | see [Wiring](#wiring) |
| 4 | A 2.4 GHz WiFi network | WiFi/TCP only | `WIFI SCAN` lists it |
| 5 | `hdlc-loopback` from [HdlcGatewayTest](https://github.com/RetroCoreLabs/HdlcGatewayTest) | testing and scripted control | `hdlc-loopback --list` |
| 6 | The [libhdlc](https://github.com/RetroCoreLabs/libhdlc) Arduino library | building | `libhdlc` is in the sketchbook's `libraries` folder |

## Wiring

| Signal | GPIO | Direction |
|---|---|---|
| Transmit clock (we generate it) | 7 | out |
| Transmit data | 11 | out |
| Receive clock (from the far end) | 12 | in |
| Receive data | 9 | in |

Loopback test on one board: wire GPIO 7 to 12 and GPIO 11 to 9.

## Quick start

Build and upload (see [Building from source](#building-from-source)), wire it as a loopback, then:

```powershell
hdlc-loopback --list                                          # which COM port is the board
hdlc-loopback --port COM10 --no-console --command "INFO"      # is it alive
hdlc-loopback --port COM10 --rates 38400 --seconds 10         # ten seconds of traffic
```

`PASS` in the result table and exit code 0 means every frame came back intact.

## Building from source

The HDLC framing, the host byte framing, the control frames and the ring buffer are in the
**libhdlc** library, shared with RC422Dev (the Raspberry Pi Pico gateway). Put libhdlc in the
sketchbook's `libraries` folder first. A clone kept elsewhere can be linked in with a directory
junction:

```powershell
New-Item -ItemType Junction -Path "$env:USERPROFILE\Documents\Arduino\libraries\libhdlc" -Target <your libhdlc clone>
```

(The sketchbook folder is whatever Arduino IDE shows under File > Preferences; on some machines
it is inside OneDrive.)

Board `esp32:esp32:lolin_s2_mini`. With the `arduino-cli` that ships inside Arduino IDE 2:

```powershell
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$cfg = "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml"
& $cli --config-file $cfg compile --clean --warnings all --fqbn esp32:esp32:lolin_s2_mini .
& $cli --config-file $cfg upload --fqbn esp32:esp32:lolin_s2_mini -p COM10 .
```

The upload writes slot 1, HDLCGW's place, and makes slot 1 start next; XMSGGW in slot 2 and the
settings stay. [Installing the firmware](../../docs/INSTALL.md) has the slots and the other ways
to install.

Build option: `USB_RX_DIRECT` (in `usb_rx.h`, default 1) reads USB input straight from TinyUSB,
so the PC is held back instead of bytes being dropped. 0 builds the Arduino core's
`Serial.read()` path, kept for comparison.

## Talking to the board

The USB serial port starts in **forwarding** mode and is binary: every byte is part of an
RFC 1662 frame (0x7E flags, 0x7D escapes). There are two ways to give it commands.

**From a program: control frames.** A frame with address 0xFE whose two check bytes are the
CRC register *not* inverted is for the firmware and never reaches the line. Types:

| PC to board | | Board to PC | |
|---|---|---|---|
| 01 HELLO | identify | 81 HELLO_REPLY | version, then key=value lines |
| 02 COMMAND | one console command line | 82 REPLY | the command's text, in chunks |
| 03 LOG_LEVEL | 0 off .. 5 trace | 83 LOG | a log line |
| 04 STAT_PUSH | send counters every N ms | 84 STAT | the counters as 32-bit numbers |
| 05 PING | empty | 85 EVENT | boot, mode change, line, clock, WiFi, TCP events |
| | | 86 NAK | refused, with a reason |

`hdlc-loopback --port COM10 --no-console --command "<command>"` sends a COMMAND frame and prints
the reply; repeat `--command` for several. The full wire format is described in libhdlc,
`extras/docs/control-frames.md`, and the code is its `control_frame.h`.

**From a terminal: the text console.** Open the port (115200 is what the build guide uses) and press ESC three times
between frames. The prompt is `HDLC#` (`HDLC@` while `DEBUG HDLC` is on, with `>` after it while `DEBUG ECHO` is on). `EXIT` goes back to forwarding mode. Programs should not
use this; use control frames.

## Commands

The same commands work in the text console and in a COMMAND control frame. Names are not case
sensitive. `HELP` prints this list on the board.

| Command | What it does |
|---|---|
| `HELP [WIFI]` | All commands, or only the WiFi and TCP ones |
| `INFO` | Build date, FreeRTOS and ESP-IDF versions, chip, heap and PSRAM free |
| `STAT` | Frame counts sent and received, line and USB byte counters, drops, ring high water, timing |
| `TASK` | Each FreeRTOS task: state, priority, stack high water, CPU time |
| `BAUD <rate>\|MAX\|DEFAULT` | Set the transmit clock, 9600..983000 bit/s (default 38400). No argument shows it |
| `CLOCK ON\|OFF` | Run or stop the transmit clock; frames wait in the buffer meanwhile |
| `F <hex bytes>` | Send an HDLC frame, checksum added. `F` alone sends a built-in 24-byte test frame |
| `SEND <hex bytes>` | Send raw bytes, no framing |
| `. [count]` | Send the bytes 0..count-1 |
| `DECODE <hex bytes>` | Decode an HDLC frame (address, control field, checksum); nothing is sent |
| `INSPECT [BAD [0..3]]` | Decode the last frame off the line, or one of the four newest bad ones |
| `DEBUG [HDLC\|ECHO ON\|OFF]` | While this console is open: `HDLC ON` cuts the line bytes into frames (checked, counted in `STAT`, kept for `INSPECT`, one line each); `ECHO ON` also prints each good frame's bytes, or with `HDLC OFF` each raw line byte (it is a `LOG 5` line; from the code, not yet seen on the board). `DEBUG` alone shows both. Neither passes frames to the PC: `EXIT` does. |
| `LOOPBACK ON\|OFF` | Loopback mode |
| `DUMP` / `CLEAR` | Dump or clear the receive buffer |
| `RXDMA` | Dump the receive DMA ring |
| `LOG [0..5]` | Log level: 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace |
| `PACKETGEN` | Packet generation mode: each byte from the PC comes back as a 6-byte frame. ESC ESC ESC returns |
| `EXIT` | Leave the console, back to forwarding frames |
| `RESET` | Restart the board |
| `FIRMWARE [SWITCH]` | Show both firmware slots; `SWITCH` restarts into XMSGGW ([Installing the firmware](../../docs/INSTALL.md)) |
| `WIFI ...`, `TCP ...`, `MIRROR ...` | See below |

## WiFi and TCP

Settings are kept in flash and survive a reset, a re-flash and a switch to XMSGGW. The network
name and passphrase are shared with XMSGGW: set them in either firmware. `BAUD` is stored too
(since 2026-11-12) and shared the same way.

| Command | What it does |
|---|---|
| `WIFI` or `WIFI STATUS` | Connection state, SSID, IP address, signal, host name, last disconnect reason; then the TCP mode and MIRROR |
| `WIFI SCAN` | List the networks the board can see, about 3 s, 2.4 GHz only |
| `WIFI SSID <name>` | Store the network name (up to 32 characters, spaces allowed) |
| `WIFI PASS <pass>` | Store the passphrase (up to 64; empty for an open network) |
| `WIFI ON` | Turn WiFi on: connect now and after every reset |
| `WIFI OFF` | Turn WiFi off: radio down, frames stay on USB |
| `WIFI CONNECT` | Drop the connection and connect again now |
| `WIFI CLEAR` | Erase all settings: WiFi off, no SSID, TCP off, MIRROR off. The network is shared, so XMSGGW loses it too |
| `TCP SERVER [port]` | Accept one TCP peer on the port (default 1362, mDNS name `hdlcgw.local`) |
| `TCP CLIENT <host> <port>` | Dial out to host:port, retry every 5 s |
| `TCP OFF` | No TCP; line frames go to USB |
| `TCP DEBUG` | The client socket, the gateway's buffers, the WiFi receive counters and lwIP's own state per connection, for fault finding |
| `MIRROR ON\|OFF` | With a TCP peer connected, send line frames to USB as well |

**First-time setup**, no network configured yet. Opening the text console (ESC ESC ESC) asks by
itself: it lists the networks it can see, takes a number from that list or a name, then the
password (shown as `*`), stores both and turns WiFi on. `NONE` means this board runs without WiFi
and stops the question (until `WIFI SSID` or `WIFI CLEAR`); Enter skips it this time. Then
`TCP SERVER`. The same by hand:

```
WIFI SCAN
WIFI SSID <name from the scan>
WIFI PASS <passphrase>
WIFI ON
WIFI STATUS
TCP SERVER
```

`WIFI STATUS` answers with four lines a program can parse:

```
WiFi: ON ssid=<name> state=CONNECTED ip=192.168.1.50 rssi=-52 hostname=hdlcgw reason=0
TCP: SERVER port=1362 listening=yes peer=none
MIRROR: OFF
TCP last drop: none, error 0, after 0 ms connected (0 drops)
```

The last line says why the gateway last dropped a TCP client (client closed, stalled for
10 s, replaced by a new client, WiFi down...). `state` is one of `OFF`, `NO_SSID`, `DISCONNECTED`, `CONNECTED`, `FAILED`, `LOST`; `rssi` is the
signal in dBm (0 when not connected); `reason` is the ESP-IDF reason code of the last
disconnect (201 means the network was not found, for example a 5 GHz-only network). The subnet
mask, gateway and DNS server are not reported.

**What the TCP stream carries**: the same RFC 1662 frames with checksum as USB, nothing else -
no idle flags and no control frames. While a peer is connected, frames off the line go to the
peer (and to USB too with `MIRROR ON`), and the peer's frames go onto the line. A new peer
replaces the old one. A peer that takes nothing for 10 s is dropped, and TCP keepalive finds
one that vanished. Commands, replies and the log always stay on USB.

**Security**: the passphrase is stored in the board's flash in plain text, and the TCP port has
no authentication. Use it on a network you trust.

## Current limitations

- A frame with a bad checksum or an abort is counted, not reported to the PC (RC422Dev does the
  same).
- The far end must send at least 4 bytes of idle ones after its last frame if it stops its
  clock, or the DMA holds the last word back.
- The board's USB driver copies data with the CPU: DMA mode is switched off in the precompiled
  Arduino core and cannot be changed from the sketch.
- One TCP peer at a time.
- WiFi is 2.4 GHz only (the ESP32-S2 has no 5 GHz radio).

## About HDLC

HDLC (ISO 13239) and IBM's SDLC send frames between 0x7E flags on a clocked line. Inside a frame,
a 0 is inserted after five 1 bits so data can never look like a flag, and a 16-bit checksum (FCS)
closes the frame. Bits go least significant first. The PC side of this gateway uses RFC 1662
byte stuffing instead, which is how PPP frames travel over an ordinary serial port.

## Documentation

- [LOOPBACK-TEST.md](LOOPBACK-TEST.md) - bench testing: wiring, the console test, the traffic
  tool, two-board tests, how to read `STAT`, and every measured result.
- [BUGS.md](BUGS.md) - open and closed defects, with causes and evidence.
- `Doxyfile` - `doxygen Doxyfile` writes the source documentation to `docs/html/`.
- `docs/` at the repository root - the ESP32-S2 datasheet.

## Origins

The bit-stuffing logic comes from Mattis Lind's SDLCBridge
([alfaskop_emu, Utils/SDLCBridge](https://github.com/MattisLind/alfaskop_emu/blob/master/Utils/SDLCBridge/SDLCBridge.ino)),
proven on real IBM SDLC hardware. The host framing, operating modes and command set follow
RC422Dev.

## Contributing

Run the bench tests in [LOOPBACK-TEST.md](LOOPBACK-TEST.md) after any change to the tasks, the
buffers or the decoder, and include the numbers.

## Licence

No licence file yet.
