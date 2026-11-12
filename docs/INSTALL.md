# Installing the firmware

The board holds both firmwares at once, each in a slot of its own, and starts one of them. This
page says where each one goes, how to put them there (from a release, from the browser, or built
from source), how to switch between them, and what keeps the settings safe.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [The two slots](#the-two-slots)
- [Settings: shared and kept](#settings-shared-and-kept)
- [The first start: missing settings are asked for](#the-first-start-missing-settings-are-asked-for)
- [Switching between them](#switching-between-them)
- [Installing from a release](#installing-from-a-release)
- [Installing from the browser](#installing-from-the-browser)
- [Building from source](#building-from-source)

<!-- /contents -->

## The two slots

The flash is 4 MB and uses the ESP32 core's default partition table:

| Flash address | Size | What is there |
|---|---|---|
| `0x1000` | | Bootloader |
| `0x8000` | | Partition table |
| `0x9000` | 20 KB | **Settings** (the `nvs` partition): one place for both firmwares |
| `0xe000` | 8 KB | Which slot starts (`otadata`) |
| `0x10000` | 1.25 MB | **Slot 1: HDLCGW** (`app0`) |
| `0x150000` | 1.25 MB | **Slot 2: XMSGGW** (`app1`) |
| `0x290000` | | Unused (`spiffs`) |
| `0x3F0000` | 64 KB | Crash dump (`coredump`) |

The slots are fixed: HDLCGW always goes in slot 1 and XMSGGW in slot 2. The same image would run
from either slot, but the commands and this page assume that order.

Size on 2026-11-12: HDLCGW 995,579 bytes (75 % of a slot), XMSGGW 1,077,327 bytes (82 %).

## Settings: shared and kept

Both firmwares read and write the one settings partition. Each keeps its own settings under its
own name, and three settings are shared:

| Settings | Kept under | Who uses them |
|---|---|---|
| WiFi network name and password, line bit rate (`BAUD`) | `board` | both |
| TCP mode, host and port, WiFi on/off, mirror | `hdlcgw` | HDLCGW |
| Node number, terminal type, parity, aliases, port seed, Flags1 counters | `xmsggw` | XMSGGW |
| Telnet users and their passwords | `xmsgusr` | XMSGGW |
| Which SINTRAN user each gateway user is | `xmsgftp` | XMSGGW |

So the WiFi network and the line speed are set once, in either firmware, and the other one starts
with the same values. `WIFI CLEAR` in HDLCGW forgets the shared network too.

The first time a firmware with shared settings starts, it copies the WiFi network and the line
rate from where the older builds kept them (XMSGGW's own keys first, then HDLCGW's). The old keys
are left in place, so an older build still finds them.

**What keeps the settings:** switching firmware, writing one slot with esptool, and an upload
from the Arduino IDE (it writes the bootloader, the partition table, the slot choice and slot 1,
never the settings).

**What erases them:**

- `esptool erase-flash`;
- writing a full image from address 0 (the `full` image of a release, or the browser page with
  "erase" ticked): the gap between the partition table and slot 1 is written with blank bytes;
- the Arduino IDE's "Erase All Flash Before Sketch Upload: Enabled";
- a firmware built with a different ESP32 core, if that core's settings format is newer: the
  core then wipes the settings partition when it starts (the Arduino core's `initArduino()`
  does that on `ESP_ERR_NVS_NEW_VERSION_FOUND`). Build both firmwares with the same core
  (3.3.12 today).

## The first start: missing settings are asked for

When the USB text console opens (three ESC) and a setting the firmware needs is missing, it asks
for it before showing the prompt. There is no login on USB: whoever holds the cable is the admin.

- **WiFi network** (both firmwares): the board first lists the networks it can see, numbered
  (about 3 s). Answer with a number from the list or a network name, then the password (shown as
  `*`; Enter for an open network). `NONE` means this board runs without WiFi: stored, shared by
  both firmwares, and not asked again until `WIFI SSID` sets a network (or `WIFI CLEAR` in
  HDLCGW starts over). Enter skips it this time only.
- **Node number** (XMSGGW): 1 to 65535; 0 is not a node number. `RESET` starts the link with it.
- **Telnet** (XMSGGW): an admin who logs in while no node number is stored is asked for it; a
  number typed next is stored, any other command skips the question.

## Switching between them

| How | What happens |
|---|---|
| `FIRMWARE` | Shows both slots: what each holds, which one runs, which starts next. USB console of both firmwares, and the XMSGGW telnet console. |
| `FIRMWARE SWITCH` | Restarts into the other firmware. USB console of both; on telnet only for the admin. Refused when the other slot is empty. |
| The button | Hold it while the board starts (plug in, or press RST), let go when both LEDs light: the board restarts into the other firmware. |

The board has two LEDs (kicad/README.md): **green = HDLCGW runs, red = XMSGGW runs**.

**Going back by itself.** A firmware chosen by a switch is on trial for its first 10 seconds,
and its LED blinks. If it restarts in that time (a crash, or the power goes), the
bootloader starts the other firmware again, and `FIRMWARE` then says the slot "did not start the
last time". After 10 seconds the LED stays lit and the firmware is kept.

What `FIRMWARE` calls each slot comes from what the board saw running there. A slot written with
esptool and never started yet shows as "a firmware that has not run on this board yet".

Tested on the bench board, 2026-11-12: switched from XMSGGW to HDLCGW and restarted HDLCGW 3 s
later; the board came back to XMSGGW by itself, and `FIRMWARE` showed slot 1 as "did not start the
last time". A later `FIRMWARE SWITCH` chose slot 1 again, and it was kept after its 10 s. Not yet
tested: the button and the LEDs (not on the board yet).

## Installing from a release

Each release on GitHub (https://github.com/RetroCoreLabs/HDLCGW/releases) has these files:

| File | Write at | What it is |
|---|---|---|
| `HDLCGW.bin` | `0x10000` | HDLCGW, slot 1 |
| `XMSGGW.bin` | `0x150000` | XMSGGW, slot 2 |
| `bootloader.bin` | `0x1000` | Bootloader |
| `partitions.bin` | `0x8000` | Partition table |
| `boot_app0.bin` | `0xe000` | Slot choice: start slot 1 |
| `full.bin` | `0x0` | All of the above in one file, for a new board. **Erases the settings.** |

esptool comes with the Arduino ESP32 core, or `pip install esptool`. The commands below are
for esptool 5; esptool 4 spells them with an underscore (`write_flash`).

**Download mode.** The S2 Mini talks to esptool through its own USB port, and that port only
listens to esptool in download mode: hold the **0** button, press and let go of **RST**, then
let go of **0**. The board shows up as a new COM port. When esptool is done, press **RST** to
start the firmware. (The Arduino IDE does this by itself.)

New board, both firmwares, nothing to keep:

```
esptool --chip esp32s2 --port COM10 write-flash 0x0 full.bin
```

Or the same with the separate files:

```
esptool --chip esp32s2 --port COM10 write-flash 0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 HDLCGW.bin 0x150000 XMSGGW.bin
```

This one keeps the settings: it writes nothing between `0x9000` and `0xe000`.

Update one firmware, keep everything else:

```
esptool --chip esp32s2 --port COM10 write-flash 0x10000 HDLCGW.bin
esptool --chip esp32s2 --port COM10 write-flash 0x150000 XMSGGW.bin
```

Writing a slot does not change which slot starts. Use `FIRMWARE SWITCH` or the button afterwards
to run the other one.

## Installing from the browser

The documentation site has a flash page (Chrome or Edge, which can reach a serial port):
https://retrocorelabs.github.io/HDLCGW/flash/. It writes the files of the newest release with
the same addresses as above. Put the board in download mode first. The page gets its files when
the documentation site is built, so it has nothing to write until the first release exists and
the site has been built after it.

## Building from source

The repository's README has the set-up (the library link, the ESP32 core 3.3.12). From the
sketch folder:

```
arduino-cli compile --fqbn esp32:esp32:lolin_s2_mini --build-path <folder> .
```

The build folder then holds `HDLCGW.ino.bin` (or `XMSGGW.ino.bin`), `HDLCGW.ino.bootloader.bin`,
`HDLCGW.ino.partitions.bin` and `boot_app0.bin`. Those are the release files with longer names.

Flashing what you built:

- **HDLCGW**: an upload from the Arduino IDE (or `arduino-cli upload`) is fine. It writes
  slot 1, and also sets slot 1 to start next.
- **XMSGGW**: **do not upload it from the Arduino IDE.** An upload always writes slot 1, so it
  would replace HDLCGW. Write it to slot 2 with esptool instead:

  ```
  esptool --chip esp32s2 --port COM10 write-flash 0x150000 <folder>/XMSGGW.ino.bin
  ```

  then start it with `FIRMWARE SWITCH` or the button.

Both sketches compile the shared code in `src/common/` (the slots, the button, the LEDs and the
shared settings, `board.h`): each sketch has a one-line `board.cpp` that includes
`../common/board.cpp`, because Arduino only compiles files inside the sketch folder.
