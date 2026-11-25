# Dual-target LOLIN S2 Mini / S3 Mini ND communication gateway

**Origin of this document.** Sections 1 to 10 are an implementation brief written by ChatGPT and
given by Ronny on 2026-11-25. They are kept as supplied (tables converted to Markdown). Statements
in them are NOT verified unless the section "Validation against the current design" at the end
says so. That last section was written by checking the brief against the KiCad schematic
`F:\KiCad\ND\HDLCGateway\HDLCGateway-2026\HDLCGateway.kicad_sch`, its exported netlist, the WEMOS
S3 mini schematic V1.0.0 and the firmware sources, on 2026-11-25.

**Purpose:** Implementation brief for the LLM maintaining the PCB and firmware. Preserve
compatibility with the existing ND interface while supporting both LOLIN modules. S3 Mini is the
preferred new target; S2 Mini remains supported. USB mass storage is explicitly out of scope.

## 1. Mandatory design constraints

- Maintain one PCB layout and the existing six ND signal connections. Do not relocate signals
  merely to obtain identical GPIO numbers across targets.
- Support the standard LOLIN S2 Mini and S3 Mini, not the S3 Mini Pro or generic ESP32
  development boards.
- Use separate synchronous transmit and receive interfaces with independent clocks; both may
  operate concurrently with Wi-Fi.
- ND receive consists of DATA and CLOCK, with no dedicated SPI chip-select. Do not assume a
  conventional SPI transaction protocol on the ND wire.
- Existing ND interface uses RS-422 and approximately 987 kbit/s synchronous HDLC. Treat the exact
  transmit clock, signal polarity and timing as parameters to verify against existing firmware and
  hardware documentation.
- Preserve the existing protocol implementation, busy semantics, wire pinout, bit order and
  electrical interface unless a specific change is approved.
- Do not invent missing ND cable pin assignments or protocol details. Inspect the existing
  repository, schematics and available ND manuals first.

## 2. Authoritative pin mapping

The following positions were checked against the manufacturer's V1.0.0 schematic drawings for
both modules. Direction is relative to the ESP32.

| PCB net | S2 Mini GPIO | S3 Mini GPIO | Physical header | Direction | Peripheral |
|---|---:|---:|---|---|---|
| TX_CLOCK | 7 | 12 | J2.4 | Output | SPI2 SCLK |
| TX_DATA | 11 | 11 | J2.6 | Output | SPI2 MOSI |
| RX_CLOCK | 12 | 10 | J2.7 | Input | SPI3 SCLK |
| RX_DATA | 9 | 13 | J2.5 | Input | SPI3 MOSI (slave input) |
| BUSY_TO_ND | 10 | 8 | J5.6 | Output | GPIO |
| BUSY_FROM_ND | 8 | 7 | J5.5 | Input | GPIO |

Do not change the physical PCB connections. GPIO numbers differ, but each signal uses the same
physical pad on both boards. GPIO13 on the S3 is labelled MISO for the default SPI2 mapping; it is
valid as a SPI3 slave MOSI input via the GPIO matrix. Do not configure RX_DATA as MISO.

Complete module header reference:

| Header | Pin | S2 | S3 |
|---|---:|---|---|
| J2 | 1 | EN | EN |
| J2 | 2 | GPIO3 | GPIO2 |
| J2 | 3 | GPIO5 | GPIO4 |
| J2 | 4 | GPIO7 | GPIO12 |
| J2 | 5 | GPIO9 | GPIO13 |
| J2 | 6 | GPIO11 | GPIO11 |
| J2 | 7 | GPIO12 | GPIO10 |
| J2 | 8 | 3V3 | 3V3 |
| J5 | 1 | GPIO1 | GPIO1 |
| J5 | 2 | GPIO2 | GPIO3 |
| J5 | 3 | GPIO4 | GPIO5 |
| J5 | 4 | GPIO6 | GPIO6 |
| J5 | 5 | GPIO8 | GPIO7 |
| J5 | 6 | GPIO10 | GPIO8 |
| J5 | 7 | GPIO13 | GPIO9 |
| J5 | 8 | GPIO14 | GPIO14 |
| J4 | 1 | GPIO40 | GPIO33 |
| J4 | 2 | GPIO38 | GPIO37 |
| J4 | 3 | GPIO36 | GPIO38 |
| J4 | 4 | GPIO34 | GPIO34 |
| J4 | 5 | GPIO21 | GPIO21 |
| J4 | 6 | GPIO17 | GPIO17 |
| J4 | 7 | GND | GND |
| J4 | 8 | GPIO15 | GPIO15 |
| J3 | 1 | GPIO39 | GPIO43 |
| J3 | 2 | GPIO37 | GPIO44 |
| J3 | 3 | GPIO35 | GPIO36 |
| J3 | 4 | GPIO33 | GPIO35 |
| J3 | 5 | GPIO18 | GPIO18 |
| J3 | 6 | GPIO16 | GPIO16 |
| J3 | 7 | GND | GND |
| J3 | 8 | VBUS | VBUS |

Header identifiers refer to manufacturer schematic numbering, not arbitrary footprint pad numbers.
Verify physical orientation against board drawings and a real module before manufacture.

## 3. Reserved and special pins

| Function | S2 | S3 |
|---|---|---|
| Boot button | GPIO0 | GPIO0 |
| Reset button | EN | EN |
| Strapping pins | GPIO0, 45, 46 | GPIO0, 3, 45, 46 |
| Onboard LED | GPIO15 | GPIO47 (addressable RGB) |
| Native USB D-/D+ | GPIO19/20 | GPIO19/20 |
| External JTAG | GPIO39-42 | GPIO39-42 |
| Default UART0 TX/RX | GPIO43/44 | GPIO43/44 |
| Onboard pull-up | GPIO18, 10 kΩ | None shown on GPIO18 |

The six assigned ND GPIOs have no identified conflicts with these functions or internal
flash/PSRAM on the reference boards. They are not 5 V tolerant. Do not infer a safe reset-time
logic level from absence of a boot conflict. Native USB remains available for normal
programming/debugging; do not add USB host circuitry.

## 4. PCB and electrical requirements

- Use one common physical KiCad footprint (nominal module outline 34.3 × 25.4 mm, 2.54 mm header
  pitch); validate row spacing, orientation and pad positions from the actual mechanical
  drawing/module. Use physical connector-based pad IDs and signal names, not GPIO numbers as
  universal pad IDs.
- Provide a common schematic symbol labelled with application nets and both GPIO numbers, or two
  alternate symbols with exactly matching physical pad IDs.
- Retain the existing RS-422 interface. If using MAX3490, each IC provides one driver and one
  receiver; the six differential signals require three drivers and three receivers if all six are
  RS-422. Verify existing cable electrical standards before deciding this count is applicable. No
  invented cable pinout.
- Maintain correct differential polarity; check driver/receiver truth tables, connector
  orientation, cable direction and termination against the existing ND interface. Place
  termination at the receiving end as required by the actual link; do not blindly duplicate
  termination or add bias networks without checking existing hardware.
- Use a common ground reference and appropriate protection for external cables. Decouple every
  transceiver according to its datasheet.
- Provide stable 3.3 V logic supply and verify regulator capacity including Wi-Fi peaks and all
  transceivers. Do not assume the Mini's regulator can supply the entire carrier. Prevent
  backfeeding USB VBUS when the carrier is externally powered; verify the chosen power topology.
- Ensure TX clock, TX data and BUSY_TO_ND have safe reset states using external resistors if
  required. Choose polarity from ND documentation; 10 kΩ is only a starting value. Prevent
  unwanted clocks during reset or boot.
- Maintain antenna clearance and access to USB-C and buttons. Add test points for all six logic
  nets, ground and power if layout permits.

## 5. Firmware target selection

Use one source tree and separate build targets for esp32s2 and esp32s3. Centralize board
definitions; never scatter raw GPIO numbers throughout drivers. Preserve the existing
SDK/framework and pin its version. ESP-IDF-style example (adapt to the actual framework):

```c
#pragma once
#include "sdkconfig.h"
#include "driver/spi_master.h"

#define SPI_TX_HOST SPI2_HOST
#define SPI_RX_HOST SPI3_HOST

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define SPI_TX_CLOCK 12
#define SPI_TX_DATA  11
#define SPI_RX_CLOCK 10
#define SPI_RX_DATA  13
#define BUSY_TO_ND    8
#define BUSY_FROM_ND  7
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#define SPI_TX_CLOCK  7
#define SPI_TX_DATA  11
#define SPI_RX_CLOCK 12
#define SPI_RX_DATA   9
#define BUSY_TO_ND   10
#define BUSY_FROM_ND  8
#else
#error Unsupported ESP32 target
#endif
```

If the project supports other boards using the same chip, also select the board explicitly rather
than assuming the MCU target uniquely identifies its pinout. Do not assume PSRAM is installed; S2
variants differ. Use internal DMA-capable RAM for portable communication buffers.

## 6. SPI transmission and reception

| Parameter | TX | RX |
|---|---|---|
| Controller | SPI2_HOST | SPI3_HOST (candidate backend) |
| Role | Master | Slave / externally clocked receiver |
| Clock source | ESP32 | ND |
| Data signal | MOSI output | MOSI input |
| MISO | Unused | Unused |
| External CS | None | None |
| Bit order | Preserve existing LSB-first wire encoding | Preserve existing LSB-first wire decoding |
| Clock edge | Match existing ND timing | Sample rising edge, verify actual timing |

For master TX, use spi_bus_initialize() and spi_bus_add_device(), set unused bus pins to -1, and
disable physical chip select with spics_io_num = -1. Disable unintended command/address/dummy
phases. Select SPI mode from actual idle polarity and sampling requirements; rising-edge sampling
alone does not uniquely determine the mode. Use the exact number of valid bits including
non-byte-aligned HDLC frames. Validate actual generated frequency, gaps between transactions and
whether gaps are permitted. Use DMA if beneficial; obey buffer alignment, lifetime and
transfer-length requirements of the selected SDK.

### Critical unresolved RX issue

The standard ESP-IDF SPI slave driver is transaction-oriented: external CS assertion/deassertion
defines transactions. A raw continuous DATA/CLOCK stream without CS is not a documented drop-in
replacement for that protocol. Setting spics_io_num = -1, permanently asserting CS or merely
queuing DMA buffers must not be presented as a proven continuous receive solution.

- Inspect and preserve any existing working receive backend first, including its assumptions and
  limitations.
- If investigating low-level SPI3, confirm actual target-specific register behavior from the
  technical reference manual and SDK headers. Do not invent register names or assume S2 and S3
  register layouts are interchangeable.
- Verify exact behavior when clock stops/resumes, when a hardware buffer fills, and when the next
  buffer is armed. There must be no missing or duplicated bits at boundaries.
- If necessary evaluate an externally clocked capture peripheral, or external logic / RP2040 PIO
  as a separate design alternative. Do not add parts or change the PCB without approval. RMT
  captures pulse durations; it is not automatically a synchronous clocked SPI receiver.
- Do not reinterpret BUSY_FROM_ND as chip-select unless the actual ND timing specification proves
  it defines a suitable transaction boundary.
- Treat lossless CS-less receive as an open engineering validation item, not a solved
  requirement. If the existing firmware uses another working receive mechanism, retain it behind a
  portable abstraction.

## 7. HDLC processing

Maintain a separation between raw bit capture, frame decoding/encoding and networking. Preserve
HDLC flags (0x7E), bit stuffing, abort handling and the existing FCS variant, address/control
semantics and wire encoding. Confirm the exact FCS parameters and byte order from existing
implementation; do not replace them by guesswork. Decode across arbitrary receive buffer
boundaries, including flags and stuffed bits spanning two buffers. Track frame errors, FCS
failures, overruns and dropped data. Avoid allocation per bit or byte, logging in hot interrupts,
or blocking on network I/O in capture routines.

At nominal 987 kbit/s there is approximately 1.013 µs per bit, 8.105 µs per byte, 519 µs per
64 bytes, and 4.15 ms per 512 bytes. Size buffers based on measured worst-case servicing delays,
not average network performance. Busy flow control must account for in-flight data and the remote
end's actual response latency. Do not assume busy guarantees immediate stopping.

## 8. FreeRTOS and Wi-Fi architecture

| Task | S2 | S3 proposed |
|---|---|---|
| Wi-Fi driver and network stack | Core 0 | Core 0 (configure lwIP affinity as applicable) |
| RX capture/processing | Core 0 | Core 1 |
| TX processing | Core 0 | Core 1 |
| Network gateway | Core 0 | Core 0 |
| Management/logging | Core 0 | Prefer Core 0, low priority |

S2 has one 240 MHz core; S3 has two 240 MHz cores. S3 also has 512 KB SRAM versus 320 KB on S2,
RMT DMA support, and integrated USB Serial/JTAG. These are architectural advantages, not measured
throughput guarantees. Both have two GP-SPI controllers; S3 does not inherently fix CS-less SPI
reception. Shared RAM/buses and interrupts still preclude guaranteed hard real-time isolation.

Use xTaskCreatePinnedToCore() only with an available core. On a single-core build (including S3
configured in unicore mode) select Core 0. Initialize peripheral interrupts from the intended core
or use the SDK's supported affinity configuration (e.g. isr_cpu_id where available in the pinned
SDK). Do not assume pinning a task automatically migrates its interrupts. Keep ISRs bounded; use
ISR-safe queue/notification APIs, DMA and preallocated buffers. Use task priorities appropriate to
measured deadlines; do not starve Wi-Fi, idle tasks or watchdog servicing. Pass ownership of
complete buffers/frames between tasks using queues or a documented ring-buffer strategy. Network
stalls must not block the receive path. Define and count overload handling explicitly.

## 9. Validation and deliverables

The implementing LLM must first review existing firmware and PCB, identify the actual RX mechanism
and SDK version, and produce a change plan. Then implement minimal target-specific changes without
unrelated refactoring.

- Build S2 and S3 independently; report exact framework versions, board settings and build
  commands. Check unicore builds if supported.
- Produce a pin mapping diff showing all six physical nets unchanged. Run KiCad ERC/DRC and
  manually verify orientation and power paths before manufacture.
- Check reset/boot signal levels with oscilloscope; verify both modules program normally.
- Validate TX clock frequency, data timing, bit order and busy polarity against existing hardware.
- Validate externally clocked RX at nominal speed, including back-to-back frames, no-CS reception,
  arbitrary clock pauses and every buffer boundary.
- Test concurrent RX/TX with Wi-Fi disabled and enabled, including sustained network load,
  reconnects and congestion. Compare against known input captures and report missing/duplicate
  bits, CRC failures, overruns, high-water buffer usage and CPU load.
- Test busy flow control in both directions, reset recovery and malformed frames. Do not declare
  hardware support complete solely because builds succeed.
- Provide changed files, test results, limitations, unresolved issues and any assumptions. No PCB
  redesign or protocol changes without explicit approval.

## 10. Primary references

Manufacturer schematics (V1.0.0, visually checked for the header mapping):
- https://www.wemos.cc/en/latest/_static/files/sch_s2_mini_v1.0.0.pdf
- https://www.wemos.cc/en/latest/_static/files/sch_s3_mini_v1.0.0.pdf
- https://www.wemos.cc/en/latest/s2/s2_mini.html
- https://www.wemos.cc/en/latest/s3/s3_mini.html

Espressif SPI documentation (select the version matching the project):
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/peripherals/spi_master.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/peripherals/spi_slave.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_slave.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/rmt.html
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/performance/speed.html
- https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s2/schematic-checklist.html
- https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html

Verification boundary (as written in the brief): Manufacturer schematic-level pin compatibility
is verified for the referenced revisions. The existing source tree, carrier PCB, exact ND busy
timing and live hardware were not supplied for this document. The proposed software architecture
and receive strategy are requirements for implementation and testing, not evidence of a working
implementation. LocalTalk and USB storage are not part of this six-signal ND design brief.

---

## Validation against the current design (2026-11-25)

Checked against: the schematic `F:\KiCad\ND\HDLCGateway\HDLCGateway-2026\HDLCGateway.kicad_sch`
(exported netlist and ERC with kicad-cli 9.0.7), the WEMOS S3 mini schematic V1.0.0 (headers J2,
J5, J4, J3, read from the PDF), the project's S2 mini symbol, and the firmware files in
`src/external/libxmsg/src/libhdlc/esp32s2/`. The S2 mini schematic PDF was not re-read here; the
S2 column relies on the existing S2 mini symbol.

Status: **OK** = checked and matches. **OPEN** = not in the design or not checked; needs a
decision, a datasheet or a measurement. **CONFLICT** = the brief disagrees with other project
documents.

### Pin mapping (brief sections 1, 2, 4)

| Check | Status | Evidence |
|---|---|---|
| Six ND nets on the pads in the brief's table | **OK** | Netlist: TX_CLK = J2.4 (S2 IO7, S3 IO12), TX_DATA = J2.6 (IO11/IO11), RX_CLK = J2.7 (IO12/IO10), RX_DATA = J2.5 (IO9/IO13), BUSY_TX = J5.6 (IO10/IO8), BUSY_RX = J5.5 (IO8/IO7). All six identical to the brief. |
| Complete header reference, all 32 positions | **OK** | The S2 and S3 symbols in `HDLCGateway.kicad_sym` give the same name as the brief for every one of the 32 header positions. |
| Same physical pads for both modules | **OK** | Both symbols (`WEMOS_S2_mini`, `WEMOS_S3_mini`) use the same pad IDs and the same footprint `HDLCGateway:WEMOS_S2_mini`. Pad letters map to the maker's headers: A = J2, B = J5, C = J4, D = J3. |
| No strapping, USB, JTAG, UART0 or LED pin used | **OK** | S3: 7, 8, 10, 11, 12, 13. S2: 7, 8, 9, 10, 11, 12. None is in the brief's table of special pins. |
| Footprint row spacing and outline | **OPEN** | The distance between the left and right header groups is not dimensioned in the WEMOS drawings. The footprint outline was copied from the C3 mini. Print 1:1 and fit a real module (`kicad/todo.md` item 2). |

### Electrical (brief section 4)

| Check | Status | Evidence |
|---|---|---|
| Three drivers and three receivers for the six signals | **OK** | U1 (data), U2 (clock), U3 (busy), each MAX3490EESA (one driver, one receiver). U3 reaches the plug only through SW2. |
| Differential polarity against the ND cable tables | **OK** for pins, **OPEN** for clock edge | Data, clock and busy pins agree with the 325400 and 325402 OCR tables in `HDLC-3015-vs-MEGALINK-3023.md`. Which clock edge the ND uses is a firmware setting and is tested in `DB25-Pico-Test-Before-KiCad.md` step 7. |
| Termination at the receiving end | **OK, none needed** (decision 2026-11-25) | Ronny: the cable is 1 cm (plug straight into the ND) or at most about 1 m with an extender. MAX3490E driver transition time 11 ns typ / 25 ns max (reported from the analog.com datasheet page); 1 m of cable is about 5 ns one way, so reflections settle in tens of ns against about 1000 ns per bit at 983 kbit/s. Whether the ND terminates its own receivers is still unknown; U4 is sized for it either way. |
| Protection on the external cable side | **OK for ESD** | MAX3490E bus pins rated ±15 kV HBM; receivers output high with open inputs (reported from the analog.com datasheet page). No extra diodes. Overvoltage beyond the RS-422 common-mode range is not covered; ground is shared through DB-25 pin 8. |
| Decoupling of every transceiver | **OK** (part present) | C1, C2, C3, 100 nF, one per MAX3490E. Checked against the datasheet's recommended value: **not done**. |
| 3.3 V supply capacity | **OK** (by calculation) | U1 to U3 now run from their own ME6211C33 (U4, LCSC C82942) fed from module VBUS (J3.8), with 1 uF in and out (datasheet values). The module's 3V3 (J2.8) only feeds the ESP32. U4 loss at 100 mA would be (5 V - 3.3 V) x 0.1 A = 0.17 W, under the 300 mW SOT-23-5 rating. |
| VBUS backfeed | **OK** | The carrier takes VBUS (J3.8) only as input to U4 and has no other supply. Nothing can backfeed. |
| Safe reset states of TX clock, TX data, BUSY_TO_ND | **OPEN** | No pull resistors in the design. During ESP32 reset the three DI inputs are driven by unconfigured GPIOs, so the ND may see undefined levels or noise. The required idle levels come from the ND side and are not documented yet. The firmware parks TX data at 1 between chunks (`spi_tx_task.c`, `data_io_default_level = true`); that does not cover reset. |
| Test points | **OPEN** | None in this schematic. (The older HDLC-ESP32-S2 schematic had TP1 to TP7.) |
| Antenna clearance, USB-C and button access | **OK, check on hardware** | Square 61.84 mm board with three M3 holes, none under the module; module USB-C at the right edge. Under the antenna end of the module: copper keep-out on both layers (bare FR-4). Wi-Fi strength to be checked on the first board. Module buttons sit on the module and stay reachable from above. |
| ERC | **OK** | 0 errors on the published schematic (warnings only for the draft's library path). DRC: 0 errors, 0 unconnected; silkscreen warnings only. |

### Firmware (brief sections 5 to 8)

| Check | Status | Evidence |
|---|---|---|
| TX on SPI2 master, no CS, LSB first | **OK** (S2 code) | `spi_tx_task.c` line 249 to 251: `dev.mode = 0`, `dev.spics_io_num = -1`, `SPI_DEVICE_BIT_LSBFIRST`, on `SPI2_HOST`. |
| RX on SPI3 as slave with data on the slave MOSI input, no external CS | **OK** (S2 code) | `spi_rx_task.c` line 275 to 276 route GPIO 12 and 9 to `SPI3_CLK_IN_IDX` and `SPI3_D_IN_IDX`. CS is driven from inside the GPIO matrix (header comment, TRM 5.2.3). |
| Existing CS-less receive backend | **exists, S2 only** | The header of `spi_rx_task.c` records a bench loopback on one board, 24 of 24 frames good at 9600, 38400 and 983000 bit/s, 2026-11-08. This is the backend the brief says to inspect and keep. Against a real ND it is not yet tested. |
| Builds for the S3 | **OPEN** | Both files compile only under `CONFIG_IDF_TARGET_ESP32S2` and program S2 registers directly. The S3 port, and checking the S3 register behaviour against the S3 TRM, is not done. |
| Central board pin definitions | **OPEN** | Pins are `#define`d in each file (`spi_tx_task.c` 43 to 44, `spi_rx_task.c` 69 to 70). Busy GPIOs are not defined anywhere. |
| Busy semantics to preserve | **CONFLICT** | The brief says "preserve the existing busy semantics", but no firmware drives or reads busy today, and which level means busy is not documented (test plan section 10). |

### Statements in the brief that disagree with other project documents

| Brief says | Project documents say | Status |
|---|---|---|
| "approximately 987 kbit/s synchronous HDLC" | MEGALINK top speed 983 kbps; HDLC 3015 computer link top 307.2 kbps (`HDLC-3015-vs-MEGALINK-3023.md`, from the Service Handbook pages). Firmware bench test at 983000 bit/s. | **CONFLICT**: 987 is not found in any project document. Use 983 (MEGALINK) or 307.2 (HDLC) until a source says otherwise. |
| One mode at about 987 kbit/s | Two modes: HDLC (3015, straps, about 300 kbit/s) and MEGALINK (3023, busy, up to about 1000 kbit/s) | The brief does not know the two modes or the straps (SW1). |
| Busy is part of the six signals in every case | Busy is only used in MEGALINK mode, through SW2 | Not a conflict in wiring; the brief's six-signal view is the MEGALINK case. |
