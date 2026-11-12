# HDLC-ESP32-S2 board: everything we know

Written 2026-11-10 so a new person (or a new LLM session) can carry on without the old chat.
Each line says whether it was **verified** (and how) or is an **assumption / guess**.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [What the board is](#what-the-board-is)
- [Files here](#files-here)
- [Wiring (real ND pin numbers, from the ND manual)](#wiring-real-nd-pin-numbers-from-the-nd-manual)
- [MAX3490E (verified from the datasheet, not from a web summary)](#max3490e-verified-from-the-datasheet-not-from-a-web-summary)
- [WEMOS S2 mini (verified from the official wemos.cc page, image, dimension PDF, schematic PDF)](#wemos-s2-mini-verified-from-the-official-wemoscc-page-image-dimension-pdf-schematic-pdf)
- [The cable that confused the measurements](#the-cable-that-confused-the-measurements)
- [ND card facts (from the ND Service Handbook, ND-107300 and ND-107340 sections)](#nd-card-facts-from-the-nd-service-handbook-nd-107300-and-nd-107340-sections)
- [DB-25 plug: what we know](#db-25-plug-what-we-know)
- [Tools](#tools)
- [Open items](#open-items)

<!-- /contents -->

## What the board is

- An interface between an ND-100 HDLC card (3015, ND-107300) or MEGALINK card (3023, ND-107340) and
  RS-422, driven by a WEMOS S2 mini (ESP32-S2).
- It started as a Raspberry Pi Pico on a MAX485 breadboard (Pico firmware repo RC422Dev). The PCB
  replaces that: WEMOS S2 mini + 2 x MAX3490E + a MALE DB-25 plug.
- Design rule from Ronny: the PCB plugs DIRECTLY into the female DB-25 socket on the back of the ND.
  No cable.
- Speed goal: at most 307.2 kbps. No MEGALINK 983 kbps mode, so no busy signals. (Ronny's decision;
  the 3015 manual recommends 76.8 kbps for a computer link.)

## Files here

| File | What |
|---|---|
| `HDLC-ESP32-S2/HDLC-ESP32-S2.kicad_sch` | schematic (KiCad 9) |
| `HDLC-ESP32-S2/HDLC-ESP32-S2.kicad_sym` | symbol library: the WEMOS S2 mini symbol |
| `HDLC-ESP32-S2/HDLC-ESP32-S2.pretty/WEMOS_S2_mini.kicad_mod` | S2 mini footprint |
| `HDLC-ESP32-S2/sym-lib-table`, `fp-lib-table` | project library tables |
| `HDLC-ESP32-S2/db25-footprint-check.pdf` | 8 stock DB-25 plug footprints printed 1:1 |
| `HDLC-ESP32-S2/wemos-s2-mini-footprint-check.pdf` | S2 mini footprint printed 1:1 |
| `../docs/HDLC-PCB-ESP32-Wiring.svg` | wiring diagram for the PCB, real ND pin numbers |
| `../docs/HDLC-Breadboard-Wiring.svg` | wiring diagram for the old breadboard, numbers as printed on Ronny's board |
| `../docs/HDLC-Cable-Measurements.md` | the measurement sheet and the measured DB-25 pinout |
| `../docs/HDLC-3015-vs-MEGALINK-3023.md` | what differs between the two ND cards |

There is no `.kicad_pcb` yet. The schematic passes ERC except two `lib_symbol_mismatch` warnings
(verified with `kicad-cli sch erc`). The netlist was exported and read: it matches the table below.

## Wiring (real ND pin numbers, from the ND manual)

ND signals: data out of the ND on 14 (+) and 2 (-); data into the ND on 16 (+) and 4 (-); clock out of
the ND on 3 (+) and 15 (-); clock into the ND on 18 (+) and 6 (-); ground on 8.
(Busy: RBY 5/17 in, TBY 19/7 out, MEGALINK only. Not used here.)

| Chip | Job | Pins on the plug | WEMOS S2 mini |
|---|---|---|---|
| U1 MAX3490E | DATA | A from 14, B from 2 (ND data in to the board); Y to 16, Z to 4 (board data to the ND) | DI from GPIO11, RO to GPIO9 |
| U2 MAX3490E | CLOCK | A from 15, B from 3 (assumption A4); Y to 18, Z to 6 | DI from GPIO7, RO to GPIO12 |

- Each chip has a 100 nF capacitor on its 3V3 pin. Chip ground (pin 4) goes to GND; plug pin 8 goes to GND.
- Solder jumpers for the ND card straps (ND numbers): 3-5, 15-17, 11-24-23, 22-10.
- Scope test points: TP1 = U1 DI (data out), TP2 = U1 RO (data in), TP3 = U2 DI (clock out),
  TP4 = U2 RO (clock in): the 3.3 V logic side of the chips. TP5 and TP6 = ground, TP7 = 3V3.
- Plug pins 1, 7, 9, 12, 13, 19, 20, 21, 25 are not connected (verified from the exported netlist).
- The WEMOS 5V pin is not used. No level shifter: the MAX3490E runs on 3.3 V.
- Firmware select (added 2026-11-12, for the two firmwares in one board): push button SW1 from GPIO5
  to GND, no resistor: the firmware turns on GPIO5's internal pull-up, so not pressed reads high and
  pressed reads low. GPIO5 is not a strapping pin (on the ESP32-S2 those are GPIO0, 45 and 46,
  datasheet section 3), so holding the button at power-on does not change how the chip boots.
- Two LEDs show which firmware runs, lit when the pin is high: D1 green on GPIO3 (HDLCGW),
  D2 red on GPIO1 (XMSGGW), each through 330 ohm (R1, R2) to GND. About 4 mA at 3.3 V for a
  2 V LED. A pure-green LED (about 3 V) would be dim at this value; use a classic yellow-green
  one or a smaller resistor. Nets verified from the exported netlist.
- In the WEMOS symbol, IO1, IO3 and IO5 were changed from "not connected" to bidirectional pins.

## MAX3490E (verified from the datasheet, not from a web summary)

- Full duplex, 3.0 to 3.6 V supply, no enable pins (driver and receiver always on), 10 Mbps.
- 8-pin pinout: 1 VCC, 2 RO, 3 DI, 4 GND, 5 Y (non-inverting driver out), 6 Z (inverting driver out),
  7 B, 8 A.
- Through-hole version: MAX3490EEPA (PDIP-8). Surface-mount: MAX3490EESA (SOIC-8).
- A web search summary once gave a wrong pinout and supply range. The datasheet is the authority.

## WEMOS S2 mini (verified from the official wemos.cc page, image, dimension PDF, schematic PDF)

- Pad grid 2.54 mm pitch, 8 rows, four columns. Columns at x = 0, 2.54, 20.32, 22.86 mm.
- Board about 25.4 x 34.3 mm. The footprint outline in KiCad is copied from the C3 mini and is not
  confirmed (todo item 2).
- Pins used: GPIO 7, 9, 11, 12 (the line), GPIO 1, 3, 5 (firmware LEDs and button), 3V3, GND.
  The WEMOS board's own LED is on GPIO15.

## The cable that confused the measurements

- Ronny's "DB 25 M to M, 100 cm" AliExpress cable has the flat cable flipped at ONE end.
  Not announced by the seller. Found by Ronny.
- Measured with jump beeps (not tested pin by pin): printed number to real ND number is
  top row real = 14 - printed (printed 1 to 13), bottom row real = 39 - printed (printed 14 to 25).
- The breadboard diagram uses printed numbers; the PCB diagram uses real ND numbers.
- Ground: printed 6 = real 8 follows from the flip and is NOT tested.
- The ND-side connector is FEMALE (Ronny's statement, not checked by me).

## ND card facts (from the ND Service Handbook, ND-107300 and ND-107340 sections)

- Data and clock pins are the same on both cards. Only the busy pins (5, 17, 19, 7) differ.
- 3015 cable plug straps are 3-5, 15-17, 11-24-23, 22-10. Their purpose is NOT documented in the pages
  read. GUESS (marked as a guess everywhere): 3-5 and 15-17 feed the ND's own clock into its
  "indication" input.
- Details: `../docs/HDLC-3015-vs-MEGALINK-3023.md`.

## DB-25 plug: what we know

- The plug must match a real connector 100 %: Ronny was burned before by connectors whose pins or
  side screw holes did not fit the PCB.
- Stock KiCad footprints and 3D models are installed with KiCad 9 (verified on disk): footprints
  `Connector_Dsub`, 3D models `Connector_Dsub.3dshapes`. Male = `DSUB-25_Pins_*`, female =
  `DSUB-25_Socket_*`. Right-angle male variants differ in the distance from the board edge to the
  pins and to the screw holes: 9.40 mm; 4.94 mm / hole 4.94; 7.70 / 9.12; 9.90 / 11.32;
  14.56 / 8.20; 14.56 / 15.98 (edge-pin offset / mounting-hole offset).
- Maker parts seen in a search (not downloaded, not checked): Amphenol Delta D (D25P13A4PL00LF,
  DB25P564GTXLF), Adam Tech / NorComp 173-E25-113R001. Some SnapMagic footprints are uploaded by
  users and need checking against the maker drawing.
- Ronny's choice: start with the 3 closest stock variants, compare with the real part.

## Tools

- `kicad-cli` is in the `bin` folder of the KiCad 9 install (it was not on PATH in the Claude shell).
  `kicad-cli sch erc` and `kicad-cli sch export netlist` were used to check the schematic.
- The schematic was generated by a throw-away script and then deleted. Further changes are edits to
  the `.kicad_sch` file (KiCad editor or text edit). Net labels sit exactly on pin ends; positions
  are on the 1.27 mm grid.

## Open items

See `todo.md`.
