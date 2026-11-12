<!-- The flash page of the documentation site. src/tools/gen_docs.py copies this folder;
     the docs workflow puts the newest release's .bin files next to it. -->
# Flash from the browser

This page writes the firmware of the newest
[release](https://github.com/RetroCoreLabs/HDLCGW/releases) to a board on a USB port. It needs
Chrome or Edge on a computer (the browser has to reach a serial port).

Put the board in **download mode** first: hold the **0** button, press and let go of **RST**,
then let go of **0**. When the browser is done, press **RST** to start the firmware.

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
