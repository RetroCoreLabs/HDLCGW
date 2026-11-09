/**************************************************************************
** HOT_PATH - place per-byte code and data in internal RAM               **
**                                                                       **
** Why: on the ESP32-S2 code runs from flash and constant tables are     **
** read from flash, both through the cache, and the ring buffers were in **
** PSRAM, also behind the cache. Every byte on the line passes through   **
** a handful of small functions (ring buffer, bit stuffing, CRC, byte    **
** stuffing), so those and the data they touch per byte live in the      **
** chip's internal SRAM instead, where a cache miss cannot stall them    **
** (Ronny's ruling 2026-11-09: hot path in internal RAM, not PSRAM or    **
** flash, as far as memory allows).                                      **
**                                                                       **
** Off the ESP (hdlc.cpp and byte_frame.cpp built for PC unit tests) the **
** two macros expand to nothing.                                         **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef HOT_PATH_H
#define HOT_PATH_H

#ifdef ESP_PLATFORM
#include <esp_attr.h>
/** Put a function in internal instruction RAM (IRAM), and never fold it
 *  into a caller: an inlined copy lands wherever the caller lives, which may
 *  be flash (seen 2026-11-09: fill_chunk inlined into queue_chunk ran from
 *  flash although it was marked). */
#define HOT_PATH_CODE IRAM_ATTR __attribute__((noinline))
/** Put a table or variable in internal data RAM, also when it is const. */
#define HOT_PATH_DATA DRAM_ATTR
#else
#define HOT_PATH_CODE
#define HOT_PATH_DATA
#endif

#endif /* HOT_PATH_H */
