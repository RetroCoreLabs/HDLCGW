# XMSGGW

An XMSG node for a Norsk Data machine on an ESP32-S2 (LOLIN S2 Mini), on the same board and
synchronous HDLC line as HDLCGW. Where HDLCGW passes line frames to a PC, XMSGGW runs the XMSG node
itself with libxmsg: LAPB to the machine, terminal sessions for telnet users, and (later) COSMOS
file access.

## Status

Runs on the board. Tested on the bench against the D100 emulator (node 100), XMSGGW as node 101
(2026-11-09 and 2026-11-10):

- the LAPB link comes up, and the reachability ("are you there?") exchange is answered, with 0 bad
  frames;
- telnet on port 23: login, the `XMSGGW> ` command line, `CONNECT D100`, a SINTRAN login and
  `LOGOUT` (back at the prompt, error 0), `EXIT`;
- 8 telnet users at once, a 9th turned away; two SINTRAN logins at the same time;
- a client that drops its TCP connection ends its SINTRAN session at once.

Last build: flash 74%, static RAM 29%. The node and the session buffers live in PSRAM (2 MB);
internal RAM is short (about 28 KB free at the start).

Not yet:

- `LIST` says "not implemented yet". It is to ask D100 for its routing table (list-routing-info)
  and show every system D100 reaches. The libxmsg code for it is being written.
- `li-rout` on D100 hangs at node 101's line: XMSGGW does not answer D100's routing question yet.
  Same libxmsg work.
- an unknown user name takes about 23 s to be refused: libxmsg spends a dummy password check at its
  own 100,000 rounds, and the time also tells whether a name exists. To be fixed in libxmsg.
- once, after a flash, the link stayed "connected" in LAPB but "down" in the node until
  `LINK STOP` and `LINK START`. Cause not found.
- the TDV HELP and EXIT keys: not proven to work. `TTYP 93` (terminal type of a TDV 2200) is the
  lead for HELP; the bytes of the EXIT key are not known.
- a file-transfer (FTP) worker task.

## Telnet

Join the WiFi network first (`WIFI SSID`, `WIFI PASS`, `WIFI CONNECT` on the USB console; with
none stored, the network HDLCGW stored on the same board is used). Then `telnet <board ip>`.

A new board has one user: **`admin`, password `admin`**. The first login must set a new password.
Passwords are kept as PBKDF2-SHA256 hashes (2,000 rounds, about 0.5 s per check on this chip; the
library default of 100,000 took 23 s) in NVS, never as text.

Commands at the `XMSGGW> ` prompt:

| Command | What it does |
|---|---|
| `CONNECT <system>` | terminal session to a SINTRAN system, e.g. `CONNECT D100` |
| `LIST` | the systems that can be reached (not implemented yet) |
| `WHO` | the users on this gateway |
| `INFO` | version, build time, link state, last password check time |
| `PASSWORD` | change your own password |
| `HELP` | the commands |
| `EXIT`, `LOGOUT`, `QUIT` | close the connection |
| `USERS` | the users (admin) |
| `ADDUSER name [admin]` | add a user; the new user must change the password at first login (admin) |
| `DELUSER name` | delete a user; the last admin cannot be deleted (admin) |
| `RESETPW name` | set a new password, to be changed at the next login (admin) |

In a session: ESC on its own breaks the running program (sent as an escape, not as a byte); an ESC
followed at once by more bytes is a function key and goes through unchanged. Log out of SINTRAN to
come back to the prompt.

## USB console

Control frames from a host program (HELLO, COMMAND, LOG_LEVEL, STAT_PUSH, PING; replies, LOG,
EVENT and STAT back), the same way HDLCGW works, or a text console after three ESC presses. STAT
frames use HDLCGW's field order; the TCP fields are always 0. No login: whoever holds the USB
cable is the administrator.

| Command | What it does |
|---|---|
| `HELP` | all commands |
| `STAT` | line, node and USB counters |
| `NODE [n]` | show or store our node number (used after `RESET`) |
| `LINK START\|STOP` | start the link or take it down |
| `REACH` | ask the peer "are you there?"; `STAT` shows asked / answered |
| `TERM CONNECT <system>\|TYPE <text>\|SHOW\|STATUS\|HANGUP` | a terminal session from the console |
| `WIFI [STATUS]\|SSID <name>\|PASS <password>\|CONNECT` | the network for telnet; the password is never shown |
| `TTYP [n]` | terminal type for new sessions (0 default; 93 = TDV 2200) |
| `BAUD <value>\|MAX\|DEFAULT` | the transmit clock (stored) |
| `CLOCK ON\|OFF` | run or stop the transmit clock |
| `INSPECT [BAD [0..3]]` | the last frame, or the n-th newest bad one |
| `LOG [0..5]` | 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace |
| `TASK` | the running tasks |
| `INFO` | build, chip and memory |
| `RESET` | restart the gateway |
| `EXIT` | leave the text console, back to control frames |

Stored in NVS: the node number, line rate, Flags1 counters, port-word seed, WiFi network, terminal
type (namespace `xmsggw`) and the users (namespace `xmsgusr`).

## Tasks

| Task | Priority | Stack | Job |
|---|---|---|---|
| SPI rx | 24 | 2048 | line receive (HDLCGW's measured setting) |
| SPI tx | 8 | 2048 | line transmit |
| Node | 4 | 8192 | libhdlc decoder, the libxmsg node and the terminal sessions; the only task that touches the node |
| Console | 2 | 4096 | USB input: control frames and the text console |
| USB tx | 1 | 2048 | USB output ring to the host |
| Telnet | 1 | 6144 | WiFi, telnet users, login and the command line (least free measured: 2852 bytes) |

## Building

Needs the libhdlc and libxmsg libraries in the Arduino `libraries` folder (a link to each
checkout is enough). Board `esp32:esp32:lolin_s2_mini`, Arduino ESP32 core 3.3.12. `build_opt.h`
sets `XMSG_MAX_SESSIONS=12` (8 telnet sessions and the console's one need 9).

```sh
arduino-cli compile --clean --fqbn esp32:esp32:lolin_s2_mini --warnings all .
```

Use `--clean`: without it the build time that `INFO` shows can come from an older build.

## First start

With no node number stored the link stays down. Send `NODE <n>`, then `RESET` (as a COMMAND
control frame, or on the text console).

The bench gateway is node **101**, named **D101** (Ronny, 2026-11-09): `NODE 101`, then `RESET`.
The name has no setting yet.
