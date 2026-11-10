# docs

Reference material for the HDLCGW board. Back to the [repository front page](../README.md).

## Written here

| File | What it is |
|---|---|
| [HDLC-Cable-Measurements.md](HDLC-Cable-Measurements.md) | The measurement sheet and the measured DB-25 pinout of the ND HDLC socket. |
| [HDLC-3015-vs-MEGALINK-3023.md](HDLC-3015-vs-MEGALINK-3023.md) | What differs between the two ND line cards the board plugs into (3015 HDLC and 3023 MEGALINK). |
| [HDLC-PCB-ESP32-Wiring.svg](HDLC-PCB-ESP32-Wiring.svg) | Wiring of the PCB: S2 Mini, the two MAX3490E drivers and the DB-25, with the real ND pin numbers. |
| [HDLC-Breadboard-Wiring.svg](HDLC-Breadboard-Wiring.svg) | Wiring of the old breadboard, with the numbers as printed on that board. |

The schematic itself is in [`../kicad/`](../kicad/README.md).

## Datasheets

| File | What it is |
|---|---|
| [esp32-s2_datasheet_en.pdf](esp32-s2_datasheet_en.pdf) | Espressif ESP32-S2 datasheet. |
| [esp32_s2_datasheet_en-1773066.pdf](esp32_s2_datasheet_en-1773066.pdf) | A second copy of the ESP32-S2 datasheet, from a distributor download (which revision each one is has not been compared). |
| [MAX3483-MAX3491.pdf](MAX3483-MAX3491.pdf) | Maxim MAX3483-MAX3491 RS-422/485 transceivers; the board uses the MAX3490E. |
