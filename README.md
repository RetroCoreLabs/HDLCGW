# XMSGGW

An XMSG node for a Norsk Data machine on an ESP32-S2 (LOLIN S2 Mini), on the same board and
synchronous HDLC line as HDLCGW. Where HDLCGW passes line frames to a PC, XMSGGW runs the XMSG node
itself with libxmsg: LAPB to the machine, then (later) terminal sessions for telnet users and COSMOS
file access.

## Status

Skeleton, started 2026-11-09. Builds with no warnings (flash 28%, static RAM 24%; the node itself
is 10,888 bytes). It has **not run on a board yet**.

Done:

- the line through the libhdlc ESP32-S2 SPI tasks, with the libhdlc decoder in per-frame mode;
- a libxmsg node in raw framing; libxmsg adds and checks the FCS;
- the USB port works the way HDLCGW's does: control frames from a host program (HELLO, COMMAND,
  LOG_LEVEL, STAT_PUSH, PING; replies, LOG, EVENT and STAT back), and a text console after three
  ESC presses. STAT frames use HDLCGW's field order; the WiFi and TCP fields are always 0;
- commands: `HELP`, `STAT`, `NODE`, `LINK START|STOP`, `BAUD`, `CLOCK`, `INSPECT [BAD n]`, `LOG`,
  `TASK`, `INFO`, `RESET`, `EXIT`. No login: whoever holds the USB cable is the administrator;
- the node number, line rate, Flags1 counters and port-word seed kept in NVS.

Not yet: telnet, users and levels, the forced password change, the hash port through mbedTLS, and
a file-transfer (FTP) worker task beside the node task. Those come after the RAM has been measured
on the board.

## Tasks

| Task | Priority | Stack | Job |
|---|---|---|---|
| SPI rx | 24 | 2048 | line receive (HDLCGW's measured setting) |
| SPI tx | 8 | 2048 | line transmit |
| Node | 4 | 8192 | libhdlc decoder and the libxmsg node; the only task that touches the node |
| Console | 2 | 4096 | USB input: control frames and the text console |
| USB tx | 1 | 2048 | USB output ring to the host |

## Building

Needs the libhdlc and libxmsg libraries in the Arduino `libraries` folder (a link to each
checkout is enough). Board `esp32:esp32:lolin_s2_mini`, Arduino ESP32 core 3.3.12.

```sh
arduino-cli compile --fqbn esp32:esp32:lolin_s2_mini --warnings all .
```

## First start

With no node number stored the link stays down. Send `NODE <n>`, then `RESET` (as a COMMAND
control frame, or on the text console).

The bench gateway is node **101**, named **D101** (Ronny, 2026-11-09): `NODE 101`, then `RESET`.
The name has no setting yet.
