# 25-pin connector to breadboard: how is it wired?

Power everything off first (USB unplugged, no supply on the breadboard), meter in beep mode.
Write what you see or what the meter says. `not measured` and `I do not know` are fine.

**Which connector has the red wires (male or female), and how many red wires are there:**

______________________________

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [What to expect (a guess from the ND manual and the KiCad designs, NOT a fact about your wiring)](#what-to-expect-a-guess-from-the-nd-manual-and-the-kicad-designs-not-a-fact-about-your-wiring)
- [Result so far: the beeps you measured](#result-so-far-the-beeps-you-measured)
- [Result so far: the AM26LS31 driver chip](#result-so-far-the-am26ls31-driver-chip)
- [Result so far: the AM26LV32 receiver chip](#result-so-far-the-am26lv32-receiver-chip)
- [What is verified and what is assumed](#what-is-verified-and-what-is-assumed)
- [Conclusion: the wiring to rebuild (from the code and your measurements)](#conclusion-the-wiring-to-rebuild-from-the-code-and-your-measurements)
- [Check these against your ND documentation (the PDF may be wrong)](#check-these-against-your-nd-documentation-the-pdf-may-be-wrong)
- [Measured pinout of your female DB-25 connector](#measured-pinout-of-your-female-db-25-connector)
- [RS-485 modules](#rs-485-modules)
- [Your notes](#your-notes)

<!-- /contents -->

## What to expect (a guess from the ND manual and the KiCad designs, NOT a fact about your wiring)

Pin numbers in this section are REAL ND pin numbers, not the numbers printed on your boards. The table
"Measured pinout of your female DB-25 connector" below gives both.

- **Pin 8 is ground.** It should beep with the Pico ground and every module's ground.
- **Pairs.** Each pair below goes to the two terminals (A and B) of one RS-485 module:
  14 and 2, 16 and 4, 3 and 15, 18 and 6, 19 and 7, 5 and 17.
  The two pins of a pair should NOT beep with each other. If they do, that is a short.
- **Strap** = pins the ND card's manual ties together inside the plug: 3 with 5, 15 with 17,
  11 with 24 with 23, and 22 with 10.
- **Crossover** = pairs swapped as in the ND-to-ND cable: 14 with 16, 2 with 4, 3 with 18,
  15 with 6. If you see these beeps, the swap was done on your wiring.
- Pins 1, 9, 12, 13, 20, 21 and 25 are unused: no wire, no beep.

## Result so far: the beeps you measured

Every pair that beeps (reported by you, nothing else beeps):

| Beep | Pins |
|---|---|
| 1 | 3 with 15, and 3 with 16 |
| 2 | 4 with 17 |
| 3 | 9 with 11 |
| 4 | 22 with 24 |

These match none of the expected patterns using the pin numbers as you counted them.

**But they match the manual's four straps exactly if the pin numbers are mirrored.** Reading
the numbers from the other side of a DB-25 flips them: top row pin n becomes 14 minus n,
bottom row pin n becomes 39 minus n.

| You measured | Mirrored | The manual's strap |
|---|---|---|
| 3, 15, 16 | 11, 24, 23 | 11 with 24 with 23 |
| 4, 17 | 10, 22 | 22 with 10 |
| 9, 11 | 5, 3 | 3 with 5 |
| 22, 24 | 17, 15 | 15 with 17 |

All four groups line up and nothing else beeps. That is the 3015 HDLC card's computer-link
strap set. If the mirror is right, the pin numbers in your table below are also mirrored.

The printed numbers on your two connectors match each other. That does not rule the mirror out:
if both boards carry the same wrong labels, they still match each other. Two checks settle it.

**Check 1: the ground wire.** Under the mirror, the real ground pin 8 is printed as 6 on your
boards (and printed 8 is really 6). Follow the red wire from printed pin 6 and from printed
pin 8 on the female connector to the breadboard. Which one goes to the Pico ground?

- Printed pin 6 goes to: ______________________________
- Printed pin 8 goes to: ______________________________

If printed 8 goes to the Pico ground, the mirror is wrong. If printed 6 does, it is right.

**Check 2: where the printed 1 sits.** Look at the metal D-shaped connector, not the board.
Hold it with the wide side up, looking at the face where the other connector plugs in.
The standard for the real pin 1 is: **female** (holes) top LEFT, **male** (pins) top RIGHT.
Which end of the top row (the row with 13 holes or pins) has the printed "1"?

- Female connector, printed 1 is at the: left end / right end
- Male connector with the breakout, printed 1 is at the: left end / right end

## Result so far: the AM26LS31 driver chip

Measured by you (beeps, chip pin to printed connector pin). The first report of pins 3 and 5 was
the wrong way round; the beep test fixed it: chip pin 3 beeps with printed 10 only, and chip
pin 5 beeps with printed 8.

| AM26LS31 pin | Output | Printed connector pin | Real pin if mirrored | What that real pin is (MEGALINK table) |
|---|---|---|---|---|
| 2 | 1Y | 23 | 16 | RXD+ (data to the ND, plus) |
| 3 | 1Z | 10 | 4 | RXD- (data to the ND, minus) |
| 6 | 2Y | 21 | 18 | RXC+ (clock to the ND, plus) |
| 5 | 2Z | 8 | 6 | RXC- (clock to the ND, minus) |

So driver 1 sends the data pair and driver 2 sends the clock pair, with each Y output on the plus
wire and each Z output on the minus wire. That matches the MEGALINK table and the older PDF
exactly, once the pin numbers are mirrored.

Also: which Pico pin (or level shifter channel) feeds chip pin 1 (1A) and chip pin 7 (2A)?

- Chip pin 1 (1A, data) is fed from: level shifter B3, from A3, from Pico pin 19 (GPIO14) (reported)
- Chip pin 7 (2A, clock) is fed from: level shifter B4, from A4, from Pico pin 20 (GPIO15) (reported)

This matches the firmware in this repository: GP14 is TX data and GP15 is TX clock. So the
breadboard ran this firmware's transmit side.

**Firmware finding (read from `src/sdlc.pio`).** The receive program takes the data pin and the
very next pin as the clock (GP16 data, GP17 clock) and reads a data bit on the RISING edge of
the clock pin. It looks for the flag as 0, six 1s, 0, so it expects the data NOT inverted. Your
clock receiver gives the Pico the ND's clock inverted, so the Pico's rising edge is the falling
edge of the ND's TXC+. If receiving worked with this wiring, the ND's data is steady at the
falling edge of TXC+. (Whether a real ND reads on that edge is NOT verified; see the list below.) If we ever wire
the receive clock non-inverted (pin 3 on 2A), the receive program must be changed to read on
the falling edge.

## Result so far: the AM26LV32 receiver chip

You reported: AM26LV32 pin 1 (1B) goes to printed connector pin 12, and pin 2 (1A) goes to
printed connector pin 25.

Under the mirror, printed 12 is real pin 2 (TXD-, the ND's data out, inverting side) and printed
25 is real pin 14 (TXD+, the ND's data out, non-inverting side). That is the ND's transmit data
pair, wired non-inverting to 1A and inverting to 1B, as it should be for a receiver. It also
matches the older PDF's receiver wiring (1A to pin 14, 1B to pin 2).

The mirror predicts the rest of the receiver like this. Fill in what is really there:

| AM26LV32 pin | Input | Listens to (real pin, from the PDF) | Mirror predicts printed pin | You measured |
|---|---|---|---|---|
| 1 | 1B | 2 (ND data out, inverting) | 12 | 12 (reported) |
| 2 | 1A | 14 (ND data out, non-inverting) | 25 | 25 (reported) |
| 6 | 2A | 3 (ND clock out, non-inverting) | 11 (or 9, same strap) | 24 and 22 (reported). These two beep together, so it is one net. That is the OTHER clock wire (real 15 and 17), not the predicted one |
| 7 | 2B | 15 (ND clock out, inverting) | 24 (or 22, same strap) | 9 and 11 (reported). These beep together, so it is one net: real 5 and 3. Opposite of the prediction |
| 10 | 3A | 19 (ND busy out, non-inverting) | 20 | nothing connected (reported) |
| 9 | 3B | 7 (ND busy out, inverting) | 7 | nothing connected (reported) |

**Finding (clock polarity).** Your clock receiver has 2A on the real 15 / 17 net and 2B on the
real 3 / 5 net. The older PDF has it the other way round (2A on 3, 2B on 15). In your wiring the
receiver's non-inverting input is on the ND's "B" wire for BOTH data (pin 14) and clock (pin 15),
and the driver's Y outputs (the non-inverting ones) are on the "B" wires 16 and 18 as well. So
your breadboard uses one rule for all four signals: the B wire is the positive one. The PDF's
clock rows (pin 3 as clock plus, pin 15 as clock minus) break that rule. If this is right, the
PDF has the clock polarity wrong, or you reversed it on the breadboard to make it work.

Which Pico pin (or level shifter channel) does each receiver output go to?

- AM26LV32 pin 3 (1Y, data) goes to: ______________________________
- AM26LV32 pin 5 (2Y, clock) goes to: ______________________________

## What is verified and what is assumed

**Verified (read or measured, with the source):**
- The ND tables: pin names, pin numbers, cable pairs and straps (section 2-20 page 7 of 8, 3015
  card), and the signs and cable pairs (section 2-22 page 5 of 5, MEGALINK card). Read from the page images.
- The wiring of the two KiCad designs (exported with `kicad-cli`).
- The firmware: pins, clock edge and data polarity (read in `src/sync.cpp` and `src/sdlc.pio`).
- The AM26LS31 pin names (datasheet in the KiCad folder).
- The beeps and chip-pin wiring that you reported. Reported by you, not measured by Claude.

**Assumed (not verified):**
1. That your boards' pin numbers are mirrored. Every report fits it, but it is not proven. Still
   open: which printed pin has the ground wire (6 or 8), and where printed 1 sits.
2. That a real ND treats its plus pins as non-inverting inputs and reads data on the rising edge
   of its receive clock plus, and so that a real ND's data is steady at the falling edge of TXC+.
3. That receiving ever worked with this wiring and firmware (the README says it did on the
   bench; your TODO says it fell behind the line speed).
4. That the AM26LV32 outputs went to GPIO16 and GPIO17 (the cables there go nowhere; this comes
   from the code only).
5. That the HDLC (3015) card uses the same plus and minus as the MEGALINK table (the 3015 table
   gives no signs).
6. The chips' power, ground and enable pins.

To stop assuming 2: with the ND connected and sending, put a logic analyzer on ND pins 14 and 3
(data plus and clock plus) and look at which clock edge the data changes on.

## Conclusion: the wiring to rebuild (from the code and your measurements)

**This section is only as good as the assumptions listed above. The real pin numbers depend on
assumption 1 (mirror) and the receive outputs on assumption 4. Do not build from it until
those two are checked.**

The receive cables on GPIO16 and GPIO17 go nowhere, so the receive side is concluded from the
code, which uses RX_PIN 16 (data) and RX_CLOCK_PIN 17 (clock), TX_PIN 14 and TX_CLOCK_PIN 15
(`src/sync.cpp`). The receive program needs the clock on the pin right after the data pin.

Pin numbers below are the REAL ND pin numbers (your boards' printed numbers are mirrored: real 16
is printed 23, real 4 is printed 10, real 18 is printed 21, real 6 is printed 8, real 14 is
printed 25, real 2 is printed 12). The mirror is my reading, supported by every measurement
so far, but not proven.

**Pico to ND (AM26LS31 driver, measured):**

| Pico pin | Through | Chip pin | Chip output | ND pin | Meaning |
|---|---|---|---|---|---|
| GPIO14 (pin 19) | level shifter A3 to B3 | 1 (1A) | 1Y (pin 2) | 16 | data plus |
| GPIO14 (pin 19) | level shifter A3 to B3 | 1 (1A) | 1Z (pin 3) | 4 | data minus |
| GPIO15 (pin 20) | level shifter A4 to B4 | 7 (2A) | 2Y (pin 6) | 18 | clock plus |
| GPIO15 (pin 20) | level shifter A4 to B4 | 7 (2A) | 2Z (pin 5) | 6 | clock minus |

**ND to Pico (AM26LV32 receiver; inputs measured, outputs concluded from the code):**

| ND pin | Meaning | Chip input | Chip output | Pico pin |
|---|---|---|---|---|
| 14 | data plus | 1A (pin 2) | 1Y (pin 3) | GPIO16 (pin 21) |
| 2 | data minus | 1B (pin 1) | 1Y (pin 3) | GPIO16 (pin 21) |
| 15 | clock minus | 2A (pin 6) | 2Y (pin 5) | GPIO17 (pin 22) |
| 3 | clock plus | 2B (pin 7) | 2Y (pin 5) | GPIO17 (pin 22) |

The clock inputs follow the manual's cable, which swaps the clock plus and minus between two
machines (pin 3 to 6, pin 15 to 18): the board listens like a second ND would. With this firmware
(reads on the rising edge) the Pico reads on the falling edge of the ND's clock plus.

**Straps on the connector (measured, real pin numbers):** 3 with 5, 15 with 17, 11 with 24 with
23, and 22 with 10. Ground: real pin 8. Nothing on the busy pins (19, 7).

**Not measured, check before powering up:**
- Ground and power of both chips, and the chips' enable pins (AM26LS31 pins 4 and 12).
- Unused inputs of the unused channels.
- The AM26LV32 is a 3.3 V part by its name. The older ESP32 KiCad sheet powers it from 5 V. Look
  at its datasheet before powering it, and never let a 5 V output reach a Pico pin.

## Check these against your ND documentation (the PDF may be wrong)

Where the PDF and your working wiring disagree, the ND documents decide. Please look each one
up and write what the ND document says. Where to look: the ND HDLC board's circuit diagram
(the line driver chip 19C, a 9638, and the receiver chip 17A, a 26LS32, from your slides 9 to 13),
section 2-20 page 7 of 8 of the service handbook, and the HDLC interface description ND-12.018.01
(your slide 6, "HDLC Point-to-Point without modem", page A-22).

Use the real pin numbers (the ones in the ND documents), not the printed numbers on your boards.

| # | Question | PDF says | Your working wiring says | What the ND document says |
|---|---|---|---|---|
| 1 | Transmit clock pair from the ND (pins 3 and 15): which pin is the positive wire? | pin 3 | pin 15 | MEGALINK table (section 2-22 page 5 of 5): TXC+ = pin 3, TXC- = pin 15. So pin 3 is positive (3023 card; the 3015 table gives no signs) |
| 2 | Receive clock pair to the ND (pins 18 and 6): which pin is the positive wire? | pin 18 | pin 18 | MEGALINK table: RXC+ = pin 18, RXC- = pin 6. Pin 18 is positive |
| 3 | Transmit data pair from the ND (pins 14 and 2): which pin is the positive wire? | pin 14 | pin 14 | MEGALINK table: TXD+ = pin 14, TXD- = pin 2. Pin 14 is positive |
| 4 | Receive data pair to the ND (pins 16 and 4): which pin is the positive wire? | pin 16 | pin 16 | MEGALINK table: RXD+ = pin 16, RXD- = pin 4. Pin 16 is positive |
| 5 | In the ND circuit diagram, which driver output (Y or Z) feeds plug pin 15, and which feeds pin 3? | not covered | not known | not answered yet |
| 6 | Are the pin numbers in the ND documents seen from the front of the plug, or from the solder side? | not covered | numbers on your boards look mirrored | Table does not say |

What the ND table does show (HDL-L, 25-pin plug, ND-107300, section 2-20 page 7 of 8): the
B-letter pins are 14 (TB), 15 (CB), 16 (RB) and 18 (SB), and the A-letter pins are 2 (TA),
3 (CA), 4 (RA) and 6 (SA). The letters follow the same pattern for data and clock. The
interface pair is the other way round: IA = 17, IB = 5. If the ND uses the letters the same
way on every signal, then the clock pair follows the data pairs, with B on 15 as on 14, 16
and 18. That would favour your wiring over the PDF's pin 3 for the send clock. This was an
inference from the letters, and the MEGALINK table below it shows it was WRONG.

**Correction.** The MEGALINK table states the signs: the plus wires are 14 (TXD+), 16 (RXD+),
3 (TXC+) and 18 (RXC+). So the send clock's plus wire is pin 3, which is what the PDF says. The
clock plus wire is on the A-letter side, unlike the data. Your breadboard's clock receiver has
2A on pin 15 (the minus wire). That is exactly what the manual's computer-link cable does (see
below), so your receiver listens the way a second ND would.

The same table also shows that in the ND-to-ND computer-link cable the clock goes plus to
minus (pin 3 to pin 6, pin 15 to pin 18), while data and busy go plus to plus. So the cable
itself swaps the clock: a second ND listens to our pin 15 on its pin 18 (RXC+). Your receiver,
with 2A on pin 15, listens the same way. The PDF's receiver (2A on pin 3) leaves out the swap.

**Working decision (so we do not get stuck on this):** use the signs from the MEGALINK table:
plus = 14, 16, 3, 18 for what the ND sends and receives, and listen to the ND's clock the way
a second ND would through the cable (pins swapped), as your breadboard receiver is wired. If the
ND shows no good frames, swap the two wires of ONE pair at a time
on the screw terminals and try again. Nothing is damaged by a swapped pair. The clock sign can
also be absorbed in the Pico firmware by sampling on the other edge.
| 7 | Computer-link straps at the far end: 3-5, 15-17, 11-24-23, 22-10. Does ND-12.018.01 page A-22 show the same? | same | your connector has the same four | |
| 8 | What does page A-22 show for the cable pins between two machines with no modem? | not covered | not known | |

## Measured pinout of your female DB-25 connector

**This is the one table the other documents refer to.** The first column is the number printed on
your boards, as you counted it. The second column is the real ND pin, assuming your numbers are
mirrored (assumption A1: real = 14 minus printed for printed 1 to 13, 39 minus printed for printed
14 to 25; not proven). The ND meaning comes from the manual (section 2-20 page 7 of 8 and section
2-22 page 5 of 5). The beeps are the ones you reported; every other pair did not beep. The chip
wiring is from your reports on the old breadboard (AM26LS31 driver, AM26LV32 receiver).

| Printed pin | Real ND pin | ND meaning (manual) | Beeps with (printed) | Old breadboard: where it ends | New design |
|---|---|---|---|---|---|
| 1 | 13 | unused | none | not measured | not used |
| 2 | 12 | unused | none | not measured | not used |
| 3 | 11 | strap point (strap 11-24-23, HDLC card) | 15, 16 | no chip | strap 11-24-23 |
| 4 | 10 | strap point (strap 22-10, HDLC card) | 17 | no chip | strap 22-10 |
| 5 | 9 | unused | none | not measured | not used |
| 6 | 8 | GND | none | ground wire not checked | common GND |
| 7 | 7 | TBY minus (MEGALINK busy out); strap point on the X.21 setting | none | nothing (AM26LV32 pin 9 not connected) | busy, optional (M5 B) |
| 8 | 6 | RXC minus (HDLC card: SA), clock into the ND | none | AM26LS31 pin 5 (2Z) | M2 B |
| 9 | 5 | RBY plus (HDLC card: IB), busy into the ND | 11 | AM26LV32 pin 7 (2B), same net as 11 | strap 3-5 (HDLC card) |
| 10 | 4 | RXD minus (HDLC card: RA), data into the ND | none | AM26LS31 pin 3 (1Z) | M1 B |
| 11 | 3 | TXC plus (HDLC card: CA), clock out of the ND | 9 | AM26LV32 pin 7 (2B), same net as 9 | M4 B and strap 3-5 |
| 12 | 2 | TXD minus (HDLC card: TA), data out of the ND | none | AM26LV32 pin 1 (1B) | M3 B |
| 13 | 1 | unused | none | not measured | not used |
| 14 | 25 | unused | none | not measured | not used |
| 15 | 24 | strap point (strap 11-24-23, HDLC card) | 3, 16 | no chip | strap 11-24-23 |
| 16 | 23 | strap point (strap 11-24-23, HDLC card) | 3, 15 | no chip | strap 11-24-23 |
| 17 | 22 | strap point (strap 22-10, HDLC card) | 4 | no chip | strap 22-10 |
| 18 | 21 | unused | none | not measured | not used |
| 19 | 20 | unused | none | not measured | not used |
| 20 | 19 | TBY plus (MEGALINK busy out) | none | nothing (AM26LV32 pin 10 not connected) | busy, optional (M5 A) |
| 21 | 18 | RXC plus (HDLC card: SB), clock into the ND | none | AM26LS31 pin 6 (2Y) | M2 A |
| 22 | 17 | RBY minus (HDLC card: IA), busy into the ND | 24 | AM26LV32 pin 6 (2A), same net as 24 | strap 15-17 (HDLC card) |
| 23 | 16 | RXD plus (HDLC card: RB), data into the ND | none | AM26LS31 pin 2 (1Y) | M1 A |
| 24 | 15 | TXC minus (HDLC card: CB), clock out of the ND | 22 | AM26LV32 pin 6 (2A), same net as 22 | M4 A and strap 15-17 |
| 25 | 14 | TXD plus (HDLC card: TB), data out of the ND | none | AM26LV32 pin 2 (1A) | M3 A |

Straps seen in your beeps, in both numberings:

| Printed pins | Real pins | Manual's strap (HDLC card, computer link) |
|---|---|---|
| 3, 15, 16 | 11, 24, 23 | 11-24-23 |
| 4, 17 | 10, 22 | 22-10 |
| 9, 11 | 5, 3 | 3-5 |
| 22, 24 | 17, 15 | 15-17 |

Still open: where the ground wire goes (printed 6 if the mirror is right, printed 8 if not), and
where printed 1 sits on each connector. Both checks are in the section "Result so far: the beeps"
above.

## RS-485 modules

Copy the names printed on the module next to its pins.

| Module | Connector pin on A | Connector pin on B | DI | RO | DE | RE |
|---|---|---|---|---|---|---|
| M1 | | | | | | |
| M2 | | | | | | |
| M3 | | | | | | |
| M4 | | | | | | |

## Your notes

Which pins did you short together, and which pins went to RS-485 modules and which did not:
