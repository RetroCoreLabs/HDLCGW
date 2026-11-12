# HDLCGW defects

Every defect that is known and not fixed at its root is listed here by name, with what was
measured. A defect leaves this file only when the cause is fixed and a bench run shows it gone.

No open defects (2026-11-09 12:40). One known capacity limit, L1 below, kept by Ronny's
decision: this phase of the design stops at 307200 bit/s (983000 needs the BUSY signal of the
PCB 3023 MegaLink, not in this phase), and a dropped COMPLETE frame is resent by the host.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [The TCP findings of 2026-11-09 (first sustained TCP run 10:42, last fix 12:35)](#the-tcp-findings-of-2026-11-09-first-sustained-tcp-run-1042-last-fix-1235)
  - [L1 (was T1) - whole line frames dropped on the way to the TCP peer when the CPU runs out](#l1-was-t1---whole-line-frames-dropped-on-the-way-to-the-tcp-peer-when-the-cpu-runs-out)
  - [T2 - the TCP connection stops, and the ESP keeps a dead peer (CLOSED 12:35)](#t2---the-tcp-connection-stops-and-the-esp-keeps-a-dead-peer-closed-1235)
  - [T3 - bad frames off the line during TCP traffic (CLOSED 12:35)](#t3---bad-frames-off-the-line-during-tcp-traffic-closed-1235)

<!-- /contents -->

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
