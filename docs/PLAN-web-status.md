# Plan: a web page on the gateway (status, traffic, commands)

**Next:** step 1 (measure the memory room).

Wanted (Ronny, 2026-11-23): open the board's IP address in a browser and see its state and
traffic (bytes, frames, errors, WiFi, TCP peer, chip), and be able to type a firmware command
and see its answer. Only if it does not break the firmware.

## What the board has room for (measured or read, 2026-11-23)

| What | Value | Source |
|---|---|---|
| Internal RAM free, HDLCGW running | about 11,100 bytes, least ever 10,600, largest block 7,668 | INFO on COM10 and COM24 |
| PSRAM free | about 2,012,000 bytes, largest block 1,998,836 | INFO |
| Flash used by HDLCGW | 1,009,103 of 1,310,720 bytes (77 %) | arduino-cli build |
| Web server library | `WebServer` (plain, not async) ships with core 3.3.12 | `libraries/` of the core |

Internal RAM is the tight one. Task stacks and lwIP's socket buffers come from internal RAM
(not verified for lwIP in this build: to be read from sdkconfig in step 1). So the design adds
**no new task** and keeps every buffer it owns in PSRAM.

## Design

1. **Where it runs.** Inside the existing WiFi task (`wifi_task.cpp`, 8 KB stack), which already
   loops every few ms: one `server.handleClient()` call per pass. No new task, no new stack.
   Port 80, one browser at a time (the plain `WebServer` serves one request at a time anyway).
2. **Pages.** One fixed HTML page kept in flash (about 10 KB: a table and a little script, no
   outside files, works offline). The script asks the board for numbers once a second and fills
   the table; the board never builds HTML at run time.
3. **`GET /api/status`** returns JSON, built into a PSRAM buffer from what STAT, INFO and
   WIFI STATUS already read:
   - line: rate, frames and bytes sent and received, bad FCS, aborts, decoder drops;
   - USB: frames and bytes each way, ring fill, drops;
   - WiFi/TCP: SSID, IP, signal, mode (802.11b), TCP role, peer, connected time, drops, bytes;
   - board: firmware and build, uptime, last restart cause, chip revision, serial, RAM and PSRAM
     free (so the page itself shows if it is eating memory).
   The page turns totals into rates (kbit/s, frames/s) itself.
4. **`POST /api/command`** with the command text. It is NOT run in the WiFi task: it is handed to
   the console task the same way a control-frame COMMAND from the PC is (`console_out_reply_begin`
   / `_end` already collect a reply into a frame). A third sink, "web", collects the reply into a
   PSRAM buffer (cap 16 KB, cut with a note when longer); the web side waits for it, up to 5 s.
   So commands run in one place, one at a time, exactly as from USB.
5. **Commands that are not allowed from the web page:** `WIFI SHOW` (prints the WiFi password),
   `WIFI SETUP` (asks questions), the ESC console, `SEND`/`DEBUG` traces that print without end.
   XMSGGW already has a list of commands per user (`commands_shared_check`); HDLCGW gets the same
   kind of check.
6. **Log lines.** A small ring in PSRAM (last 200 lines of firmware log output); the page shows it
   under the command box, read with `GET /api/log?since=<n>`.
7. **Both firmwares.** The page, the JSON writer and the sink go in `src/common/`; each firmware
   fills in its own numbers (XMSGGW: LAPB state, peer, sessions, NAMES instead of line frames).

8. **Clock (Ronny, 2026-11-23).** As soon as WiFi connects, the board asks an internet time
   server (NTP, `configTime()` in the core; server and time zone stored with the other settings,
   default `pool.ntp.org` and Norway's zone) and asks again after every reconnect. The page shows
   the board's clock, ticking with the once-a-second refresh, and "no time yet" until the first
   answer. INFO and WIFI STATUS show it too. Log lines get the clock time once it is known.
9. **Two switches (Ronny, 2026-11-23).**
   - Build switch `WITH_WEB` (default 1). With 0 none of the web code is compiled in: no flash,
     no RAM, no port 80. For a build that must be as lean as possible.
   - Command `WEB ON|OFF` (and `WEB` alone shows the state, the URL and the request counts),
     stored in flash with the other settings (same store as WIFI/TCP/BAUD), kept across restarts
     and flashes. `WIFI CLEAR` sets it back to the default. In a build without the web code the
     command answers that it is not built in.

## What could break it, and how each is checked

| Risk | Check |
|---|---|
| Internal RAM runs out (a socket needs buffers) | INFO's "least ever" free internal RAM before and after, with the page open and polling for 10 minutes; stop if it drops below a limit agreed in step 1 |
| WiFi task stack overflow | FreeRTOS high-water mark of the WiFi task printed in INFO; must keep a margin of at least 1 KB |
| Slower gateway traffic | traffic test (two boards over WiFi, and line at 307200) with and without the page polling; same numbers expected, since a poll is about 2 KB once a second |
| A slow or stuck browser holds the WiFi task | request time limit 2 s; then the connection is dropped |
| Anyone on the network can run commands | accepted by Ronny; the not-allowed list (item 5) still keeps the WiFi password off the page |
| B1 (USB vs WiFi send) returning | the B1 test from BUGS.md with the page polling |

## Steps

1. Measure: lwIP buffer settings from the build's sdkconfig, internal RAM with one extra TCP
   socket open, the WiFi task's stack high-water mark. Write the numbers here, set the limit.
2. Status only: `/` and `/api/status`, HDLCGW. Flash one board, run the checks above.
3. Commands: the web sink in `console_out`, `/api/command`, the not-allowed list, the log ring.
   Checks again.
4. XMSGGW: move the shared part to `src/common/`, XMSGGW's own numbers.
5. README section (build switch, `WEB` command), HELP line, BUGS.md entries for anything found.
   Build once with `WITH_WEB=0` to prove it still builds and is the old size.
6. HdlcGatewayTest: nothing needed; optional link to the board's page in the Version tab.

## Decided (Ronny, 2026-11-23)

- Anyone on the network may run commands from the page; no password.
- HDLCGW first; XMSGGW afterwards with the shared code.
- In a build that has it, the page is ON until someone types `WEB OFF` (stored).
- The page refreshes every second, and shows the board's clock from NTP.
