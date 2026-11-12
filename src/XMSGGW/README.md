# XMSGGW

An XMSG node for a Norsk Data machine on an ESP32-S2 (LOLIN S2 Mini), on the same board and
synchronous HDLC line as HDLCGW. Where HDLCGW passes line frames to a PC, XMSGGW runs the XMSG node
itself with libxmsg: LAPB to the machine, terminal sessions for telnet users, and (later) COSMOS
file access.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [Status](#status)
- [Telnet](#telnet)
- [FTP](#ftp)
  - [Every FTP command](#every-ftp-command)
- [Files on the console](#files-on-the-console)
- [USB console](#usb-console)
- [Tasks](#tasks)
- [Building](#building)
- [First start](#first-start)

<!-- /contents -->

## Status

Runs on the board. Tested on the bench against the D100 emulator (node 100), XMSGGW as node 101
(2026-11-09 and 2026-11-10):

- the LAPB link comes up, and the reachability ("are you there?") exchange is answered, with 0 bad
  frames;
- telnet on port 23: login, the `XMSG> ` command line, `CONNECT D100`, a SINTRAN login and
  `LOGOUT` (back at the prompt, error 0), `EXIT`;
- 8 telnet users at once, a 9th turned away; two SINTRAN logins at the same time;
- a client that drops its TCP connection ends its SINTRAN session at once.

On 2026-11-10, after the move to libxmsg's shared console, checked live: login, `HELP`, `LIST`,
`NAMES`, `INFO`, `WHO`, `CONNECT D100` (the TDV SLUTT and HJELP keys work in S3-CONFIG),
`CONNECT-TO D101` from D100's own terminal (the same login and prompt), `USERS`, `ADDUSER`,
`DELUSER`, and `LI-SYS D101` at D100's prompt (the real count: 1 in use, 7 free with one telnet
user, from libxmsg ef0e53a on).

Last build (2026-11-12): flash 82% of its slot, static RAM 30%. The node and the session buffers live in PSRAM (2 MB);
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

Join the WiFi network first: the USB text console asks for it when none is stored (it lists the
networks it can see), or `WIFI SSID`, `WIFI PASS`, `WIFI CONNECT`. The network is shared with
HDLCGW, so one set in either firmware is used by both. Then `telnet <board ip>`.

A new board has one user: **`admin`, password `admin`**. The first login must set a new password.
Passwords are kept as PBKDF2-SHA256 hashes (2,000 rounds, about 0.5 s per check on this chip; the
library default of 100,000 took 23 s) in NVS, never as text.

Commands at the `XMSG> ` prompt:

| Command | What it does |
|---|---|
| `CONNECT <system>` | terminal session to a SINTRAN system by name or number, e.g. `CONNECT D100` |
| `LIST` | the routing table of the host we are linked to |
| `NAMES` | the system names and named ports the host knows |
| `WHO` | the users on this gateway: the telnet sessions, and a line marked `FTP` for a connected FTP user |
| `INFO` | link state, line speed, frame and datagram counts, version and build time; who is on FTP (user, address, how long) and the FTP counts since start |
| `PASSWORD` | change your own password |
| `HELP` | the commands |
| `EXIT`, `LOGOUT`, `QUIT` | close the connection |
| `USERS` | the users (admin) |
| `ADDUSER name [admin]` | add a user; the new user must change the password at first login (admin) |
| `DELUSER name` | delete a user; the last admin cannot be deleted (admin) |
| `RESETPW name` | set a new password, to be changed at the next login (admin) |
| `NODE [n]` | show or store our node number, used after `RESTART` (admin) |
| `RESTART` | restart the gateway; every session ends (admin) |
| `FIRMWARE [SWITCH]` | show both firmware slots; `SWITCH` restarts into HDLCGW (admin) |

An admin who logs in while no node number is stored is asked for one: a number typed next is
stored (`RESTART` then starts the link), any other command skips the question.

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
| `/D100/SYSTEM` | that user's files, with size and date; download (`RETR`) and upload (`STOR`) |

A SINTRAN file `NAME:TYPE` is named `NAME.TYPE` in FTP. A download is read from SINTRAN
through libcosmos' COSMOS file client and sent to the client as it arrives, through a 256 KB
buffer in PSRAM; there is no size limit. A file SINTRAN refuses gives `550` with SINTRAN's error number.
A file with no bytes in it (FILE-STATISTICS "1 PAGE , 0 BYTES", such as `RONNY-R:TXT` on D100)
downloads as an empty file: SINTRAN leaves the length out of its answer when it opens one, which
libxmsg reads as 0 bytes since c0b0c8b, as SINTRAN's own COPY-FILE does ("SOURCE EMPTY").
Tested 2026-11-12.

An upload (`STOR`) is taken whole into PSRAM (at most 1 MB) and then written to SINTRAN as a new
file under the folder's SINTRAN user. A file that exists is replaced: the gateway first asks for a
new file, SINTRAN refuses that with error 62 (octal 76) and leaves the file as it was, and the
gateway then asks again to write over it. Tested 2026-11-12 with 2000, 3000 and then 500 bytes:
each read back unchanged, and FILE-STATISTICS showed 500 bytes after the last one (SINTRAN keeps
the 2 pages the larger version used). An empty file is refused, because no capture shows how
SINTRAN writes one.

`TYPE A` (ASCII, the start value) clears bit 7 of every byte on the way out, and sets it to even
parity on the way in, which is how SINTRAN keeps text (all 4780 bytes of STARTX3:MODE on D100 have
even parity). `TYPE I` (binary) moves the bytes unchanged both ways. Neither changes line ends:
SINTRAN text already ends its lines with CR LF.

A folder listing walks SINTRAN's directory one file at a time: 253 files took 60-66 s
(2026-11-11 and 2026-11-12), about 0.2 s a file. Each file is one request to SINTRAN and its answer;
SINTRAN's own file client lists at the same rate (libxmsg decoded a capture of its LIST-FILES), so
the time is SINTRAN's, not the gateway's or FTP's. The listing is sent as it arrives (the first
line after about 2.4 s), but graphical FTP programs such as WinSCP draw a folder only when the whole
listing is in, so they look stuck for that minute. Each version of a file is its own entry, so a
name can appear more than once. `ls`
(NLST) gives the names, `dir` (LIST) the long form, as on SINTRAN's own FTP server. The long form
is `ls -l` lines, because graphical FTP programs read only that, with SINTRAN's facts in them:

```
-rw-rw-r--    3 SYSTEM   rwacdrwacdr----       4780 Aug 25  1998 STARTX3.MODE
```

The permission bits are the own, friend and public read and write rights; the number after them
is the page count; then SINTRAN's full user name, the access letters (own, friend, public, each
`rwacd`), the byte count (0 for a device file), and the date: the last write, else the creation,
in SINTRAN's own time (it keeps no time zone).

What SINTRAN's own FTP server (TCP/IP C07, 1990) does, and the gateway copies, plus one switch
of the gateway's own:

| Command | What it does |
|---|---|
| `LIST ECHO` | the files whose name starts so; SINTRAN does the matching, as for LIST-FILES |
| `CONV` | names as SINTRAN writes them, `NAME:TYPE;1`, and back to `NAME.TYPE`. SINTRAN's server has it; the gateway also takes `SITE CONV` |
| `SITE TABLE` | the gateway's own: LIST prints SINTRAN's table (below) instead of `ls -l` lines, and back. SINTRAN's server always prints the table and knows no `SITE` |
| `HELP` | the commands |

SINTRAN's own server cannot run a SINTRAN command over FTP: its program (`FTP-SERVER-C07:PROG`)
only reads, writes, lists and deletes files, logs in and out, and switches `CONV` (checked on the
program and on a live server, 2026-11-12). The gateway has no such command either.

SINTRAN's table, as its own server prints it (the file number is 3 wide, `100` and ` 73`):

```
(PACK-ONE:SYSTEM)
 no. own  fri  pub    pgs    bytes  last modified    name
 73 rwacdrwacdr----     4     7471  27 Sep 98 18:14 ECHOPL:PLNC;1
```

`SIZE` and `MDTM` ask SINTRAN for that one file. `NLST CHAT*` lists the files whose name matches
the pattern; the gateway matches that one itself.

### Every FTP command

All of them, as `ftp_server.cpp` handles them. Commands are not case sensitive. Only `USER`,
`PASS`, `QUIT`, `NOOP`, `HELP`, `FEAT`, `OPTS` and `SYST` work before login; the rest answer
`530 Log in first.`

| Command | What the gateway does |
|---|---|
| `USER name` | starts a login with a gateway user (the same users as telnet); `331` |
| `PASS password` | ends the login; `230`. A wrong name or password gets `530`, and the third wrong try ends the session. A user whose password must be changed gets `530` and has to log in over telnet once. After login TYPE is A, and CONV and SITE TABLE are off |
| `QUIT` | `221`, and the session ends |
| `NOOP` | `200 OK.` |
| `HELP` | `214`: the command list and a short note on folders, names, TYPE, CONV, SITE TABLE and STOR |
| `FEAT` | `211`: EPSV, MDTM, PASV, SIZE, UTF8 |
| `OPTS ...` | `200 OK.` (for `OPTS UTF8 ON`; names are plain ASCII either way) |
| `SYST` | `215 UNIX Type: L8`, so clients expect `ls -l` lines |
| `PWD`, `XPWD` | `257` and the current folder, such as `"/D100/SYSTEM"` |
| `CWD folder`, `XCWD` | moves to a folder (`/`, `/D100`, `/D100/SYSTEM`; `..` and relative names work); `250`, or `550` with the reason |
| `CDUP`, `XCUP` | one folder up |
| `TYPE A` (or `A N`) | ASCII, the start value: bit 7 is cleared on download and set to even parity on upload |
| `TYPE I` (or `L 8`) | binary: bytes unchanged both ways. Any other type gets `504` |
| `MODE`, `STRU` | `200 OK.` and nothing changes (stream mode, file structure only) |
| `PASV` | passive mode on data port 2121; `227` |
| `EPSV` | the same; `229` |
| `PORT`, `EPRT` | active mode: the gateway connects back, but only to the client's own address |
| `LIST [NAME]` | `dir`: the long form, `ls -l` lines (or SINTRAN's table after `SITE TABLE`). In a user folder, `NAME` is passed to SINTRAN as a name filter, as SINTRAN's own server does |
| `NLST [NAME]` | `ls`: names only. `NAME` filters as for LIST; a pattern with `*` (`CHAT*`) is matched by the gateway |
| `RETR NAME.TYPE` | downloads a file from the folder's SINTRAN user, streamed as it arrives, no size limit |
| `STOR NAME.TYPE` | uploads a file (at most 1 MB, not empty). A file that exists is replaced. `226`, or `550` with SINTRAN's error |
| `SIZE NAME.TYPE` | `213` and the byte count, asked from SINTRAN; 0 for a file SINTRAN keeps no byte count for, as the listing shows |
| `MDTM NAME.TYPE` | `213 YYYYMMDDhhmmss`: the last write, else the creation, in SINTRAN's own time |
| `CONV`, `SITE CONV` | switches names between `NAME.TYPE` and SINTRAN's `NAME:TYPE;1`; `200` |
| `SITE TABLE` | switches LIST between `ls -l` lines and SINTRAN's own table; `200`. Any other SITE gets `504` |
| `DELE`, `RNFR`, `RNTO`, `APPE` | `550`: deleting, renaming and appending are not built yet |
| `MKD`, `XMKD`, `RMD`, `XRMD` | `550`: the folders are the machines and their SINTRAN users and cannot be made or removed |
| anything else | `502 Command not implemented.` |

A file name may also be written `NAME:TYPE`. Limits: one FTP user at a time (a second gets
`421`); a session with no command and no transfer for 30 minutes is closed with `421`; a data
connection that has not opened after 10 seconds is given up with `425`.

An admin sets each gateway user's SINTRAN account on the `XMSGGW>` command line:

| Command | What it does |
|---|---|
| `SINTRAN` | list the accounts (passwords are never shown) |
| `SINTRAN <gateway-user> <sintran-user> <password>` | set one; the password may be left out |
| `SINTRAN <gateway-user> -` | remove one |

The SINTRAN password has to be sent to SINTRAN, so it is stored on the board as typed, not
hashed. FTP itself sends the gateway password in clear, as telnet does.

Not built yet:
- deleting files (SINTRAN's own server has `DELE`), appending and renaming them (SINTRAN's server has neither);
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
| `LIST-FILES D102(SYST).C` | every file starting with C; the user may be abbreviated |
| `FILE-STAT D100(SYSTEM).CHATKT:MODE` | the file's size, dates and use count, as SINTRAN's FILE-STATISTICS |
| `COPY-TERM D100(SYSTEM).LOAD-MODE:MODE` | the file shown on the console, bit 7 cleared |

After the dot comes the filter, passed to SINTRAN exactly as typed: `:MODE` for every MODE file,
`C` for every file starting with C, `CHATKT:MODE` for one file; no filter means every file. A
capture of SINTRAN's own LIST-FILES from D100 to D102 (2026-11-11) shows the request carries
`(USER)FILTER'` as typed, an abbreviated user such as `SYST` included, and the remote SINTRAN does
the matching; the gateway sends the same. SINTRAN's own command needs the dot (`D102(SYSTEM)C` is
refused there); the gateway also takes the filter without it.

Leave out the password and the stored one is used for your own SINTRAN user; leave out the user
and your SINTRAN account (the `SINTRAN` command) is used. Spaces work too:
`LIST-FILES D100 SYSTEM :SYMB`. One file command runs at a time on the gateway; another user
is told to try again.

The escape key stops a running command at once: the output, and SINTRAN's work too (the file
client's stop call), so the next command can start straight away.

Not shown yet: the file number and version (`;1`) SINTRAN prints, which are not in the entry the
file client hands over. The access lines (`PUBLIC ACCESS : READ`, `FRIEND ACCESS : ...`,
`OWN ACCESS : ...`) are printed in SINTRAN's own words.

Tested 2026-11-11: LIST-FILES with no filter, `:MODE` (18 files) and `:SYMB` (17), on D100 and on
D102 through D100; FILE-STAT for one file and for `:MODE`, its lines matching SINTRAN's own for the
same files; COPY-TERM of LOAD-MODE:MODE; a missing file; escape during a running command.

## USB console

Control frames from a host program (HELLO, COMMAND, LOG_LEVEL, STAT_PUSH, PING; replies, LOG,
EVENT and STAT back), the same way HDLCGW works, or a text console after three ESC presses (prompt `XMSG#`; telnet shows `XMSG>`). STAT
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
| `WIFI [STATUS]\|SSID <name>\|PASS <password>\|CONNECT` | the network for telnet |
| `WIFI SCAN` | list the networks the board can see (about 3 s, 2.4 GHz only) |
| `WIFI SHOW` | show the stored network name and password; USB console only (telnet has no WIFI command) |
| `TTYP [n]` | terminal type for new sessions (0 default; 93 = TDV 2200) |
| `PARITY [ON\|OFF]` | even parity on typed bytes for new sessions (ON default) |
| `ALIAS [<name> <system number>\|DEL <name>]` | names `CONNECT` takes, besides the ones learned from the host |
| `BAUD <value>\|MAX\|DEFAULT` | the transmit clock (stored, shared with HDLCGW) |
| `CLOCK ON\|OFF` | run or stop the transmit clock |
| `INSPECT [BAD [0..3]]` | the last frame, or the n-th newest bad one |
| `LOG [0..5]` | 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace |
| `TASK` | the running tasks |
| `INFO` | build, chip and memory |
| `RESET` | restart the gateway |
| `FIRMWARE [SWITCH]` | show both firmware slots; `SWITCH` restarts into HDLCGW (also on telnet: `FIRMWARE` for every user, `SWITCH` for the admin) |
| `EXIT` | leave the text console, back to control frames |

Stored in NVS: the node number, Flags1 counters, port-word seed, terminal type, parity and aliases
(namespace `xmsggw`), the users (namespace `xmsgusr`) and their SINTRAN users (namespace
`xmsgftp`). The WiFi network and the line rate are shared with HDLCGW (namespace `board`,
`src/common/board.h`); the first start of this build copies them from the old `xmsggw` keys.

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

XMSGGW lives in slot 2. **Do not upload it from the Arduino IDE**: an upload always writes slot 1
and would replace HDLCGW. Write the built `XMSGGW.ino.bin` to `0x150000` with esptool, then start
it with `FIRMWARE SWITCH` (from HDLCGW) or the button. [Installing the firmware](../../docs/INSTALL.md)
has the commands.

## First start

With no node number stored the link stays down. The USB text console asks for it when it opens
(and for the WiFi network, when none is stored), and so does an admin telnet login. Or send
`NODE <n>`, then `RESET` (as a COMMAND control frame, or on the text console).

The bench gateway is node **101**, named **D101** (Ronny, 2026-11-09): `NODE 101`, then `RESET`.
The name has no setting yet.
