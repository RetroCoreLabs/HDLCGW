# HDLCGW defects

Every defect that is known and not fixed at its root is listed here by name, with what was
measured. A defect leaves this file only when the cause is fixed and a bench run shows it gone.

One open defect: [B1](#b1---a-second-s2-mini-com20-drops-off-usb-under-traffic) (2026-11-14). One known capacity limit, L1 below, kept by Ronny's
decision: this phase of the design stops at 307200 bit/s (983000 needs the BUSY signal of the
PCB 3023 MegaLink, not in this phase), and a dropped COMPLETE frame is resent by the host.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [B1 - a second S2 Mini (COM20) drops off USB under traffic](#b1---a-second-s2-mini-com20-drops-off-usb-under-traffic)
- [The TCP findings of 2026-11-09 (first sustained TCP run 10:42, last fix 12:35)](#the-tcp-findings-of-2026-11-09-first-sustained-tcp-run-1042-last-fix-1235)
  - [L1 (was T1) - whole line frames dropped on the way to the TCP peer when the CPU runs out](#l1-was-t1---whole-line-frames-dropped-on-the-way-to-the-tcp-peer-when-the-cpu-runs-out)
  - [T2 - the TCP connection stops, and the ESP keeps a dead peer (CLOSED 12:35)](#t2---the-tcp-connection-stops-and-the-esp-keeps-a-dead-peer-closed-1235)
  - [T3 - bad frames off the line during TCP traffic (CLOSED 12:35)](#t3---bad-frames-off-the-line-during-tcp-traffic-closed-1235)

<!-- /contents -->

## B2 - two boards over WiFi: the TCP server stops answering on the network

Open, 2026-11-23. Setup: two S2 Minis on HDLCGW (0357688 + 479bb00), both WIRELESS ON, one
`TCP SERVER 1362`, the other `TCP CLIENT` to it; PC test tool
`hdlc-loopback --port COM10 --port-b COM24 --no-baud --size <n> --seconds 90 --rates 307200`.

What was measured:

- Every run: after 20 to 77 s of traffic the link drops ("socket took nothing for 10 s", or
  error 113), and only a few frames ever arrive (0 to 485 of 177 to 1165 sent, 64-4000 bytes).
  USB never failed; neither board restarted.
- Afterwards the SERVER board answers only 40-50 % of pings (idle, no traffic) and the client
  cannot reconnect; the client answers 100 %. COM24 server: `TCP flow: socket full 9876 times`.
- It follows the server role: COM24 (rev 1.0) as server, then COM10 (rev 0.0) as server, same.
- Not caused by 802.11b: a test build with normal b/g/n WiFi on the server (COM10) failed the
  same way, twice.
- `WIFI CONNECT` (or RESET) on the stuck board brings pings back at once and frees about 12 KB
  of PSRAM (1,982,792 -> 1,995,096 free). Same picture as the "wider than one socket" stall in
  the TCP findings of 2026-11-09 below, which `WIFI_RX_COPY` fixed for the PC-as-peer case.

Later the same day, with the PC test tool on COM10 only and the gateway window echoing every
frame COM24 gives it back into COM24 (full two-way traffic, COM24's USB free for `TCP DEBUG`):

- This time the CLIENT (COM10, rev 0.0) went deaf (pings 60-70 % lost), the server COM24 kept
  answering 100 %. So it is not tied to the server role either; either board can be hit.
- COM24 during the stall: 900 bytes sent and not acknowledged, retransmitting (nrtx 1), nothing
  received for 30 s; no out-of-order data, receive window open.
- COM10 after the run, still deaf: its WiFi RECEIVE works (8944 frames copied, 0 dropped), but
  its new connection is stuck in SYN_SENT with the SYN still in the unsent queue, and its pings
  are half answered. lwIP holds no backlog, memory is fine. So on the deaf board the SEND path
  under lwIP (WiFi driver transmit) is what stalls. WIFI CONNECT clears it.
- Core 3.3.12 sdkconfig: static WiFi TX buffers 8, TX cache 16, A-MPDU TX on, BA window 6,
  `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`. TCP_NODELAY and WiFi sleep off are already set.
- Web search (2026-11-23): esp32.com has several "IP stops answering until esp_wifi_stop/start"
  threads with no confirmed cause; nothing on board-to-board TCP specifically.

Not known: why the 2026-11-09 fix does not cover a board-to-board link; whether the client's
sending pattern (many small segments both ways) refills lwIP's out-of-order queue. Next: TCP
DEBUG counters (out-of-order queue length) on the server during a run.

## B1 - a second S2 Mini (COM20) drops off USB under traffic

Fixed 2026-11-23: both firmwares join WiFi with 802.11b only (`board_wifi_protocol`, commit below). Cause as far as measured: on v1.0 chips the radio sending at the fast g/n rates breaks USB from the PC; see the 2026-11-23 section at the end.

Setup: two LOLIN S2 Minis with the same build (HDLCGW 0.3.5, built 18:07:00 that day), both on
WiFi, joined over TCP with WIRELESS ON (the TCP peer replaces the cable), PC test tool
`hdlc-loopback --port A --port-b B --no-baud --size 256`. Board A is the one on the bench since
November (COM10). Board B is a bare S2 Mini first used that day (COM20, nothing on its pins).

What was measured:

- Every run, within the first seconds of traffic, Windows reports COM20 "A device attached to
  the system is not functioning". The port then stays listed but every open hangs ("semaphore
  timeout") until RST. COM10 kept running in every run, uptime unbroken.
- It stayed with board B whichever board listened and whichever dialled, and whichever port
  the tool opened first.
- It also happened with WIRELESS OFF and no WiFi traffic (LOOPBACK ON, frames to the line).
- After two of the failures INFO on board B said "Last start: other watchdog (code 7)".
  esptool's own reset also uses a watchdog, so that line alone does not prove a hang.
- Board B, right after a restart: 0 line bytes received (its receive pins are not picking up
  noise), free internal memory 14,804 bytes; board A, which does not fail, 14,364.
- Once board B was found in download mode (USB 303A:0002, COM9) after a failure.
- Board B had already restarted once by itself earlier that day, at the end of a WIFI SETUP.
- Moved to another cable and PC port, board B did not show on USB at all; not followed up.

Later the same day (board B now COM22, on a PC port directly and a new cable; hub and cable
were changed together, so which of the two mattered is not known):

- In download mode, flash reads from board B are now clean (1.1 s for 64 KB); through the hub
  they failed. The crash dump area in flash is blank: no crash was recorded.
- WIRELESS traffic now runs (0 bad, 0 lost, about 240 and 137 kbit/s) until board B restarts,
  7 to 10 s in. INFO afterwards: "Last start: other watchdog (code 7)" and, from the line INFO
  now prints, "Chip reset cause: 16, RTC watchdog, whole chip".
- Test build with the chip's low-voltage (brownout) detector switched off: no restart any more.
  After 1 to 18 s the PC still gets "device not functioning" on writes, but the board keeps
  running: a status line it printed every 2 s over USB showed the console task running, the
  USB stack mounted and connected, nothing waiting in its receive buffer, and the text reached
  the PC. Only the PC-to-board direction had failed, on the PC side.
- Test build with the detector on and the WiFi send power cut to 8.5 dBm: two 60 s runs with no
  restart and no USB error. But the radio link of board B became poor (33 % ping loss, TCP
  stalls), so this is a test, not a fix.
- The esp-idf "hint" of what started a restart cannot be read by the firmware: it is 0 even
  right after a RESET command, so the system start clears it first.

2026-11-15, measured on the boards themselves: a test build kept the brownout detector on but
stopped it from resetting or interrupting, and counted two things every millisecond, the
detector's latched flag and its live output. Two 60 s runs, board B failed on USB within the
first seconds both times; counts on both boards: 0 and 0. So the supply did NOT drop below the
detector's level (level 7) in a way a 1 ms sample sees. The supply-dip explanation written here
on 2026-11-14 is withdrawn.

What is left, all measured: board B's USB fails only with WiFi traffic at full send power (not
at 8.5 dBm), the board itself keeps running, and only the PC-to-board direction fails, at the
PC (Windows error 31, "device not functioning"). One explanation that fits, NOT measured: the
WiFi transmitter on this board disturbs its USB signal, so the PC sees bad USB packets. How the
restart (RTC watchdog, cause 16) with the normal firmware follows from that is not known.

2026-11-15, later: Ronny replaced board B with a brand-new S2 Mini (also bought from
AliExpress, chip revision v1.0 = "rev 100", the same as board B). It fails the same way on the
same test.

- The two revisions do run one thing differently: Espressif's errata (USB-105) has ESP-IDF set
  the USB unit to "INCR" bus bursts on v0.0 only. Read from the boards: GAHBCFG 0x00000001
  (single transfers, no DMA), GUSBCFG 0x40001447 on both; TinyUSB in this core does not use
  USB DMA. So that difference does not reach this firmware; ruled out.
- Reduced to the smallest case, no TCP, no line, no forwarding (WIRELESS OFF, TCP OFF): the PC
  writes frames to USB while 20 pings of 1400 bytes run against the board's WiFi address.
  Good board (rev 0): 800 of 800 pings answered, 40 s, no USB error. New board (rev 100):
  "device not functioning" within seconds. The same USB writes with no pings: no error.

Cause, as far as the bench can go: on these v1.0 boards, the board's own WiFi transmitter
breaks USB packets from the PC. It is the board, not the firmware: the same build, the same
test and the same load pass on the other board. Not measured: whether it is the board layout
or the chip; a genuine LOLIN S2 Mini with a v1.0 chip would settle it. Lowering the WiFi send
power hides it but is not a fix (rule: no workarounds). Open until a board that does not do
this is tested.

Does not fit yet: the failure seen with WIRELESS OFF (above) was before the change of hub and
cable, and has not been repeated on the direct port.

2026-11-23, the cause narrowed, and the fix. Read from the chips (new INFO lines): the bench board
(works) is chip revision 0.0, the 15 November board (fails, now COM24) revision 1.0; package,
eFuse block, built-in flash and PSRAM, flash chip (JEDEC 204016) and all USB and flash-supply
eFuses are the same. Espressif's change notice PCN20230702 (v0.0 to v1.0) says v1.0 sends with
more power at the fast 802.11g/n data rates; 802.11b and the low rates are unchanged. No
Espressif erratum or public report of USB failing during WiFi send was found.

Test on COM24, full send power, PC writing frames to USB (single port, 307200, 256-byte
frames; with nothing on the line the window fills, so the USB load is light after the first
seconds and the same in every run) while 20 pings of 1400 bytes run against the board for the
whole 60 s:

| Build | Runs | USB from the PC | Pings answered |
|---|---|---|---|
| normal (802.11b/g/n) | 2 | failed in both, board dropped off USB until replugged | 405 of 800, 374 of 1500 |
| 802.11b only | 5 (4 test switch, 1 final build) | no error in any | 799/800, 1491, 1493, 1492, 1463, 1499 of 1500 |

Fix chosen by Ronny: 802.11b on every board, both firmwares, revision 0.0 too (one behaviour
everywhere). 802.11b gives up to 11 Mbit/s on the air, about 5 Mbit/s usable; the gateway
needs at most half a megabit. The router must accept 802.11b clients (this one does). Not
measured: whether a real LOLIN board with a v1.0 chip also needed it, and the old failure with
WIRELESS OFF before the hub change.

## The TCP findings of 2026-11-09 (first sustained TCP run 10:42, last fix 12:35)

Setup: ESP build 03:48:38, TCP SERVER 1362, MIRROR OFF, PC test tool as TCP client
(pane A), RC422Dev Pico build 10:32:34 on USB (pane B), 983000 bit/s, 60 s per case. Log
`tcprun-20261109-104217.log` in the HdlcGatewayTest session. Every USB-side counter stayed 0
(USB TX dropped, line RX ring overflow, FIFO overruns, DMA errors, field 39).

### L1 (was T1) - whole line frames dropped on the way to the TCP peer when the CPU runs out

**Known limit, not a defect (Ronny, 2026-11-09 12:40):** only whole frames are dropped, and
the host resends them. Measured at 307200, the design rate: 626-byte frames 0 dropped in
every direction; 10-byte frames, about 7000 a second both ways, 528 of 132901 dropped in 60 s
(0.4 %). The cause is CPU: idle 3-4 % in those cases.

**Current state, build with T2 and T3 fixed:** every remaining lost frame is a TCP output
drop (plus one software line RX ring overflow in the heaviest case), to the frame. With
10-byte frames: 16888 dropped in two-way at 983000, 528 in two-way at 307200, 444 and 0 in
two runs of one-way line -> TCP at 983000. 626-byte frames: 3 dropped in two-way at 983000,
0 one-way. CPU idle 3-4 % in the 10-byte cases (WiFi driver 29 %, our WiFi task 17-25 %,
console 23-31 %), so the WiFi task cannot always empty the 4 KB TCP ring. Internal heap
lowest 7680 bytes.

Earlier findings:

The TCP output ring (16 KB, PSRAM) fills and whole frames are dropped (`STAT` "TCP: ... N
dropped"): 11 of 918 in 626-byte two-way (only 39 kbit/s of line input), 55 of 7733 in
626-byte line-to-TCP only (635 kbit/s). Board total after the run: 893.

What the code shows, not yet measured: the WiFi task writes at most one 1460-byte segment per
10 ms pass (`run_peer_io`, `LOOP_MS`), and in two-way traffic the same task first feeds every
frame from the peer to the line through `spi_tx_task_push()`, which sleeps 1 ms per try while
the line TX ring is full, so TCP output waits behind line input. How long `g_peer.write()`
blocks has not been measured.

### T2 - the TCP connection stops, and the ESP keeps a dead peer (CLOSED 12:35)

Fixed by copying every received WiFi frame out of the driver's buffer (`wifi_rx_copy()`,
`wifi_task.cpp`), with keepalive and a 10 s stall rule for dead peers. The story follows.

10-byte frames both ways: about 12 s in, the ESP stopped acknowledging the PC; 20 s later the
PC's socket failed with Windows' "connected host has failed to respond". 2745 frames sat
unacknowledged in the PC's send buffer. The ESP still reported `peer=192.168.1.180` until the
PC closed its side at 10:47:09 (then `peer=none`); there is no keepalive and no timeout. During
the stall the TCP output ring filled (dropped frames rose from 66 to 893). WiFi stayed up
(state CONNECTED, rssi -42 afterwards). Internal heap lowest since boot fell to 15720 bytes
(USB-only runs: 21956).

Also seen at 307200 (10:55-10:56): after a clean close by the PC at 10:55:55, `WIFI STATUS`
at 10:56:02 still showed `peer=192.168.1.180`. Not yet known whether the ESP had not
noticed the close yet or never would.

**Wider than one socket (10:56-11:00):** after a test whose far end went away (the Pico was
reflashed) while the PC kept sending frames to the ESP for 4 minutes, the PC closed its socket
at 10:55:55. From then on nothing on IP answered: ping to 192.168.1.136 failed, a TCP connect
to 1362 timed out, while the PC reached its own gateway fine. Over USB at the same time:
`state=CONNECTED rssi=-42`, `listening=yes peer=192.168.1.180` (the closed peer). Two TASK
reads a few seconds apart: the WiFi task (ours) used 4.9 ms, so its loop was running and not
stuck; lwIP's task (tiT) used 0.1 ms and the WiFi driver task 1.9 ms, so almost nothing reached
the IP stack. `frames from peer` did not move. Internal heap 20936 free. So the station stopped
passing received packets to lwIP, or lwIP stopped taking them, and nothing in the firmware
notices. Which one it is has not been measured. Configuration facts that bear on it (core
3.3.12 sdkconfig): WiFi static RX buffers 8, dynamic 32; lwIP TCP window 5760, receive mailbox
6; out-of-order queueing on with no limit on pbufs (`CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=0`);
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`. The board was then reflashed (state lost).

**Again on the fix build 10:54:35 (11:04), 307200 bit/s:** the three 626-byte cases passed
(0 lost, ping answered after each). The 10-byte two-way case killed the IP stack within
seconds of its start: of 3624 frames the PC sent only 1018 reached the ESP; ping and TCP
connect dead from then on. The new stall rule dropped the peer after 10 s (`peer=none`,
1 stalled peer), so the dead stack is not our socket. During the case PSRAM free fell 104 KB
(2071536 -> 1967608) while internal heap stayed about 17 KB (lowest 12248); CPU idle 70 %, lwIP
task 0.3 %. `WIFI CONNECT` (station reconnect) brought ping back at once and PSRAM free returned
to 2071872: whatever the WiFi driver or lwIP was holding in PSRAM was released by the station
restart. Not yet known: which buffers those are, and why they are never released otherwise.

**Located, 11:10-11:29 (builds with drop reasons, TCP DEBUG and lwIP's own pcb state):**
10-byte frames one way PC -> ESP -> line at 307200 fail every time within about 1-2 s of
traffic. With keepalive on, lwIP aborted the connection after 14 s (ECONNABORTED, 113). With
keepalive off, nothing is aborted: lwIP's rcv_nxt freezes while its receive window stays open
(rcv_wnd 5760, announced 4357..5561), nothing refused, FIONREAD 0, and the out-of-order queue
holds later data (585 bytes in two separate runs). The missing segment never arrives, not even
as the PC's retransmission; the PC gives up after its retransmission timeout and the ESP keeps
the dead peer. Our task is not the cause: it waits for nothing (no frame waiting, buffer
drained) and simply gets no data. Once rcv_nxt jumped by 72 KB by itself and froze again.
Joining the network with 802.11b/g only (no 11n, no A-MPDU reordering) gave the same failure,
so 11n reordering is ruled out. Next: a PC-side packet capture to see whether the PC's
retransmissions are sent and whether the ESP answers them.

**PC-side capture, 11:30 (PC on wired Ethernet, the router carries the WiFi hop):** the PC
retransmitted the missing segment 8 times (20, 665, then 1436 bytes, normal backoff). The ESP
answered none of the lone retransmissions at 5.1, 6.7, 10.0, 32.1, 33.3 and 35.7 s: they never
reached its TCP. The ESP's many duplicate ACKs were answers to other, later segments. One
retransmission at 29.6 s got through and the stream caught up 72 KB.

**Cause, confirmed 12:07 (TCP DEBUG counting queued segments):** at both freezes lwIP's
out-of-order queue held exactly 32 segments in 32 pbufs (584-587 bytes), for about 20 s each,
until lwIP purged it; then one retransmission got in and the queue filled again within a
second. Fix under test (build flashed 12:10): `wifi_rx_copy()` in `wifi_task.cpp` copies every
received frame out of the driver's buffer and returns the buffer at once.

**Fix result, 12:10-12:14, 307200:** 10-byte one-way PC -> line: 91668 of 91668, 0 lost,
connection never stalled, receive copy 0 drops for memory, out-of-order queue empty or 2
segments, lwIP now holds the PC back with its window instead. 10-byte two-way (the case that
killed the IP stack): the connection survived, PC -> line 111909 of 111909. The stall is
gone; T2 stays open until the 983000 cases pass too. Left over in that case: T1 (50 TCP output
drops) and T3 (424 bad and about 6800 missed frames off the line), and internal heap down to
7968 bytes at its lowest (copied packets live in lwIP's heap while queued).

**Fix result at 983000, 12:14-12:25 (eight cases in all with the 307200 ones):** no freeze,
no stall, ping after every case, the out-of-order queue never above 2 segments, receive copy 0
drops for memory, a closed peer seen at once. 626-byte frames clean in every direction over
TCP (two-way 872.6 + 301.0 kbit/s). **T2 is fixed**; it moves to Closed after the write-up.
What is left belongs to T1 and T3 (10-byte frames from the line while TCP runs) and to low
internal heap (lowest 7532 bytes).

**The explanation as first written:** with Nagle off on the PC every
10-byte frame is its own ~19-byte segment. After one loss lwIP queues the following segments
out of order, and in this core build each queued segment keeps the WiFi driver's own receive
buffer (`CONFIG_LWIP_L2_TO_L3_COPY` is off; out-of-order queueing is unlimited). The core gives
the driver 32 dynamic receive buffers (`WiFiGeneric.cpp`, core 3.3.12). About 31 segments of
19 bytes (585 bytes, seen twice) use them up, so the retransmission has no buffer to arrive in
until lwIP times the queue out. This also fits the 104 KB of PSRAM held while the stack was
dead (about 64 buffers of 1.6 KB) and that 626-byte frames never fail. Test: `TCP DEBUG` now
counts the queued segments; a freeze at about 31 would confirm it.

**Consequence seen in the USB sweep 11:34-11:36:** the ESP missed the PC's clean close (FIN) of
that connection at 11:32:15 as well, kept the peer, and sent the line's frames to it instead
of USB for about 90 s (TCP to peer +247, dropped +143); USB got nothing from the line in that
time. A dead peer the ESP does not notice makes the gateway deaf towards USB, silently.

Cause unknown. What is known from the code: the WiFi task at priority 1 is the only reader of
the socket and blocks in `spi_tx_task_push()` (1 ms sleep per try) while the line TX ring is
full; with 10-byte frames that is at most about 1000 frames a second against 2200 a second
offered. Whether lwIP or the WiFi driver then ran out of receive buffers has not been measured.

### T3 - bad frames off the line during TCP traffic (CLOSED 12:35)

Fixed by running the line receive task at priority 24, above the WiFi driver and lwIP
(`HDLCGW.ino`). The story follows.

10-byte frames both ways over TCP: 22 frames off the line with a bad FCS and 2 short
flag-to-flag runs (32601 sent by the Pico, 32470 received by the ESP). The same case over USB
earlier the same day: 0 bad. Line RX ring overflow 0, FIFO overruns 0, DMA errors 0, so the
loss is not in the counted places. Cause unknown; not yet known whether the Pico sent them bad.

**Cause found 12:25 (STAT "Line RX passes", build 12:16:07):** 10-byte two-way at 983000 over
TCP: the line receive task (priority 9) went unserviced 95 times for over 20 ms, longest
97.6 ms, and one pass found 675 descriptors' worth of words in a 512-descriptor ring: the
receive DMA ring (2048 bytes, 16.7 ms at 983000) wrapped over unread words. That gives the
unseen frames (11610), the bad ones (662, torn at the wrap) and the short runs (112). The
software ring did not overflow, which is why no counter moved. Above priority 9 run the WiFi
driver (23) and lwIP (18), which took 35 % of the CPU in bursts. The same stream one way
only, fresh after a reset: 0 gaps over 20 ms, 0 bad. Fix under test: line receive task at
priority 24.

**Fixed, 12:28-12:35 (seven cases, no reset, priority 24):** longest gap between receive
passes 3.3-3.8 ms, 0 gaps over 20 ms, at most 36 of 512 descriptors per pass (was 97.6 ms,
95 gaps, 675). Bad frames 10 in the heaviest case and 0 in the other six, 0 short runs, 0
unseen except 780 in that heaviest case, where the SOFTWARE line RX ring overflowed (10839
bytes): there the console task, not the DMA, fell behind (CPU idle 3 %). That remainder is
the same CPU limit as T1. **T3 as a DMA-ring defect is fixed.**

Also seen, NOT an ESP defect: the Pico drops frames to its USB at about 550 kbit/s of 626-byte
frames (its RECEIVED DROPPED 3349 and 3054, queue 33 of 34); the ESP sent every frame.

Closed the same night (before the TCP run, the board had no open defects at 04:41):

The three listed earlier the same night were fixed by reading USB input straight from TinyUSB
(`usb_rx.cpp`, see CLAUDE.md) and shown gone by four 60 s runs at 983000 bit/s on build
02:55:24, all frames delivered, see LOOPBACK-TEST.md section 8e:

- B1, the Arduino core dropping USB input bytes so frames went onto the line short: the core's
  receive queue is no longer used, TinyUSB paces the PC with NAK. Field 39 stayed 0 and the
  Pico counted 0 bad frames.
- B2, the core's per-byte USB receive costing 36 to 43 percent of the CPU: now 4 to 12 percent.
- B3, ESP-to-PC USB sending topping out near 830 kbit/s of stuffed bytes: with the CPU free the
  USB write carried 1.19 Mbit/s and no frame was dropped, so the ceiling was CPU, not the
  64-byte CDC FIFO.
- B4, 10-byte frames both ways at 983000 dropped whole frames at the USB TX ring: the cause was
  CPU, not ring size or the PC. The line TX task used 64-byte DMA chunks, about 1900 driver
  transactions a second at 983000 and 12.6 percent of the single core. Chunks now cover about
  2 ms of line time (spi_tx_task.cpp), the task takes about 5 percent, and both 10-byte cases
  ran 60 s with 0 lost (343262 and 412608 frames). A larger USB TX ring and a higher USB TX
  priority were both tried and measured first: neither fixed it, both were taken back.
  Final sweep on that build: 376 PASS, 0 FAIL, all rates, sizes 10 to 626.
