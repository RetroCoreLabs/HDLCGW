# Testing the DB-25 connection with the Pico, before the KiCad board

For the person about to wire a Raspberry Pi Pico to the 25-pin HDLC socket of a Norsk Data
ND-100 and prove that it works. The KiCad board (`kicad/`) copies this wiring, so every pin
proved here is one less guess on the board. Do the steps in order; each one only needs the
ones before it.

Every fact below says where it comes from. **Not verified** means nobody has measured it yet,
and this test is how we find out.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [1. Read this first: the cable flips the pin numbers](#1-read-this-first-the-cable-flips-the-pin-numbers)
  - [Look-up table: the numbers used in this test](#look-up-table-the-numbers-used-in-this-test)
- [2. What you need](#2-what-you-need)
- [3. The wiring](#3-the-wiring)
- [4. Checks before anything is powered](#4-checks-before-anything-is-powered)
- [5. Test 1 - the Pico on its own, then through the chips (no ND)](#5-test-1---the-pico-on-its-own-then-through-the-chips-no-nd)
- [6. Test 2 - against the real ND](#6-test-2---against-the-real-nd)
- [7. Test 3 - measure the clock edge (settles assumption A4)](#7-test-3---measure-the-clock-edge-settles-assumption-a4)
- [8. Results to fill in](#8-results-to-fill-in)
- [9. Done means: ready for the KiCad layout](#9-done-means-ready-for-the-kicad-layout)
- [Related documents](#related-documents)

<!-- /contents -->

## 1. Read this first: the cable flips the pin numbers

The 1-metre "DB 25 M to M" cable used on the bench (bought on AliExpress) has its flat cable
turned over at ONE end. The seller does not say so. Ronny found it. (Source: `kicad/README.md`,
"The cable that confused the measurements".)

What that means in practice:

- **Through this cable, pin N at one end is NOT pin N at the other end.** Each row is read
  backwards: the pin that is first in a row at one end is last in that row at the other.
- The formula, from the beep measurements in `HDLC-Cable-Measurements.md`:
  - top row (13 pins, printed 1 to 13): **real pin = 14 - printed pin**
  - bottom row (12 pins, printed 14 to 25): **real pin = 39 - printed pin**
- "Printed" = the number printed on the breakout board at the far end of the cable.
  "Real" = the ND's pin number, as in the ND manuals.
- How it was found: the beeps on the breakout matched none of the expected patterns, but
  matched all four of the ND card's straps exactly once the numbers were mirrored. That fits
  every measurement so far. **It has not been checked pin by pin**, and ground (real 8 =
  printed 6) **has not been tested**. Section 4 checks both.
- **The KiCad board does not use this cable**: it plugs straight into the ND socket, so
  there is no flip there. All board drawings use REAL numbers. Only the breadboard diagram
  (`HDLC-Breadboard-Wiring.svg`) uses printed numbers.

### Look-up table: the numbers used in this test

| Signal (from the ND's side) | Real ND pin | Printed pin through the flipped cable |
|---|---|---|
| Data OUT of the ND, plus (TXD+) | 14 | 25 |
| Data OUT of the ND, minus (TXD-) | 2 | 12 |
| Data INTO the ND, plus (RXD+) | 16 | 23 |
| Data INTO the ND, minus (RXD-) | 4 | 10 |
| Clock OUT of the ND, plus (TXC+) | 3 | 11 |
| Clock OUT of the ND, minus (TXC-) | 15 | 24 |
| Clock INTO the ND, plus (RXC+) | 18 | 21 |
| Clock INTO the ND, minus (RXC-) | 6 | 8 |
| Ground | 8 | 6 |
| Strap 3-5 | 3, 5 | 11, 9 |
| Strap 15-17 | 15, 17 | 24, 22 |
| Strap 11-24-23 | 11, 24, 23 | 3, 15, 16 |
| Strap 22-10 | 22, 10 | 17, 4 |
| Busy INTO the ND, plus (RBY+), MEGALINK only | 5 | 9 |
| Busy INTO the ND, minus (RBY-), MEGALINK only | 17 | 22 |
| Busy OUT of the ND, plus (TBY+), MEGALINK only | 19 | 20 |
| Busy OUT of the ND, minus (TBY-), MEGALINK only | 7 | 7 |

Watch out: printed 22 is real 17 (busy), and printed 17 is real 22 (strap). Real 7 is printed 7.

All 25 pins, from the formula above (printed = 14 - real for real 1 to 13, printed = 39 - real
for real 14 to 25):

| Real | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Printed | 13 | 12 | 11 | 10 | 9 | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 |

| Real | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 | 25 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Printed | 25 | 24 | 23 | 22 | 21 | 20 | 19 | 18 | 17 | 16 | 15 | 14 |

**PCB (straight plug into the ND): use the Real column. Breadboard through the flipped cable:
use the Printed column.** The PCB has no cable, so it has no flip.

Signal names and signs come from the ND Service Handbook (3023 MegaLink table, section 2-22,
page 5 of 5). The 3015 HDLC card uses the same data and clock pins (section 2-20, page 7 of 8),
but its table gives no plus or minus signs; that the signs are the same there is **not verified**.
The OCR tables of cables 325402 (HDLC) and 325400 (MEGALINK), given by Ronny on 2026-11-25, are in
`HDLC-3015-vs-MEGALINK-3023.md`. Both cross data and clock on the same pins (14-16, 2-4, 15-18,
3-6).

**Rule for the whole test:** write down every number as a REAL ND number, and say "printed"
whenever you mean the number on the breakout. Mixing the two is how this went wrong before.

## 2. What you need

- A Raspberry Pi Pico running RC422Dev firmware (repository RC422Dev). It answers
  `RC422Dev 0.2.0` or later on USB.
- Two MAX3490E chips (3.3 V, full duplex, no enable pins), the same chip the board will use.
  Through-hole: MAX3490EEPA. Pins: 1 VCC, 2 RO, 3 DI, 4 GND, 5 Y, 6 Z, 7 B, 8 A (from the
  datasheet; `kicad/README.md`). One 100 nF capacitor per chip from VCC to GND.
  (The old breadboard used an AM26LS31 + AM26LV32 with a level shifter. Do not mix the two
  set-ups. Using the MAX3490E proves the board's chip.)
- A breadboard, jumper wires, a multimeter with a beep mode.
- Either the flipped cable plus a DB-25 breakout, or a male DB-25 that plugs straight into
  the ND's female socket (the ND side is female; Ronny's statement, **not verified**).
- The HDLC Gateway tool on the PC (`hdlc-gateway-tool.exe`, `hdlc-loopback.exe`, repository
  HdlcGatewayTest) to talk to the Pico.
- For step 7: a logic analyser or a two-channel scope.

## 3. The wiring

Pico pins from `src/sync.cpp` in RC422Dev: **GP14 = data out, GP15 = clock out, GP16 = data in,
GP17 = clock in**. The receive program needs the clock on the pin right after the data pin, so
do not move GP16/GP17. Chip wiring as on the board (`kicad/README.md`, "Wiring"), with the
S2 Mini's pins replaced by the Pico's.

| Chip | Chip pin | Goes to | Real ND pin | Printed (flipped cable) |
|---|---|---|---|---|
| U1 (data) | 3 DI | Pico GP14 | - | - |
| U1 | 2 RO | Pico GP16 | - | - |
| U1 | 5 Y | data INTO the ND, plus | 16 | 23 |
| U1 | 6 Z | data INTO the ND, minus | 4 | 10 |
| U1 | 8 A | data OUT of the ND, plus | 14 | 25 |
| U1 | 7 B | data OUT of the ND, minus | 2 | 12 |
| U2 (clock) | 3 DI | Pico GP15 | - | - |
| U2 | 2 RO | Pico GP17 | - | - |
| U2 | 5 Y | clock INTO the ND, plus | 18 | 21 |
| U2 | 6 Z | clock INTO the ND, minus | 6 | 8 |
| U2 | 8 A | clock OUT of the ND (see note) | 15 | 24 |
| U2 | 7 B | clock OUT of the ND (see note) | 3 | 11 |
| both | 1 VCC | Pico 3V3(OUT) (pin 36) | - | - |
| both | 4 GND | Pico GND | 8 | 6 |

**Note on the clock receiver (assumption A4, not verified):** A on 15 and B on 3 is the
"wrong way round" by the signs: the ND's clock plus is pin 3. It is wired this way on purpose.
The ND-to-ND cable in the manual crosses the clock (pin 3 to 6, 15 to 18), so wired like this
the Pico listens the way a second ND would. With RC422Dev reading on the rising edge of GP17,
the Pico then samples on the falling edge of the ND's clock plus. Whether that is the right
edge is what step 7 measures. If frames from the ND fail, swapping the two clock wires on U2
pins 7 and 8 is the first thing to try. A swapped pair does no damage. The same can be done in
firmware without touching a wire: see step 7.
(The cable OCR tables in `HDLC-3015-vs-MEGALINK-3023.md` show this crossing for both cables:
15 to 18 and 3 to 6.)

**The straps** (3-5, 15-17, 11-24-23, 22-10) are the 3015 card's computer-link straps in the
cable plug. What they do is not written in the pages read so far (`kicad/todo.md` item 5).
Leave them as wire links you can pull out, and run step 6 with and without them.

Nothing else on the plug is connected in this HDLC test: real pins 1, 7, 9, 12, 13, 19, 20, 21,
25 (printed 13, 7, 5, 2, 1, 20, 19, 18, 14). For the MEGALINK test with busy, see section 10.

## 4. Checks before anything is powered

Everything unplugged, meter in beep mode. Write the answers in section 8.

1. **Is the flip real?** With the cable and breakout connected, beep real pin 1 at the ND end
   of the cable against the breakout. The flip predicts printed 13. Then real 14 -> printed 25,
   real 25 -> printed 14. Three beeps prove the formula for both rows.
2. **Ground**: beep real 8 at the ND end against the breakout. The flip predicts printed 6. If
   it beeps with printed 8 instead, the flip formula is wrong: stop and re-measure everything.
3. **No shorts inside a pair**: 14-2, 16-4, 3-15, 18-6 must NOT beep with each other.
4. **Each wire lands on its chip pin**: every row of the table in section 3, chip pin against
   plug pin.
5. **Power**: 3V3 to GND is not a short, each chip's pin 1 reads 3V3 and pin 4 reads GND.
   Nothing on the board may carry 5 V into a Pico pin (the Pico pins take 3.3 V only).

## 5. Test 1 - the Pico on its own, then through the chips (no ND)

This proves the Pico, the chips and the wiring before an ND is involved.

1. Plug the Pico into USB. Find its COM port: `hdlc-loopback --list`. Open it in the window
   (pane A) and run `STAT` and `BAUD`. Expected: `RC422Dev`, `Current baud rate: 307200`
   (307200 bit/s is the design speed; set it with `BAUD 307200` if not).
2. **Loopback through the chips with a loop plug.** Instead of the ND, wire a spare DB-25 so
   that what the Pico sends comes straight back, keeping plus to plus:
   real 16 to 14, 4 to 2, 18 to 15, 6 to 3. (Data: U1 Y -> U1 A, Z -> B. Clock: U2 Y -> U2 A,
   Z -> B, which matches the A4 wiring above.) If the loop plug goes through the flipped cable,
   use the printed numbers from the table.
3. Run the traffic test from the PC (it sends frames and counts what comes back):
   ```
   hdlc-loopback --port COM18 --rates 307200 --seconds 30
   ```
   (use your Pico's COM port). The traffic test is documented for one board looped back; that it
   runs the same way on the Pico alone is **not verified** - if it refuses, use the Pico's own
   `loopback` console command instead. Expected: `PASS`, exit code 0, and in `STAT`: sent = received,
   bad CRC 0, odd bits 0, aborted 0.
4. Repeat with 626-byte and 4105-byte frames (`--size 626`, `--size 4105`). 4105 bytes is the
   largest frame the gateways accept.

If this fails: check section 4 again, then swap one pair at a time. A passing loop plug means
the plug wiring and both chips work. It does not prove the clock edge against a real ND.

## 6. Test 2 - against the real ND

1. Power the ND off. Plug the cable (or the straight plug) into the HDLC socket. Power on.
2. Pico in pane A, log level up (`LOG` on the Pico if your build has it), frame list open.
3. Bring the line up from the ND side as you normally do for this card. (How the ND side is set
   up for a computer link with this Pico is **not written down anywhere yet**. Record exactly
   what you did in section 8.)
4. What good looks like:
   - frames from the ND appear in the frame list (the first is usually a link set-up frame,
     SABM), and the Pico's `STAT` counts them as good;
   - `RX line decoder: ... bad CRC 0, odd bits 0`;
   - the Pico's answers reach the ND (the ND side stops resending the same frame).
5. Do the run with the straps in and with the straps out. Write down both.

If the Pico sees nothing, or only bad frames:
- swap the clock pair on U2 pins 7 and 8 (this is the A4 assumption);
- then swap the data pair on U1 pins 7 and 8;
- one change at a time, re-run, write down each result.
- Instead of swapping wires, the clock edges can be changed in firmware (step 7).

## 7. Test 3 - measure the clock edge (settles assumption A4)

The clock edges are a firmware setting, not a wiring question. There are only four
combinations, and each direction can be tested on its own:

| Direction | Proven when | Firmware setting |
|---|---|---|
| Receive (ND to us) | frames from the ND arrive with bad CRC 0, odd bits 0 | which edge of the receive clock the data is read on |
| Transmit (us to ND) | the ND accepts our frames and stops resending | which edge of the transmit clock the data changes on |

Where the setting is:
- ESP32-S2 firmware (read 2026-11-25): both directions use SPI mode 0.
  - Receive: `src/external/libxmsg/src/libhdlc/esp32s2/spi_rx_task.c` line 80,
    `#define RX_SPI_MODE 0`, applied at line 292. Data is read on the rising edge of GPIO 12.
    Line 275, `esp_rom_gpio_connect_in_signal(PIN_RX_CLOCK, SPI3_CLK_IN_IDX, false)`: `true`
    inverts the receive clock in the GPIO matrix.
  - Transmit: `src/external/libxmsg/src/libhdlc/esp32s2/spi_tx_task.c` line 249, `dev.mode = 0`.
    Data changes on the falling edge of GPIO 7 and is steady on the rising edge. Mode 1 swaps
    that.
- Pico (RC422Dev): reads on the rising edge of GP17 (section 3 note). Where the transmit edge
  is set in RC422Dev is **not checked**.

**The rule: "works" must mean "works with margin".** If we read on the same edge on which the
data changes, we read exactly while the data switches. That can pass at 307200 on a short
bench wire and still fail at 983000, with a longer connection, or warm. The right edge reads in
the middle of each bit. So:
1. Find a setting that works at 307200.
2. Run it at the highest rate you will use (983000 for MEGALINK), with 4105-byte frames, for
   at least 10 minutes. Bad CRC must stay 0.
3. If it only works at the low rate, change the edge of that direction and repeat.

**With a scope or logic analyser (the quickest proof):** with the ND sending, probe real pin 14
(data plus) and real pin 3 (clock plus), ground on 8. Through the flipped cable that is printed
25, printed 11 and printed 6. Look at which clock edge the data changes on. The data must be
steady at the edge we read. Write down: "data changes on the rising / falling edge of pin 3".
Also measure the clock rate on pin 3 (expected 307200 Hz if the ND clock is set to that;
**not verified**).

## 8. Results to fill in

| Check | Result | Date |
|---|---|---|
| 4.1 real 1 / 14 / 25 at the ND end beep with printed | | |
| 4.2 real 8 (ground) beeps with printed | | |
| 4.3 no short inside any pair | | |
| 4.4 every chip pin to plug pin | | |
| 4.5 power checks | | |
| 5.3 loop plug, 30 s at 307200, default size | sent / received / bad | |
| 5.4 loop plug, 626 and 4105 bytes | | |
| 6 ND: how the ND side was set up | | |
| 6 ND, straps IN: frames seen / good / bad | | |
| 6 ND, straps OUT: frames seen / good / bad | | |
| 6 pairs swapped (which, and did it help) | | |
| 7 data changes on which edge of pin 3 | | |
| 7 clock rate on pin 3 | | |
| 7 receive edge setting that works, and at which top rate, how long | | |
| 7 transmit edge setting that works, and at which top rate, how long | | |

## 9. Done means: ready for the KiCad layout

The connection is proved, and the board can be laid out, when:

- the flip formula and ground are confirmed by beeps (or corrected in
  `HDLC-Cable-Measurements.md` and `kicad/README.md`);
- the loop plug passes at 307200 with 4105-byte frames;
- the ND's frames arrive with 0 bad and the ND accepts the Pico's answers;
- the clock edge is measured, and the clock pair's wiring (A4) is confirmed or changed;
- it is known whether the straps are needed.

Then update `kicad/todo.md` items 4 (clock pins) and 5 (straps) with the results, and carry
any wiring change into the schematic before the PCB is started.

## 10. Test 4 - MEGALINK card 3023 with busy signals

For the MEGALINK (ND-107340, card 3023) up to about 983 kbit/s. Facts used here, and where they
come from:
- No straps on a MEGALINK: cable 325400 is a direct connection (Ronny, 2026-11-25).
- Busy pins and directions: cable 325400 OCR table in `HDLC-3015-vs-MEGALINK-3023.md`. TBY
  (19 +, 7 -) is an output of the ND, RBY (5 +, 17 -) is an input. Each machine's TBY goes to
  the other machine's RBY.
- Switch 13A on the 3023: XX XX OFF ON = busy enabled, XX XX ON OFF = busy disabled, and the
  26G switches choose the clock source (983 kbit/s, thumbwheel, 614.4 kbit/s). From
  `HDLC-3015-vs-MEGALINK-3023.md`; the 26G switch positions for each source are not written
  there.
- Which level means "busy", and what the ND does when it sees RBY: **not written in anything
  read so far**. This test finds out.

### 10.1 What changes from the HDLC test

- **Remove all four strap links** (3-5, 15-17, 11-24-23, 22-10). On the PCB: DIP switch SW1,
  positions 1 to 5, all OFF.
- Data and clock: exactly as section 3 (U1, U2), same pins.
- A third MAX3490E (U3) for busy, with its own 100 nF capacitor:

| Chip | Chip pin | Goes to | Real ND pin | Printed (flipped cable) |
|---|---|---|---|---|
| U3 (busy) | 3 DI | MCU busy out | - | - |
| U3 | 2 RO | MCU busy in | - | - |
| U3 | 5 Y | busy INTO the ND, plus (RBY+) | 5 | 9 |
| U3 | 6 Z | busy INTO the ND, minus (RBY-) | 17 | 22 |
| U3 | 8 A | busy OUT of the ND, plus (TBY+) | 19 | 20 |
| U3 | 7 B | busy OUT of the ND, minus (TBY-) | 7 | 7 |
| U3 | 1 VCC, 4 GND | 3V3, GND | - | - |

MCU pins for busy:
- PCB (schematic `HDLCGateway.kicad_sch`, WEMOS S3 mini since 2026-11-25): GPIO 8 -> U3 DI,
  U3 RO -> GPIO 7. With an S2 mini in the same holes: GPIO 10 -> U3 DI, U3 RO -> GPIO 8.
  On the PCB, DIP switch SW2 positions 1 to 4 (all ON) connect U3 to plug pins 5, 17, 19 and 7.
- Pico breadboard: no pins chosen. Whether RC422Dev has any busy support is **not checked**.
- Firmware that drives or reads the busy pins: **not written yet** on the ESP32-S2.

Not connected in this test: real 1, 9, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25
(printed 13, 5, 4, 3, 2, 1, 19, 18, 17, 16, 15, 14).

### 10.2 Checks before anything is powered

Everything in section 4, and also:
1. **Straps really out:** these must NOT beep: real 3-5 (printed 11-9), 15-17 (24-22),
   11-24 and 24-23 (3-15 and 15-16), 22-10 (17-4).
2. **No short inside a busy pair:** 5-17 (printed 9-22) and 19-7 (printed 20-7) must NOT beep.
3. **Each U3 pin to its plug pin**, every row of the table in 10.1.

### 10.3 Loop plug, no ND

1. Loop plug as in step 5.2 for data and clock, plus busy, keeping plus to plus:
   real 5 to 19 and 17 to 7 (through the flipped cable: printed 9 to 20 and 22 to 7).
2. Set the MCU's busy out high, then low. Busy in must follow. (Needs a console command or test
   build that sets and reads the busy pins; none exists yet.)
3. Traffic test as step 5.3 at 307200, then at 983000 with 4105-byte frames, with the
   margin rule from step 7.

### 10.4 Against the real 3023

1. ND: switch 13A = XX XX OFF ON (busy enabled). Set 26G for the clock you want. Write down
   exactly how 13A and 26G were set.
2. Watch the ND's TBY (real 19 against 7; printed 20 against 7), or the MCU's busy in, while the
   ND is idle and while it sends. Write down which level appears when.
3. Hold our busy out (RBY at the ND, real 5/17) at one level, then the other. Write down at
   which level the ND sends and at which it stops. That gives the meaning of the busy level.
4. Run traffic at 307200, then at 983000. Good looks like section 6.4, with bad CRC 0 at the top
   rate (step 7 rule).
5. **Same run with busy disabled** (13A = XX XX ON OFF), U3 still connected. Does the link work
   at 307200 without busy? At 983000? This answers whether busy is needed only for the high
   rate (Ronny's statement, not yet checked).

### 10.5 Results to fill in

| Check | Result | Date |
|---|---|---|
| 10.2 straps out, busy pairs not shorted, U3 pins to plug pins | | |
| 10.3 busy loop: out high -> in high, out low -> in low | | |
| 10.3 loop plug traffic at 307200 / 983000 | sent / received / bad | |
| 10.4.1 how 13A and 26G were set | | |
| 10.4.2 TBY level when ND idle / when ND sends | | |
| 10.4.3 RBY level at which the ND stops sending | | |
| 10.4.4 traffic with busy enabled, 307200 / 983000 | frames seen / good / bad | |
| 10.4.5 traffic with busy disabled, 307200 / 983000 | frames seen / good / bad | |
| 10.4 receive and transmit edge settings used | | |

## Related documents

- `HDLC-Cable-Measurements.md` - every beep measured so far, and the full printed/real table
  for all 25 pins.
- `HDLC-3015-vs-MEGALINK-3023.md` - what differs between the two ND cards.
- `HDLC-PCB-ESP32-Wiring.svg` (real numbers) and `HDLC-Breadboard-Wiring.svg` (printed numbers).
- `../kicad/README.md`, `../kicad/todo.md` - the board, and what is open on it.
- `Bench-Setups.md` - the other bench layouts, including the Pico and the ESP board line to line.
- RC422Dev repository: `CLAUDE.md` and `docs/REQUIREMENTS.md` for the Pico firmware.
