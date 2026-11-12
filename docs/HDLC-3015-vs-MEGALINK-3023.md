# HDLC card (3015) versus MEGALINK card (3023)

Sources, all read from the page images:
- ND Service Handbook Vol II (NDSH02-03), section 2-20 pages 1 to 8: ND-107300 HDLC interface, the
  3015 card.
- Same handbook, section 2-22 pages 1 to 5: ND-107340 MEGALINK interface, the 3023 card.
- Your notes (ND HDLC.pptx), slides 3, 17 and 24, where they are named.

Anything not on those pages is marked "not verified".

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [Scope decision (Ronny, 2026-11-06)](#scope-decision-ronny-2026-11-06)
- [Short answer](#short-answer)
- [Side by side](#side-by-side)
- [Pins whose job changes between the cards](#pins-whose-job-changes-between-the-cards)
- [What this means for the Pico board](#what-this-means-for-the-pico-board)
- [If the board supports the busy signal (MEGALINK)](#if-the-board-supports-the-busy-signal-megalink)
- [Not verified](#not-verified)

<!-- /contents -->

## Scope decision (Ronny, 2026-11-06)

Aim for at most 307.2 kbps and leave out the MEGALINK's special 983 kbps mode, for simplicity. The
manual's thumbwheel table (above) marks 76.8 kbps (setting 9) as the recommended computer-link speed
on the 3015 and 307.2 kbps (setting 0) as the top. So: four MAX485 modules, no busy modules, no busy
wiring. The busy section below stays as reference only.

Not checked: that the MAX485 modules and this firmware's timing run cleanly at 307.2 kbps. The
firmware sets its transmit speed from a baud value in `src/sync.cpp`; that has not been tested at
that rate.

## Short answer

Your assumption is half right.
- **Right:** the data and clock pins are the same on both cards. The only extra signal pair on the
  MEGALINK is the busy output (TBY, pins 19 and 7), and its busy input (RBY, pins 5 and 17) is a
  real signal where the HDLC card has these pins strapped to its own clock output.
- **Not the whole story:** the cards also differ in speed, line types, what the switches mean,
  clock selection, which machines they can talk to, the cable, and the straps.

## Side by side

| Item | HDLC, 3015 (ND-107300) | MEGALINK, 3023 (ND-107340) |
|---|---|---|
| Card part number | 322615 | 322623 |
| Purpose | Single-line serial interface to a modem, a data network or a computer link | Special computer-to-computer link between ND-110 and ND-110, or ND-110 and N-10 |
| Line types | V.24/V.28 modem, X.21/X.27, computer link, V.36/V.11 (64 kbit/s), V.35 on request | Computer link only, balanced RS-422 drivers and receivers |
| Top speed | 307 kbps in computer link | 983 kbps |
| Who it can talk to | not stated on the pages read | "A MEGALINK interface can only communicate with another MEGALINK interface" |
| Frame format | HDLC, handled in hardware, DMA | HDLC, handled in hardware, DMA |
| Signals on the 25-pin plug | TB/TA (14, 2), CB/CA (15, 3), RB/RA (16, 4), IA/IB (17, 5), SB/SA (18, 6), GND (8) | TXD (14, 2), TXC (15, 3), RXD (16, 4), RBY (17, 5), RXC (18, 6), TBY (19, 7), GND (8) |
| Busy signals | none. The I pair (17, 5) is "indication" | RBY input on 5 and 17, TBY output on 19 and 7 |
| Cable between two machines | 325402 | 325400 (325440 to an N-10) |
| What the cable crosses | 14 to 16, 2 to 4, 15 to 18, 3 to 6, 8 to 8. Pins 5 and 17 are not in the cable | the same, plus 19 to 5 and 7 to 17 |
| Straps in each plug | 3-5, 15-17, 11-24-23, 22-10 | none listed |
| Switch 13A | line type (computer link is 4-3-2-1 = XX OFF OFF ON), autoload connection | busy on or off: XX XX OFF ON = busy enabled, XX XX ON OFF = busy disabled |
| Switch 26G | 4 switches: oscillators on or off | 6 switches: clock source (983 kbps, or baud from thumbwheel 1, or 614.4 kbps), processor clock, oscillator test |
| Baud thumbwheel (TH1) | 0 = 307.2, 3 = 1.2, 6 = 9.6, 7 = 38.4, 8 = 153.6, 9 = 76.8 (recommended in computer link), 11 = 19.2, 13 = 4.8, 14 = 2.4 kbps | the same table, used only if 26G selects "baud via TH1" |
| Device numbers (TH3) | HDLC 1 to 6: 1640-1657 ident 150, 1660-1677 ident 151, 1700-1717 ident 152, 1720-1737 ident 153, 1740-1757 ident 154, 1760-1777 ident 155 | MEGALINK 1 to 6: the same numbers and idents |
| Indicator | not on the pages read | one yellow LED: not lit means the transmit clock is running |
| Variants | ND-107360 (4800 bps) and ND-107370 (9600 bps), fixed speed | none listed |

## Pins whose job changes between the cards

| Pin | HDLC 3015, computer link | MEGALINK 3023 |
|---|---|---|
| 14, 2 | data out of the ND | data out of the ND |
| 16, 4 | data into the ND | data into the ND |
| 15, 3 | out of the ND (named C; clock in computer link is our reading) | transmit clock out of the ND |
| 18, 6 | into the ND (named S, the clock) | receive clock into the ND |
| 5, 17 | into the ND, strapped to 3 and 15 at the far plug | busy input (RBY), driven by the other machine's TBY |
| 19, 7 | not in the signal table. In the X.21 column they are strap points (18-19, 6-7) | busy output (TBY) of the ND |
| 10, 11, 22, 23, 24 | strap points only (22-10, 11-24-23) | not listed at all |

Only the pins 5, 17, 19, 7 (and the straps on 10, 11, 22, 23, 24) change role.

## What this means for the Pico board

- The same four modules (data in, data out, clock in, clock out) serve both cards.
- For the HDLC card the board needs the straps 3-5, 15-17, 11-24-23, 22-10.
- For the MEGALINK card the straps are not listed. Pins 5 and 17 must not be strapped to 3 and 15
  there: they would then be fed by the ND's own clock instead of a busy signal.
- Your slide 3 shows busy disabled on the 3023 (switch 13A). With busy disabled the MEGALINK should
  not need the RBY signal. That is what the manual's switch table says; whether it behaves so on
  your machine is not verified.
- To handle both cards from the Pico, the only pins that change are 5 and 17. They need to be
  strapped to 3 and 15 for the HDLC card and driven by a Pico busy signal (or left open with busy
  disabled) for the MEGALINK card.
- The manual says a MEGALINK only talks to another MEGALINK. Emulating the far end of a MEGALINK
  link is therefore a different job from emulating the far end of an HDLC link, even if the data
  and clock wiring is the same.

## If the board supports the busy signal (MEGALINK)

Per Ronny (2026-11-06, not checked against the manual pages): busy is only needed if the MEGALINK
is to run at the special 983 kbps. At the normal maximum of 307.2 kbps it is not needed, so the four
modules are enough and M5/M6 are optional.

Pin numbers below are REAL ND pins. For the numbers printed on your boards see the table "Measured
pinout of your female DB-25 connector" in HDLC-Cable-Measurements.md (assumption A1, mirror, not proven).

Evidence: the MEGALINK cable table (section 2-22 page 5 of 5) has TBY+ on 19 and TBY- on 7 (ND
output), RBY+ on 5 and RBY- on 17 (ND input). The board is the other end, so:

| Direction | Pins | Module | What it does |
|---|---|---|---|
| ND to board (the ND's busy) | 19 (plus), 7 (minus) | M5, listening (DE + /RE to GND) | A from 19, B from 7. RO goes through a level shifter channel to a Pico GPIO |
| Board to ND (our busy) | 5 (plus), 17 (minus) | M6, sending | DI comes from a Pico GPIO through a level shifter channel. A to pin 5, B to pin 17 |

What changes:
- **Six modules, not four.** The level shifter has eight channels and six are used, so channels 7
  and 8 are free for the two busy signals. Two more Pico GPIO pins are needed (any free ones; not
  chosen yet).
- **No straps on 5 and 17.** The HDLC card's straps 3-5 and 15-17 tie the ND's own clock output to
  pins 5 and 17. With M6 driving those pins, a strap would put two outputs against each other.
  The straps and M6 must never both be connected.
- **Switching between the cards.** M6's DE pin decides whether it drives the line (on a MAX485 a
  low DE should turn the driver off; that is general knowledge, check the datasheet). So M6 can
  be switched off from the Pico. The straps cannot be switched off by a module; they need
  a jumper, a relay or an analog switch.
- **Cable.** With a straight cable nothing else changes; pins 19, 7, 5, 17 simply become signal
  wires. With the manual's cable they cross 19 to 5 and 7 to 17, as for the clock.
- **Switch 13A on the ND (MEGALINK).** XX XX OFF ON = busy enabled, XX XX ON OFF = busy disabled.
  With busy disabled none of this is needed (per the switch table).
- **Firmware.** Something has to read TBY and drive RBY. Not checked whether this firmware has
  any hook for it.

Not stated on the pages read: which level means busy, and what the ND does when it sees RBY.

## Not verified

- That the HDLC frame timing and the frame content are identical on both cards. The pages only
  say HDLC in hardware.
- What the straps on pins 10, 11, 22, 23 and 24 do, and whether they harm a MEGALINK.
- Whether the firmware in this repository handles busy signals at all (not checked).
- The 3015 switch table has duplicated rows in the scan for V.35, V.24, X.21 bis and X.25. Only
  the computer-link row (13A 4-3-2-1 = XX OFF OFF ON) is quoted here.
