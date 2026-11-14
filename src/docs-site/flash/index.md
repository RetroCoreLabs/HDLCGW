<!-- The flash page of the documentation site. src/tools/gen_docs.py copies this folder;
     the docs workflow puts the newest release's .bin files next to it. -->
# Flash from the browser

This page writes the firmware of the newest
[release](https://github.com/RetroCoreLabs/HDLCGW/releases) to a board on a USB port. It needs
Chrome or Edge on a computer (the browser has to reach a serial port).

!!! warning "For the LOLIN S2 Mini only"
    This firmware is built for one board: the
    [WEMOS / LOLIN S2 Mini](https://www.wemos.cc/en/latest/s2/s2_mini.html) (ESP32-S2, 4 MB
    flash, with PSRAM). It will not run on another ESP32 board, not even another ESP32-S2
    board: the pins, the PSRAM and the flash layout are those of the S2 Mini.

On the board, the small LED (GPIO 15) shows the WiFi state: it blinks fast when no network is
stored, blinks slowly while it tries to join, and stays on once it has joined.

Put the board in **download mode** first: hold the **0** button, press and let go of **RST**,
then let go of **0**. When the browser is done, press **RST** to start the firmware.
A brand-new board often shows no serial port at all until it is in download mode, so do this
first even if the browser lists nothing.

The slots, the settings and what erases them are on the
[install page](../docs/INSTALL.md).

<script type="module" src="https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"></script>

## Both firmwares

Bootloader, partition table, HDLCGW in slot 1 and XMSGGW in slot 2. The board starts HDLCGW.
Tick "erase" only for a new board: it erases the settings too (WiFi, users, node number).

<esp-web-install-button manifest="both.json"></esp-web-install-button>

## Only HDLCGW

Writes slot 1 and nothing else. The settings and XMSGGW stay.

<esp-web-install-button manifest="hdlcgw.json"></esp-web-install-button>

## Only XMSGGW

Writes slot 2 and nothing else. The settings and HDLCGW stay. Start it afterwards with
`FIRMWARE SWITCH` or the button.

<esp-web-install-button manifest="xmsggw.json"></esp-web-install-button>

## Pins used on the S2 Mini

Both firmwares use the same pins. The line signals go through RS-422 drivers (MAX3490E) to the
ND card; the full wiring is in
[the breadboard drawing](https://github.com/RetroCoreLabs/HDLCGW/blob/master/docs/HDLC-Breadboard-Wiring.svg).

| GPIO | Direction | Signal |
|---|---|---|
| 11 | out | Transmit data (TD) |
| 7 | out | Transmit clock, sent to the far end |
| 9 | in | Receive data (RD) |
| 12 | in | Receive clock, from the far end |
| 15 | out | On-board LED: WiFi state |
| 3 | out | Green LED: HDLCGW is running |
| 1 | out | Red LED: XMSGGW is running |
| 5 | in | Button: held at start, it switches firmware |
