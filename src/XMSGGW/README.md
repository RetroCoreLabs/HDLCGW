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

On 2026-11-10, after the move to libxmsg's shared console, checked live: login, `HELP`, `LIST`,
`NAMES`, `INFO`, `WHO`, `CONNECT D100` (the TDV SLUTT and HJELP keys work in S3-CONFIG),
`CONNECT-TO D101` from D100's own terminal (the same login and prompt), `USERS`, `ADDUSER`,
`DELUSER`, and `LI-SYS D101` at D100's prompt (the real count: 1 in use, 7 free with one telnet
user, from libxmsg ef0e53a on).

Last build: flash 77%, static RAM 30%. The node and the session buffers live in PSRAM (2 MB);
internal RAM is short.

Line speed: the line runs at whatever `BAUD` stored, and at 38400 when nothing is stored. The bench
runs at 307200 (both boards set with `BAUD 307200`, 2026-11-10). Long output from SINTRAN is not
limited by the line: the same `HELP` listing took 5.8 s through XMSGGW and 7.0 s between D100 and
D102 with no gateway in the path. SINTRAN sends two 255-byte frames and then waits 200-500 ms.
A test at 900000 (one `HELP` listing, 2026-11-10) lost no frame and took 4.4 s; the bench went
back to 307200 afterwards.

Not yet:

- `li-rout` on D100 hung at node 101's line in an earlier build; not checked again since `LIST`
  was built.
- an unknown user name takes about 23 s to be refused: libxmsg spends a dummy password check at its
  own 100,000 rounds, and the time also tells whether a name exists. To be fixed in libxmsg.
- once, after a flash, the link stayed "connected" in LAPB but "down" in the node until
  `LINK STOP` and `LINK START`. Cause not found.
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
| `CONNECT <system>` | terminal session to a SINTRAN system by name or number, e.g. `CONNECT D100` |
| `LIST` | the routing table of the host we are linked to |
| `NAMES` | the system names and named ports the host knows |
| `WHO` | the users on this gateway |
| `INFO` | link state, line speed, frame and datagram counts, version and build time |
| `PASSWORD` | change your own password |
| `HELP` | the commands |
| `EXIT`, `LOGOUT`, `QUIT` | close the connection |
| `USERS` | the users (admin) |
| `ADDUSER name [admin]` | add a user; the new user must change the password at first login (admin) |
| `DELUSER name` | delete a user; the last admin cannot be deleted (admin) |
| `RESETPW name` | set a new password, to be changed at the next login (admin) |
| `NODE [n]` | show or store our node number, used after `RESTART` (admin) |
| `RESTART` | restart the gateway; every session ends (admin) |

The same login and command line answer a SINTRAN user who runs `CONNECT-TO D101` (up to 4 at
once). Login and commands are libxmsg's shared console (`console/console.h`); `gw_console.cpp`
plugs in the gateway's services.

In a session: ESC on its own breaks the running program (sent as an escape, not as a byte); an ESC
followed at once by more bytes is a function key and goes through unchanged. Log out of SINTRAN to
come back to the prompt.

## FTP

An FTP server on port 21, for one FTP user at a time. Passive mode uses data port 2121; active
mode (`PORT`, `EPRT`) connects back to the client, but only to the client's own address. Log in
with a gateway user, the same ones as telnet. The folders:

| Folder | What it shows |
|---|---|
| `/` | the machines, by the names read from the host (D100, D102, ...), without this gateway |
| `/D100` | one folder: the SINTRAN user set for your gateway user |
| `/D100/SYSTEM` | that user's files, with size and date; download them (`RETR`) |

A SINTRAN file `NAME:TYPE` is named `NAME.TYPE` in FTP. A download is read whole from SINTRAN
through libcosmos' COSMOS file client into PSRAM (at most 1 MB) and sent to the client as it
arrives. A file SINTRAN refuses gives `550` with SINTRAN's error number.

A folder listing walks SINTRAN's directory one file at a time: 253 files took 60 s
(2026-11-11). Each version of a file is its own entry and the version number is not in it, so a
name can appear more than once. The size is SINTRAN's byte count (0 for a device file); the date
is the last write, else the creation, in SINTRAN's own time (it keeps no time zone). `SIZE` and
`MDTM` ask SINTRAN for that one file. `NLST CHAT*` lists the files whose FTP name matches the
pattern.

An admin sets each gateway user's SINTRAN account on the `XMSGGW>` command line:

| Command | What it does |
|---|---|
| `SINTRAN` | list the accounts (passwords are never shown) |
| `SINTRAN <gateway-user> <sintran-user> <password>` | set one; the password may be left out |
| `SINTRAN <gateway-user> -` | remove one |

The SINTRAN password has to be sent to SINTRAN, so it is stored on the board as typed, not
hashed. FTP itself sends the gateway password in clear, as telnet does.

Not built yet:
- writing, deleting and renaming files;
- showing only the machines that are online now: today `/` lists every machine the host knows by
  name.

Tested 2026-11-11: downloads of 255, 5035 and 6835 bytes came back at the sizes SINTRAN's
FILE-STATISTICS gives, with curl (passive and active), tnftp (passive) and Windows `ftp.exe`
(active), from D100 and from D102 through D100. A missing file is refused with SINTRAN error 46.
One curl active-mode download failed once ("server failed to connect to data port") and did not
fail again in five more tries; the cause is not known. Listing tested with curl, WinSCP
(`ls`, `get`), tnftp (`nlist CHAT*`) and Windows `ftp.exe` (`dir`).

## Files on the console

Three commands on the `XMSGGW>` console read files on the SINTRAN machines, written the way
SINTRAN writes a file: `machine(user(password)).NAME:TYPE`. `HELP` shows them with examples.

| Command | What it does |
|---|---|
| `LIST-FILES D100(SYSTEM)` | every file of SYSTEM on D100 |
| `LIST-FILES D100(SYSTEM).:MODE` | every MODE file (SINTRAN does the matching) |
| `FILE-STAT D100(SYSTEM).CHATKT:MODE` | the file's size, dates and use count, as SINTRAN's FILE-STATISTICS |
| `COPY-TERM D100(SYSTEM).LOAD-MODE:MODE` | the file shown on the console, bit 7 cleared |

Leave out the password and the stored one is used for your own SINTRAN user; leave out the user
and your SINTRAN account (the `SINTRAN` command) is used. Spaces work too:
`LIST-FILES D100 SYSTEM :SYMB`. One file command runs at a time on the gateway; another user
is told to try again.

The escape key stops the output and brings the prompt back at once, but the gateway goes on
reading from SINTRAN until the listing or file has ended (up to a minute for a big folder), and
until then a new file command is told to try again. Stopping SINTRAN early needs a stop call in
libcosmos' file client, asked for 2026-11-11.

Not shown yet: the file number and version (`;1`) SINTRAN prints, which are not in the entry the
file client hands over, and the access lines (`PUBLIC ACCESS : READ`...), shown as the raw access
word because its bits are not decoded yet.

Tested 2026-11-11: LIST-FILES with no filter, `:MODE` (18 files) and `:SYMB` (17), on D100 and on
D102 through D100; FILE-STAT for one file and for `:MODE`, its lines matching SINTRAN's own for the
same files; COPY-TERM of LOAD-MODE:MODE; a missing file; escape during a running command.

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
| `PARITY [ON\|OFF]` | even parity on typed bytes for new sessions (ON default) |
| `ALIAS [<name> <system number>\|DEL <name>]` | names `CONNECT` takes, besides the ones learned from the host |
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
| Node | 4 | 12288 | libhdlc decoder, the libxmsg node, the terminal sessions, and the login and command line (libxmsg console); the only task that touches the node |
| Console | 2 | 4096 | USB input: control frames and the text console |
| USB tx | 1 | 2048 | USB output ring to the host |
| Telnet | 1 | 6144 | WiFi and the telnet connections: bytes in and out through per-connection queues |

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
