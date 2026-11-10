# KiCad board: what is still open

**Next:** pick the DB-25 plug footprint. Hold the real connector against
`HDLC-ESP32-S2/db25-footprint-check.pdf` (8 stock variants, printed 1:1) and say which page fits, or
say none fits. Then the plug (J1) gets its footprint and 3D model.

Everything we know so far is in `NOTES.md` in this folder. Only open work is listed here.

## Must be decided or checked before a PCB is laid out

1. **DB-25 plug footprint (J1).** Empty on purpose. Two ways:
   - match the real connector to one of the 8 stock KiCad variants (`DSUB-25_Pins_*`), including the
     side screw holes and the distance from the board edge to the pins; or
   - pick a part number with a public maker drawing, and compare that drawing with the footprint
     hole by hole.
   After the choice: set the footprint, set the 3D model, delete the `FootprintCheck` field on J1.
2. **WEMOS S2 mini footprint.** Hold the real board on `HDLC-ESP32-S2/wemos-s2-mini-footprint-check.pdf`.
   The outline, the notch and the two 3 mm holes near one end are copied from the KiCad C3 mini
   footprint and are NOT confirmed for the S2 mini. Which end the USB connector is at is not recorded.
3. **Power for the two MAX3490E chips.** Not checked: how much current the S2 mini 3V3 pin can give.
   Read the S2 mini regulator data and the MAX3490E supply current, then compare.
4. **Clock receive pins (assumption A4).** U2 gets the ND clock on pins 15 (A) and 3 (B). This was
   copied from the old wiring, not measured. Check with a scope or with the real ND.
5. **Do the ND card straps (jumpers) matter?** The 3015 plug has 3-5, 15-17, 11-24-23, 22-10. What
   they do is not documented in the pages read. The board has solder jumpers for them. Test with and
   without.

## PCB layout (not started)

6. Create `HDLC-ESP32-S2.kicad_pcb`: board outline, DB-25 plug at the board edge so it plugs
   straight into the ND socket, S2 mini placement with the USB port reachable, MAX3490E close to the
   plug, test points reachable with a scope hook, ground return.
7. Termination and surge protection on the RS-422 pairs: not designed. Needs a decision.

## Documents to bring in line

8. `docs/HDLC-PCB-ESP32-Wiring.svg`: add test points TP1 to TP7, and use the name WEMOS S2 mini
   (it still says LOLIN S2 Mini).
9. `docs/HDLC-Cable-Measurements.md` and `docs/HDLC-3015-vs-MEGALINK-3023.md`: add that the
   male-to-male cable is flipped at one end, and that the board plugs straight into the ND (no cable).
10. Two `lib_symbol_mismatch` warnings in ERC (flattened MAX3490 symbol). Harmless, tidy later.

## Not KiCad, but this board depends on it

11. ESP32 firmware at 307.2 kbps or less is untested on this board (firmware lives in `src/`).
