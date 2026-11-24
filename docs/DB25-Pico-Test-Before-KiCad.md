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
| Busy lines (MegaLink 3023 only, NOT used) | 5/17 in, 19/7 out | - |

Signal names and signs come from the ND Service Handbook (3023 MegaLink table, section 2-22,
page 5 of 5). The 3015 HDLC card uses the same data and clock pins (section 2-20, page 7 of 8),
but its table gives no plus or minus signs; that the signs are the same there is **not verified**.

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
pins 7 and 8 is the first thing to try. A swapped pair does no damage.

**The straps** (3-5, 15-17, 11-24-23, 22-10) are the 3015 card's computer-link straps in the
cable plug. What they do is not written in the pages read so far (`kicad/todo.md` item 5).
Leave them as wire links you can pull out, and run step 6 with and without them.

Nothing else on the plug is connected: real pins 1, 7, 9, 12, 13, 19, 20, 21, 25 (board netlist).

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

## 7. Test 3 - measure the clock edge (settles assumption A4)

With the ND sending: logic analyser on real pin 14 (data plus) and real pin 3 (clock plus),
ground on 8. Look at which clock edge the data changes on. The data must be steady at the edge
the Pico reads. Write down: "data changes on the rising / falling edge of pin 3".
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

## Related documents

- `HDLC-Cable-Measurements.md` - every beep measured so far, and the full printed/real table
  for all 25 pins.
- `HDLC-3015-vs-MEGALINK-3023.md` - what differs between the two ND cards.
- `HDLC-PCB-ESP32-Wiring.svg` (real numbers) and `HDLC-Breadboard-Wiring.svg` (printed numbers).
- `../kicad/README.md`, `../kicad/todo.md` - the board, and what is open on it.
- `Bench-Setups.md` - the other bench layouts, including the Pico and the ESP board line to line.
- RC422Dev repository: `CLAUDE.md` and `docs/REQUIREMENTS.md` for the Pico firmware.
