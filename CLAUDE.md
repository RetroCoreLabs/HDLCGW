# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An **Arduino sketch** for the **LOLIN ESP32-S2 Mini** that bridges a host PC to a synchronous
HDLC/SDLC line. The line has a clock per direction: SPI2 as master transmits and generates
the transmit clock, SPI3 as slave receives on the far end's clock. The HDLC bit stuffing,
flag/abort detection and FCS are done in software; the USB serial port carries RFC 1662
byte-stuffed frames to the host. It is the ESP32 sibling of RC422Dev (the Pico version at
`github.com/RetroCoreLabs/RC422Dev`) and speaks the same host protocol. The bit-stuffing logic
comes from Mattis Lind's SDLCBridge (alfaskop_emu, Utils/SDLCBridge), proven on real IBM SDLC
hardware.

There is no README, no test suite and no CI. The only commit before the 2026-11-08 rewrite is
"kinda working HDLC receiver", which used a GPIO interrupt receiver; that code is gone.

## Building and uploading

Board: `esp32:esp32:lolin_s2_mini`. The ESP32 Arduino core installed on this machine is
**3.3.12** (the only version present). There is no `arduino-cli` on PATH; use the one bundled
with Arduino IDE 2, together with the IDE's config file so it finds the installed core:

```powershell
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$cfg = "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml"

# compile; --clean matters after deleting or renaming a source file, the build cache
# otherwise keeps compiling the old copy
& $cli --config-file $cfg compile --clean --warnings all --fqbn esp32:esp32:lolin_s2_mini .

# upload (find the port with: & $cli --config-file $cfg board list)
& $cli --config-file $cfg upload --fqbn esp32:esp32:lolin_s2_mini -p COMx .

# serial console (115200; in FORWARDING mode the port is binary, press ESC three times
# for the text console)
& $cli --config-file $cfg monitor -p COMx -c baudrate=115200
```

Verified 2026-11-08: clean compile, zero warnings with `--warnings all`; uploaded and bench
tested on the board (see spi_rx_task status below).

Documentation: `doxygen Doxyfile` in this folder writes `docs/html/index.html` (ignored by
git). Doxygen is NOT installed on this machine, so the comments have not been run through it.

## Coding standard

The house C standard applies (`~/.claude/skills/c-coding-standard`, with the embedded-C
naming from `~/.claude/skills/embedded-c-style`). The parts that shape this repo:

- **File and function names align, same case.** Every function in `foo.cpp` starts with
  `foo_`; the type a file defines is named after the file (`ring_buffer`, `hdlc_encoder`,
  `byte_frame_decoder`). snake_case everywhere, no `_t` suffix, no camelCase, no classes.
- **A file that holds a FreeRTOS task is `<module>_task.cpp`**, its entry point is
  `<module>_task_run`, and every function in it starts with `<module>_task_`.
- `g_` on module state, UPPER_SNAKE constants and const tables, `type *ptr`, Allman braces,
  4 spaces, 100 columns, ASCII only, LF endings.
- Every file opens with the banner block (title in caps, WHY prose, "Ronny Hansen").
  Every function, static ones included, has a Doxygen block: `@brief`, `@param[in|out|in,out]`
  per parameter, `@return` or `@retval`.
- Keep comments. Hardware facts, citations and gotchas are load-bearing.
- The `.ino` keeps its name (Arduino requires it to match the folder) and holds only
  `setup()` and `loop()`.

## Architecture

```
 TX clock out GPIO 7, TD out GPIO 11  <- spi_tx_task (SPI2 master) <- g_tx_buffer <-+
                                                                                    |  console_task <- USB serial in
 RX clock in GPIO 12, RD in GPIO 9    -> spi_rx_task (SPI3 slave)  -> g_rx_buffer ->+   hdlc.cpp (bit level)
                                                                                    |   byte_frame.cpp (RFC 1662)
                                                                                    |   commands.cpp (DEBUG text)
                                                                                    +-> g_usb_tx_buffer -> usb_tx_task -> USB serial out
```

All four rings (line RX, line TX, USB TX, TCP TX) are 4 KB in internal RAM (see "Hot path"
below). Task priorities: spi_rx 24 (above the WiFi driver 23 and lwIP 18, see below),
spi_tx 8, console 2, usb_tx 1, WiFi 1.

- **spi_tx_task** owns SPI2 (`SPI2_HOST`) as a master through the ESP-IDF `spi_master`
  driver with DMA: SCK 7 is the transmit clock we generate, MOSI 11 is TD. No MISO, no chip
  select. Two 64-byte chunks are queued; the task sleeps on the driver until one finishes,
  refills it from `g_tx_buffer` (idle 0xFF when empty) and queues it again. LSB first, mode 0,
  clock = transmit bit rate (`BAUD` command, default 38400; a change re-creates the device
  after the queued chunks drain). The clock pauses between chunks; HDLC receivers sample on
  edges so that is harmless. **Do not go back to the Arduino `SPIClass`**: it busy-waits, so
  the task never blocked and starved the console task (seen on the bench 2026-11-08).
  **Frames enter `g_tx_buffer` in one locked block** (`ring_buffer_write_block`); pushing a
  frame byte by byte let the transmit task catch the ring half-filled and clock idle ones out
  mid-frame, which the receiver saw as an abort (also seen on the bench).
- **spi_rx_task** owns SPI3 as a slave, programmed at register level through `hal/spi_ll.h`
  because the stock ESP-IDF slave driver needs a chip-select edge to end a transaction and an
  HDLC line has none. Receive clock in on GPIO 12, RD in on GPIO 9. The slave's CS input is
  tied inside the GPIO matrix (constant 1 while arming, then constant 0 = selected forever;
  TRM 5.2.3). Segmented-transfer DMA mode with EOF-on-count off keeps the DMA walking a
  circular ring of 512 four-byte descriptors (one DMA word each, so a completed word reaches
  software at once; 64-byte buffers held a whole Pico frame hostage, see the file comment and
  LOOPBACK-TEST.md section 6). Buffers are pre-filled with 0xFF and the task also takes a
  word the DMA has written but not yet announced ("peek", counted in STAT), because the DMA
  closes a descriptor only when the next transfer starts. Even so the DMA holds the newest
  complete word plus any partial word until one more byte arrives, so a far end that stops
  its clock after the closing flag (RC422Dev) must send at least 4 bytes of fill, ones per
  Ronny's ruling 2026-11-08, not flags; the task polls the descriptor owner bit every
  1 ms, copies filled buffers to `g_rx_buffer`, drops repeated idle 0xFF bytes, and counts
  FIFO overruns. **Status: verified on the bench 2026-11-08**, 24 of 24 loopback frames good
  at 9600, 38400 and 983000 bit/s. The file banner lists the TRM sections each register
  setting rests on. Bench test: wire GPIO 7 to 12 and GPIO 11 to 9 on the same board,
  ESC ESC ESC, `HDLC`, `F`, expect "HDLC frame, 24 bytes, FCS good"; `STAT` shows the line
  RX counters. **Both receive pins carry the IO MUX glitch filter** (25 ns,
  `gpio_new_pin_glitch_filter`): with the Pico as the far end over jumper wires, 1 frame in
  about 200 arrived with exactly one bit read as 0 instead of 1, length intact, and with the
  filter on six 60 s runs were 4990 of 4990 clean (LOOPBACK-TEST.md section 8c). Keep it.
  The two-board link to RC422Dev is clean in both directions since 2026-11-08 23:51; the
  ESP-to-Pico loss was a Pico SDK lock refusing a same-core caller under FreeRTOS (section 8c
  tells the story), nothing in HDLCGW.
- **console_task** owns the USB serial port's input. Three modes (`g_console_mode`), the same
  three as RC422Dev: `CONSOLE_FORWARDING` (default, binary): host bytes go through
  `byte_frame_decoder`, each complete frame is bit-stuffed onto the line as-is (the host
  supplies the FCS); line frames with a good FCS are byte-stuffed back to the host, FCS
  included. Three ESC bytes between frames switch to `CONSOLE_DEBUG`, the text console; `EXIT`
  switches back; `PACKETGEN` enters `CONSOLE_PACKET_GEN`, where every host byte becomes a
  6-byte frame back to the host and ESC ESC ESC returns to forwarding. Frame counters in
  `g_sent_stats` / `g_received_stats` (`STAT`). A frame for the line is encoded in full and
  enters `g_tx_buffer` in ONE locked write; a frame for the host goes to `g_usb_tx_buffer`,
  never to `Serial` directly (both rules were bench failures, see the file banners).
  Host input is read in blocks of at most 256 bytes per loop pass, never "while
  (Serial.available())": with the PC writing non-stop that loop never returned, the line RX
  ring was never drained again and two-way 983000 runs delivered 0 frames B->A (2026-11-09).
- **Host interface rule (Ronny, 2026-11-08 evening): programs talk to the firmware with
  control frames, never with ESC ESC ESC.** The text console is the hatch for a person with a
  terminal, nothing else. For bench work from a script use the PC tool's framed path
  (`hdlc-loopback --port COMx --no-console --command STAT ...`, or its MCP server), not
  `arduino-cli monitor`. RC422Dev is to get the same control frames so both gateways are
  driven alike; until then it is the one place the console is still needed by a tool.
- **Control frames** (`control_frame.cpp`, `console_out.cpp`; written 2026-11-08 by a second
  session at Ronny's request, wire format decided by Ronny): a byte-stuffed frame addressed
  to 0xFE whose two check bytes are the CRC register NOT inverted (residue 0x0000, a data
  frame's good FCS leaves 0xF0B8, so the two can never collide) is for the firmware itself
  and never reaches the line. Types host to firmware: HELLO 01, COMMAND 02 (a console line),
  LOG_LEVEL 03, STAT_PUSH 04, PING 05; firmware to host: 81..86 (HELLO_REPLY, REPLY chunks,
  LOG, STAT as u32 list in `console_out.cpp` order, append only, EVENT codes 1..7 with 16+
  reserved for WiFi, NAK). Parsed in FORWARDING only, on purpose: a host that finds a board
  left in DEBUG sends EXIT through the console and then HELLO again (the PC tool does that),
  so HELLO is not parsed from keystrokes. All console text goes through `console_out_*`,
  which prints in DEBUG and collects REPLY chunks inside a control command. The C# twin is
  `ControlFrame.cs` in the HdlcGatewayTest repository; change both or neither, and tell the
  PC side before flashing.
- **Static RAM is capped near 122 KB** on this core build although the sketch reports more
  dynamic memory free: a version of `console_out` with 3 KB more static buffers overflowed
  `dram0_0_seg` by 1232 bytes at link time, and the WiFi stack alone adds 47 KB of static
  data. Large buffers off the per-byte path (the bad-frame history) are
  therefore allocated from the board's 2 MB PSRAM (`ring_buffer_alloc()`, BOARD_HAS_PSRAM is
  set for `lolin_s2_mini`). The SPI DMA descriptors and buffers must stay internal
  (`DRAM_ATTR`).
- **Hot path in internal RAM (Ronny's ruling 2026-11-09)**: everything every line or USB byte
  passes through runs from the chip's internal SRAM, not from flash or PSRAM through the
  cache. `hot_path.h` gives `HOT_PATH_CODE` (IRAM_ATTR plus noinline: an inlined copy lands in
  its caller, which may be flash) and `HOT_PATH_DATA` (DRAM_ATTR, for the CRC table); both are
  empty off the ESP so `hdlc.cpp` still builds for PC tests. Marked: the ring buffer read and
  write functions, the HDLC bit encoder and decoder and CRC, the byte-stuffing encoder and
  decoder, and every per-byte loop in the tasks (`fill_chunk`, `push_bytes`,
  `handle_line_bytes`, `send_line_frame_locked`, `handle_forwarding_input`,
  `usb_tx_task_fill_piece`). The line RX, line TX and USB TX rings are 4 KB each from
  `ring_buffer_alloc_internal()`, which fails rather than fall back to PSRAM. Placement was
  checked in the ELF with `xtensa-esp32s2-elf-nm` (internal IRAM is 0x4002xxxx..0x4006xxxx,
  flash code 0x4008xxxx and up); do that again after adding a hot function. Task stacks were
  cut to measured use plus at least 1 KB (`HDLCGW.ino` comment) to pay for it. Measured after:
  about 17 KB internal heap free, 16 KB lowest; STAT reports ring high water and the lowest
  internal heap since boot (fields 29..32).
- **wifi_task** (2026-11-08, Ronny's decision and shape): WiFi station with SSID, passphrase,
  on/off, TCP mode, host, port and mirror in NVS (Preferences namespace `hdlcgw`,
  `WiFi.persistent(false)` so the driver itself never writes flash). TCP SERVER on 1362 by
  default (nd100x dials `hdlcgw.local:1362`, mDNS service `_hdlc._tcp`), CLIENT mode dials
  host:port with a 5 s retry. The TCP stream is pure RFC 1662 frames with FCS, exactly like
  nd100x's `--hdlc` port: no idle flags, never a control frame. With a peer connected, line
  frames go to the peer (and to USB too only with `MIRROR ON`) and the peer's frames go to
  the line through `console_task_send_line_frame`, which is mutex-protected for its two
  callers; without a peer USB gets the line frames as before. A new TCP client replaces the
  old one. Console, control frames and the log stay on USB. Events 16..23 (`control_frame.h`)
  report the state; `WIFI STATUS` prints three key=value lines the PC side parses. Commands:
  `WIFI SSID|PASS|ON|OFF|CONNECT|STATUS|CLEAR`, `TCP SERVER [port]|CLIENT <host> <port>|OFF`,
  `MIRROR ON|OFF`. The station is polled, not event-driven, so every EVENT frame is sent from
  the WiFi task. Build with WiFi: 977 KB of the 1,310 KB app slot, 102 KB static as the
  sketch reports it, and that figure is already at the real cap: adding a 4 KB static array
  on top of it overflowed `dram0_0_seg` by 768 bytes (the WiFi stack's own data is not in
  the reported number). Nothing static may be added any more; cold buffers go to PSRAM. Internal
  heap with the station up and TCP listening: about 17 KB free since the hot path moved in
  (2026-11-09; 21 KB before). Measured with 10-byte TCP traffic both ways at 983000: lowest
  7.5 KB, because received packets copied by `wifi_rx_copy()` wait in lwIP's heap. That is the
  next thing to run out; before adding anything that allocates, measure with INFO.
  Verified 2026-11-08: joins a 2.4 GHz network (the S2 has no 5 GHz radio, so a 5 GHz-only
  SSID shows as reason 201 and never connects; `WIFI SCAN` lists what the board can see),
  `hdlcgw.local` resolves on Windows, a frame sent to port 1362 from the PC goes onto the
  line, settings survive a reset and the station reconnects by itself.
- **Line TX chunk length follows the bit rate** (2026-11-09, BUGS.md B4): each DMA transaction
  is about 2 ms of line time (244 bytes at 983000, at least 8 bytes at low rates), from the
  DMA-capable internal heap. A fixed 64-byte chunk meant about 1900 driver transactions a
  second at 983000 and 12.6 percent of the CPU, which left no headroom for 10-byte frames
  both ways (about 7000 frames a second): first the USB TX ring dropped frames, and giving
  the USB TX task more priority only moved the loss to the line RX ring. With the 2 ms chunk
  the task takes about 5 percent and those runs lose nothing. The single core is the limit
  at that frame rate; measure with TASK before adding per-frame work.
- **Inter-frame fill on the line**: after the closing flag the encoder pads its last byte
  with 1..7 ones and `console_task_send_line_frame` adds one whole idle byte, so two flags are
  never fewer than 8 ones apart. ISO 13239 fill is flags or idle; 1..6 ones between flags is
  undefined and a real receiver (RC422Dev, 2026-11-08) made a phantom flag out of six pad ones
  and lost every second frame. Do not remove that byte.
- **usb_rx** (2026-11-09, Ronny's decision): host bytes are read straight from TinyUSB with
  `tud_cdc_n_read()`, not through `Serial.read()`. The core's USBCDC object copied every packet
  into its own queue byte by byte and threw away what did not fit after a 10-tick wait, so
  frames went onto the line short with the PC's FCS (BUGS.md B1, proven byte for byte), and
  the copying took over a third of the CPU (B2). `usb_rx_begin()` sets the core's
  `devices[0]` to NULL after `Serial.begin()`, which stops all the core's CDC callbacks;
  TinyUSB 0.21.0 then re-arms the receive endpoint only when a whole packet fits, so the PC
  is paced by NAK. `usb_rx_poll()` hands TinyUSB's line state and line coding to the core's
  own `_onLineState` / `_onLineCoding` every pass, which keeps the 1200-baud upload restart
  working (re-tested by a second upload). Build flag `USB_RX_DIRECT` (usb_rx.h, default 1;
  0 = the old path, kept to compare). Side effect: core `log_e` output to USB is gone too,
  which is right for a binary port. The 8 KB core queue is no longer allocated (internal heap
  16.3 KB to 24.6 KB free). The USB driver itself copies the hardware FIFO with the CPU: the
  precompiled core has TinyUSB's DWC2 DMA mode off and that cannot be changed from the sketch.
- **TCP path fixes (2026-11-09, BUGS.md T2/T3, all measured on the bench):**
  - **WiFi receive copy** (`wifi_rx_copy()`, build flag `WIFI_RX_COPY`, default 1): the
    core's ESP-IDF is built without `CONFIG_LWIP_L2_TO_L3_COPY`, so lwIP kept the WiFi
    driver's receive buffer for every queued packet, and TCP's out-of-order queue is
    unlimited. A PC sending 10-byte frames (one ~19-byte segment each, Nagle off) filled all
    32 driver buffers with 32 queued segments after one loss; the retransmission had no
    buffer to arrive in, the connection froze, the ESP missed FIN and RST, once the whole IP
    stack went silent. The copy (private ESP-IDF API `esp_wifi_internal_reg_rxcb`,
    re-registered each time the station gets an IP) returns the driver buffer at once. Ties
    the sketch to this core version; check it after a core update with `TCP DEBUG`.
  - **Line receive task at priority 24**: at 9 the WiFi driver and lwIP held it off up to
    97.6 ms and the receive DMA ring (2048 bytes, 16.7 ms at 983000) wrapped over unread
    words: unseen and torn frames with no counter moving. `STAT` "Line RX passes" shows the
    longest gap and the most descriptors in one pass; over 512 means the ring wrapped.
  - **WiFi task never sleeps on a full ring or socket**: peer frames wait for line room
    without blocking (`spi_tx_task_has_room()`), the socket is written with `send()` and
    `MSG_DONTWAIT` (the core's `NetworkClient::write()` retries a full socket for up to 10 s).
    A peer that takes nothing for 10 s is dropped; TCP keepalive (5 s idle, 1 s x5) finds a
    vanished one. `WIFI STATUS` prints the reason for the last drop.
  - `TCP DEBUG` prints the peer socket, our buffers, the receive-copy counters, the radio
    mode and lwIP's own state per connection (rcv_nxt, window, refused and out-of-order
    bytes, segments and pbufs). STAT fields 47..50: socket full, waits for line room,
    stalled peers dropped, TCP ring high water.
  - **Design rate 307200 (Ronny, 2026-11-09)**: 983000 needs the PCB 3023 MegaLink BUSY
    signal, not in this phase. Whole frames dropped when the CPU runs out are accepted (the
    host resends); BUGS.md L1 has the numbers.
- **usb_tx_task** is the only writer of binary data to `Serial`. The core's USB CDC write
  blocks and spins per 64-byte piece while the host is slow; done from console_task it starved
  the line receive ring at 983 kbit/s. A full `g_usb_tx_buffer` drops whole frames and counts
  them (`usb_tx_task_dropped`, shown by `STAT`). Measured ceiling of the board-to-PC path on
  2026-11-08: about 270 kbit/s of payload, independent of the line rate.
- **commands.cpp**: the command set is RC422Dev's, so both gateways are driven alike: HELP,
  SEND, BAUD (any 9600..983000, MAX, DEFAULT; transmit clock only), LOOPBACK, ECHO, DUMP,
  CLEAR, `.`, HDLC, F (`F <hex>` adds FCS, dumps the control field and sends; `F` alone sends
  the built-in 24-byte test frame, an HDLCGW addition), TASK, INFO, STAT, RESET, INSPECT (last
  frame off the line), DECODE, EXIT, PACKETGEN, plus CLOCK ON|OFF (HDLCGW addition: stops
  and restarts the transmit clock so the line can be made quiet; frames wait in the ring
  meanwhile). Output texts match RC422Dev's where the host
  tool parses them ("Baud rate set to", the `#`/`@` prompt). The control-field decoder is in
  `hdlc.cpp` (`hdlc_decode_control`); RC422Dev's has a bug that never decodes REJ/SREJ, ours
  uses the right values 2 and 3.
- **Decoder minimum-frame rule**: a flag-to-flag run shorter than `HDLC_MIN_FRAME_BYTES` (4,
  ISO 13239 "invalid frame") is ignored and counted in `hdlc_decoder.short_frames`, not
  reported as bad. The transmitter pads the byte after a closing flag with idle ones, so when
  the next frame follows at once those ones plus the opening flag form a phantom one-byte
  frame; without the rule `STAT` showed 1339 BAD in 5521 good frames. The sink has a fourth
  callback, `on_discard`, for exactly this: the first version of the rule dropped the run in
  the decoder but left the byte in the sink's buffer, and every following frame reached the
  host with that byte stuck on the front (91 of 155 frames bad at the host, bench 2026-11-08).
  `hdlc-loopback --verbose 3` is what showed it: "got 127 bytes, header says 126, first
  difference at offset 1".
- **Bench testing**: `LOOPBACK-TEST.md` in this folder is the guide (wiring, console test,
  the `hdlc-loopback` traffic tool from the `HdlcGatewayTest` repository, two-gateway wiring to
  RC422Dev, how to read `STAT`, measured numbers). Run it after any change to the tasks, the
  rings or the decoder.
- **hdlc.cpp** is pure C-style code with no Arduino dependency, so it can be unit tested on a
  PC. Encoder: `open` / `write` / `write_fcs` / `close` / `abort`, each returning 0..2 line
  bytes into a caller buffer. Decoder: `feed` one line byte, frames delivered through an
  `hdlc_frame_sink` of three callbacks. Bits are LSB first on the wire; the decoder re-aligns
  on every flag so wire byte alignment does not matter. Two behaviours are deliberately copied
  from SDLCBridge and must stay: a stuffed zero before the closing flag after five ones, and
  the shift-right/LSB-first build of output bytes.
- **ring_buffer** is mutex-guarded and may only be used from tasks, never from an interrupt.
- `Doc/` holds the ESP32-S2 datasheet PDF; that is the authority for peripheral behaviour,
  not memory of other ESP32 variants.

## Things to know before changing code

- Transmit pins come from the board's `pins_arduino.h` via `SCK`/`MOSI`; receive pins are
  plain numbers in `spi_rx_task.cpp`. Check the variant file (core 3.3.12,
  `variants/lolin_s2_mini`: SS 12, MOSI 11, MISO 9, SCK 7) before moving a transmit signal.
- **Pins and SPI functions are not fixed on this chip.** The "SCK 7 / MISO 9 / MOSI 11 /
  SS 12" set is only the Arduino variant's default; the chip's own IO MUX (datasheet v1.9,
  Table 2-3, page 17) puts SPI2 on GPIO 12 = FSPICLK, 11 = FSPID (MOSI), 13 = FSPIQ (MISO),
  10 = FSPICS0, 9 = FSPIHD, and gives SPI3 no IO MUX pin at all. Every signal can instead be
  routed to any GPIO through the GPIO matrix (datasheet 2.3.1, 2.3.6, TRM chapter 5), which is
  what both tasks do, so GPIO 12 is the receive clock input here, not a chip select. Section
  4.2.1.2 (page 39): SPI2 and SPI3 both do master and slave, full duplex, DMA, slave clock up
  to 40 MHz.
- **Talking to the board from a script**: use the Arduino `monitor` command (it opens the
  port the way the IDE does). The .NET `System.IO.Ports.SerialPort` never made the core's
  USB CDC report the port as open (the `while (!Serial)` that `setup()` had at the time
  waited forever), and one open/close sequence from it matched the core's DTR/RTS "enter
  bootloader" pattern (`USBCDC::_onLineState`), leaving the board in ROM download mode (USB
  PID 0002) until the next upload.
- **The line starts at power-up**, host or no host (Ronny's decision 2026-11-08). `setup()`
  does not wait for the USB port to be opened; the banner is only seen by a host that is
  connected at boot.
- The ESP32-S2 Technical Reference Manual (Version 1.3) is the authority for the receiver's
  register programming. It is not in the repo (10 MB); fetch it from Espressif when needed.
  Sections used: 5.2.3 (GPIO matrix constant inputs), 24.5 (slave mode), 24.8 (DMA), 24.11.
- `Backup-08112026.zip` in the folder is Ronny's own backup of the pre-rewrite tree. Not part
  of the build; do not commit or delete it.
- Known gap: there is no FORWARDING-mode reply for a frame with a bad FCS or an abort; it is
  only counted. RC422Dev behaves the same way.
- The USB CDC receive queue (the core's, 8192 bytes in `setup()`) is only used when
  `USB_RX_DIRECT` is 0; the default build reads TinyUSB directly (see usb_rx above).
- `Doc/` also holds nothing from the tool repository; the host-side test tool lives in
  `HdlcGatewayTest` (RetroCoreLabs on GitHub) and is C#. Do not add host test programs here.
