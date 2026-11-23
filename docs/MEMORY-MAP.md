# Memory map: what sits in internal RAM, what sits in PSRAM

Measured 2026-11-23 on HDLCGW (commit 8d27152 plus the INFO line below), LOLIN S2 Mini,
ESP32-S2 with 2 MB PSRAM, Arduino-ESP32 core 3.3.12. Why it matters: BUGS.md B2.

## The three kinds of RAM

| Kind | Size | Who can use it |
|---|---|---|
| Internal RAM, DMA-capable | the part of the chip's own RAM the transfer hardware can reach | **the WiFi radio**, the SPI line DMA, USB; also everything else |
| Internal RAM, not DMA-capable | the rest of the internal heap | CPU only |
| PSRAM | 2 MB on a separate chip | CPU only; slower; never for DMA |

**The WiFi driver needs DMA-capable internal RAM for every packet it sends** (roughly 1.6 KB
each). When that runs out, sending stops and receiving goes on: BUGS.md B2.

`INFO` prints `Internal RAM by kind: DMA <free> free (least <lowest since start>), largest
block ...`. **This DMA figure is the one to watch, not the internal total.**

## Measured (PC as TCP peer, WIRELESS ON, 256-byte frames, 120 s)

| Build | Internal free (total) | DMA free at start | DMA lowest under traffic | Result |
|---|---|---|---|---|
| 8d27152 | 11,608 | 3,848 | **104** | pass, 17259/17259 |
| same + 1281 static bytes | 10,332 | 2,572 | **136** | FAIL, 5 of 241, board deaf |

So: the working firmware already runs DMA memory down to about 100 bytes. The total internal
free (about 10-11 KB) hides that; most of it is memory the radio cannot use.

## Who uses internal RAM

### Static (fixed at build time), from `xtensa-esp-elf-nm` on the built firmware

Ours, biggest first (bytes):

| Name | Size | What | Must be internal? |
|---|---|---|---|
| `g_desc` | 6,144 | line receive DMA descriptors (spi_rx_task) | yes: DMA |
| `g_dma_buf` | 2,048 | line DMA buffer | yes: DMA |
| `g_peek_fifo` | 2,048 | line receive peek FIFO | no |
| `g_stuffed` | 1,038 | line transmit bit-stuffed frame | check (SPI TX DMA?) |
| `g_input` | 1,025 | console input line | no |
| `g_frame`, `g_chunk` x2, `g_peeked` | about 2,000 | frame and chunk buffers | check each |
| `CRC_TABLE` | 512 | FCS table (data, read only) | no, could be `const` in flash |

From the Arduino core, for USB features this firmware never uses (we only use the USB serial
port): `ncm_epbuf` 6,416 (USB network adapter), `_mscd_epbuf` 4,096 (USB memory stick),
`_transfer_buf` 4,096 and `_usbh_devices` / `_hcd_data` 1,381 (USB host), `_hidh_epbuf` 2,688
(USB keyboard host), `ep_in_sw_buf` / `ep_out_sw_buf` 3,104 each. About 23 KB. They are in the
core's precompiled libraries; removing them needs a rebuilt core (not tried).

### Task stacks (taken from the internal heap at start)

| Task | Stack given | Never used (TASK command) | Used at most |
|---|---|---|---|
| loopTask (Arduino; `loop()` only calls `board_tick` once a second) | 8,192 | 6,424 | ~1,800 |
| WiFi (ours) | 8,192 | 6,264 | ~1,900 |
| Console | 4,096 | 1,776 | ~2,300 |
| SPI rx / SPI tx / USB tx | 2,048 each | 1,652 / 1,244 / 1,392 | 400-800 |
| WiFi LED | 2,048 | 1,504 | ~550 |

About 10 KB of stack is never touched.

### Run-time allocations (ours)

| Buffer | Size | Where |
|---|---|---|
| TCP send ring | 16,384 | PSRAM (internal if no PSRAM) |
| bad-frame history, last frame, decoders, encode buffers, HELLO reply | up to 8 x 4,112 | PSRAM |
| board settings state | small | PSRAM |
| WiFi `g_in_buf` 1,024, `g_out_buf` 1,460 | 2,484 | **internal**, on purpose (socket reads/writes) - check whether needed |
| `TASK` command list | small, freed | internal |

## Rules

1. **A new buffer goes in PSRAM** (`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`), unless hardware
   DMA reads or writes it. No new `static` array over 64 bytes in internal RAM without an entry
   in this file.
2. **Every change is checked against the DMA figure**: run the WIRELESS test from BUGS.md B2
   and read `INFO`; the DMA lowest must stay above the margin below.
3. Task stacks are sized from the `TASK` high-water numbers plus a margin, not guessed.

## Done 2026-11-23
Ronny chose: margin 4 KB; smaller stacks and our buffers to PSRAM; a rebuilt core without the unused USB buffers maybe later.

- loopTask stack 8192 -> 3072 (`SET_LOOP_TASK_STACK_SIZE`, HDLCGW.ino); WiFi task 8192 -> 4096.
- To PSRAM: console input line (1025), console output chunk, frame and stuffed buffers (about
  2,070), WiFi socket in/out buffers (2,484). Each falls back to internal RAM on a board
  without PSRAM.
- Margin `BOARD_DMA_MARGIN` = 4096 (board.h). INFO prints "DMA margin 4096: kept since start"
  or "BELOW", STAT prints the DMA line, and the firmware logs one WARN when the lowest falls
  under it.

| Same WIRELESS test | DMA free at start | DMA lowest under traffic | Result |
|---|---|---|---|
| before (8d27152) | 3,848 | 104 | pass, no margin |
| after | 19,300 | 14,724 | pass, 19467/19467, pings 100 % |

Stacks after the change (TASK, never used): WiFi 2,116 of 4,096, loopTask 1,248 of 3,072.
So the stacks did come out of the DMA-capable part (the open question above is answered).

## XMSGGW, done 2026-11-23

- loopTask stack 8192 -> 3072, Node task 12288 -> 6144 (Telnet stays 6144: used about 2.7 KB).
- To PSRAM: console output chunk, frame and stuffed buffers (about 2,070), and the Node task's
  line receive, transmit and last-frame buffers (about 2,000). The Node task refuses to run,
  with a log line, if it gets no memory for them at all.
- Same DMA margin guard (INFO line and a one-time log WARN).

| XMSGGW | DMA free at start | DMA lowest | Node stack never used |
|---|---|---|---|
| before | 8,456 | 7,572 at rest | 10,212 of 12,288 |
| after | 24,432 | 20,928 after 31 FTP connection tries (20 refused logins, the rest turned away as a second session) and 10 telnet connects | 4,232 of 6,144 |

Under real use, 2026-11-23 (COM10 board, line through the Pico and the PC window to D100 at
307200 bit/s): a telnet login, CONNECT D100 with a SINTRAN login and commands, FTP listings
and a 46,516-byte download (13 s):

| XMSGGW, after that | Never used of 6,144 | DMA lowest |
|---|---|---|
| Node task | 2,308 (worst use about 3.8 KB, during the FTP download) | 19,500 |
| Telnet task | 2,924 | |

Line: 9,147 frames sent, 5,765 received, 0 bad on either end. Node stays at 6,144: 2.3 KB
left is the margin for a longer transfer or several sessions at once (not measured).

## Still open

- The line buffers in the shared HDLC library (`g_peek_fifo`, `g_stuffed`, `g_peeked`,
  `g_chunk`) belong to the libxmsg session.
- A rebuilt Arduino core without the unused USB buffers (about 23 KB): later, Ronny's call.
