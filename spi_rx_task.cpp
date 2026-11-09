/**************************************************************************
** SPI_RX_TASK - receive half of the synchronous line                    **
**                                                                       **
** Pins (any GPIO works, the signals go through the GPIO matrix):         **
**   PIN_RX_CLOCK GPIO 12 <- receive clock from the far end               **
**   PIN_RX_DATA  GPIO 9  <- RD, the data we receive                      **
** No chip-select pin. The slave's CS input is driven from inside the    **
** GPIO matrix: ESP32-S2 TRM 5.2.3 (page 169) - selecting input 0x38     **
** gives a constant 1, 0x3C a constant 0. CS starts at 1 (deselected)    **
** and is switched to 0 once everything is armed; that switch is the     **
** falling edge the slave wants.                                         **
**                                                                       **
** Register facts this file relies on (ESP32-S2 TRM, Version 1.3):       **
**  - 24.5 (p.591): in slave mode "The CS signal should be held low      **
**    during the transmission, and its falling/rising edges indicate the **
**    start/end of a transmission."                                      **
**  - 24.5.4 and 24.8 (p.596, 600): DMA full-duplex segmented-transfer   **
**    mode (SPI_DMA_SLV_SEG_TRANS_EN) keeps receiving as long as CS is   **
**    low; with SPI_RX_EOF_EN = 0 no end-of-frame is ever generated, so  **
**    the DMA just walks the descriptor chain.                           **
**  - 24.9 (p.601): slave input clock up to f_apb/2 = 40 MHz.            **
**  - 24.11.2 (p.608): SPI_INFIFO_FULL_ERR_INT = DMA RX FIFO full while  **
**    data still arrives, i.e. we lost bytes.                            **
** ESP-IDF v5.5.5 facts (esp32s2 hal/spi_ll.h):                          **
**  - spi_ll_slave_set_rx_bitlen() and spi_ll_dma_rx_enable() are empty  **
**    on the S2 ("not used in esp32s2"); the length is not programmed.   **
**  - the DMA engine's descriptor is the ROM lldesc_t; a descriptor is   **
**    4-byte aligned, buffer size a multiple of 4, owner bit 1 = DMA.    **
**  - spi_dma_ll_rx_start() writes dma_in_link.addr (20 bits) and start. **
**                                                                       **
** Two things the manual does not state for slave mode were confirmed on **
** the bench 2026-11-08 (loopback from spi_tx_task on the same board,    **
** 24 of 24 frames good at 9600, 38400 and 983000 bit/s): the DMA hands  **
** descriptors back (owner -> software) one by one while the transfer    **
** stays open, and the matrix switch of CS from 0x38 to 0x3C counts as   **
** the falling edge the slave needs. Pins: datasheet v1.9 Table 2-3 puts **
** SPI3 on no IO MUX pin at all, so SPI3 always goes through the matrix. **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include <esp_heap_caps.h>
#include "spi_rx_task.h"

#include <esp_timer.h>
#include "hot_path.h"

#include <string.h>

#include <Arduino.h>
#include <driver/gpio_filter.h>
#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <hal/spi_ll.h>
#include <soc/gpio_pins.h>
#include <soc/gpio_sig_map.h>
#include <soc/lldesc.h>

#include "console_out.h"
#include "control_frame.h"
#include "hdlc.h"

#define PIN_RX_CLOCK 12
#define PIN_RX_DATA  9

/* Descriptor owner bit (lldesc_t.owner), values from the ROM header
 * esp32s2/rom/lldesc.h: LLDESC_SW_OWNED = 0, LLDESC_HW_OWNED = 1. That header
 * is not on the sketch include path, so the two values are repeated here. */
#define OWNER_SOFTWARE 0
#define OWNER_DMA      1

/* SPI mode for the line: data changes on one clock edge and is sampled on
 * the other. Mode 0 matches the transmit side and SDLCBridge. */
#define RX_SPI_MODE 0

/* DMA ring: DESC_COUNT buffers of DESC_BYTES each. DESC_BYTES must be a
 * multiple of 4 (DMA requirement) and below 4092 (12-bit size field).
 *
 * Why 4 bytes, the smallest possible: a buffer reaches software only when it
 * is FULL. Measured on the bench 2026-11-08 with an RC422Dev (Pico) as the far
 * end, whose clock stops right after a frame: a 9-byte frame sat complete in
 * a 64-byte buffer (RXDMA dump: 7E 01 03 48 49 62 58 7E, then zeros) and was
 * never delivered, because nothing arrived to fill the other 55 bytes. With
 * one word per buffer every completed word is delivered at once. The DMA
 * writes memory in whole 32-bit words, so the last 1..3 bytes before a far
 * end stops its clock still stay in the hardware; the slave's byte counter
 * (spi_ll_slave_get_rx_byte_len) reads 0 in this mode and cannot tell us how
 * many. A far end with a continuous clock, or one that sends at least three
 * bytes of fill after its closing flag, never hits that.
 *
 * 512 words = 2 KB = 16 ms of line at 983 kbit/s; the task runs every 1 ms
 * and takes up to 31 words per run at that rate. */
#define DESC_COUNT 512
#define DESC_BYTES 4

/* A buffer handed to the DMA is filled with this first, so software can tell
 * a word the DMA has written from one it has not, before the descriptor is
 * closed. All ones is the one value a frame cannot contain (bit stuffing
 * allows at most five ones in a row, a flag six), and a written word of all
 * ones is idle line, which carries nothing and would be dropped anyway. */
#define SENTINEL 0xFFu

/* 4 KB = about 33 ms of line traffic at 983 kbit/s. console_task drains it
 * every millisecond, so this is slack for the moments console_task is busy
 * with a frame from the host, not steady-state storage. Internal RAM, not
 * PSRAM, because every received byte passes through it (hot_path.h); it was
 * 16 KB in PSRAM until 2026-11-09, and internal RAM has room for 4 KB. STAT
 * shows how full it ever got (high water). */
#define BUFFER_SIZE 4096

static spi_dev_t *const g_hw = &GPSPI3;

/* Descriptors and buffers must sit in internal DRAM (DMA capable) and be
 * 4-byte aligned. Static arrays in DRAM satisfy both on the S2. */
static DRAM_ATTR __attribute__((aligned(4))) lldesc_t g_desc[DESC_COUNT];
static DRAM_ATTR __attribute__((aligned(4))) uint8_t g_dma_buf[DESC_COUNT][DESC_BYTES];

ring_buffer g_rx_buffer;    /* storage allocated in init, see ring_buffer_alloc() */

static spi_rx_task_stats g_stats;
static int g_next_index = 0;      /* descriptor the task expects back next */
static bool g_peeked[DESC_COUNT]; /* word taken before the DMA closed it */

/* Peeked descriptors in ring order, oldest first. The DMA closes them in
 * that order as it moves on; each one is handed back the moment it is
 * closed. Handing them back only when the task's own index came round again
 * was a bug (bench 2026-11-08, step D): the DMA reached a still
 * software-owned descriptor one lap later, before the task did, and from
 * then on every word landed somewhere wrong. */
static int g_peek_fifo[DESC_COUNT];
static int g_peek_head = 0;
static int g_peek_count = 0;
static uint8_t g_last_pushed = 0;

void spi_rx_task_init(void)
{
    if (!ring_buffer_alloc_internal(&g_rx_buffer, BUFFER_SIZE))
    {
        Serial.println("spi_rx_task: no internal RAM for the receive ring");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void spi_rx_task_get_stats(spi_rx_task_stats *out)
{
    *out = g_stats;
}

void spi_rx_task_print_dma(void)
{
    console_out_printf("Slave byte counter: %u, DMA int raw 0x%08X, next descriptor %d\r\n",
                  (unsigned)spi_ll_slave_get_rx_byte_len(g_hw), (unsigned)g_hw->dma_int_raw.val,
                  g_next_index);
    /* 16 descriptors per line: owner letter (D = DMA, s = software) and the
     * buffer bytes. The ring is large; only the 64 descriptors around the
     * one the task expects next are shown. */
    int first = (g_next_index - 16 + DESC_COUNT) % DESC_COUNT;
    for (int row = 0; row < 4; row++)
    {
        console_out_printf("%4d:", (first + row * 16) % DESC_COUNT);
        for (int col = 0; col < 16; col++)
        {
            int i = (first + row * 16 + col) % DESC_COUNT;
            console_out_printf(" %c", g_desc[i].owner == OWNER_DMA ? 'D' : 's');
            for (int k = 0; k < DESC_BYTES; k++)
            {
                console_out_printf("%02X", g_dma_buf[i][k]);
            }
        }
        console_out_println();
    }
}

/**
 * @brief Link the descriptors into a ring and hand every buffer to the DMA.
 */
static void build_ring(void)
{
    for (int i = 0; i < DESC_COUNT; i++)
    {
        lldesc_t *d = &g_desc[i];
        d->size = DESC_BYTES;
        d->length = 0;
        d->offset = 0;
        d->sosf = 0;
        d->eof = 0;
        d->owner = OWNER_DMA;
        d->buf = g_dma_buf[i];
        d->qe.stqe_next = &g_desc[(i + 1) % DESC_COUNT];
        memset(g_dma_buf[i], SENTINEL, DESC_BYTES);
        g_peeked[i] = false;
    }
}

/**
 * @brief Whether the DMA has written this word: its buffer no longer holds
 *        the sentinel. See the SENTINEL comment for why idle ones are safe
 *        to mistake for "not written".
 * @param[in] i  Descriptor index.
 * @return true when data is there.
 */
static bool word_written(int i)
{
    const uint8_t *b = g_dma_buf[i];

    return (b[0] & b[1] & b[2] & b[3]) != SENTINEL;
}

/**
 * @brief Hand a descriptor back to the DMA: sentinel in the buffer, owner bit.
 * @param[in] i  Descriptor index.
 */
static void give_back(int i)
{
    memset(g_dma_buf[i], SENTINEL, DESC_BYTES);
    g_desc[i].length = 0;
    g_desc[i].owner = OWNER_DMA;
    g_peeked[i] = false;
}

/**
 * @brief Put the IO MUX glitch filter on one receive pin.
 *
 * The filter drops pulses shorter than two APB clocks (25 ns at 80 MHz),
 * invisible to a 19200..983000 bit/s signal. Why: with the Pico as the far
 * end over jumper wires, 1 frame in about 200 arrived with exactly one bit
 * read as 0 instead of 1 and the length intact (five of five bad frames,
 * bench 2026-11-08). That is a sub-microsecond dip on the data wire at the
 * sampling edge, most likely our own transmit clock's falling edge coupling
 * into the wire beside it. The far end's bits are stable 25 us either side
 * of the edge, so a 25 ns filter costs nothing.
 *
 * @param[in] pin  GPIO number.
 */
static void add_glitch_filter(int pin)
{
    gpio_pin_glitch_filter_config_t config;
    gpio_glitch_filter_handle_t filter = NULL;
    esp_err_t err;

    memset(&config, 0, sizeof(config));
    config.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT;
    config.gpio_num = (gpio_num_t)pin;
    err = gpio_new_pin_glitch_filter(&config, &filter);
    if (err == ESP_OK)
    {
        err = gpio_glitch_filter_enable(filter);
    }
    if (err != ESP_OK)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "glitch filter on GPIO %d: %s", pin,
                        esp_err_to_name(err));
    }
}

/**
 * @brief Route the clock and data pins into SPI3 and park CS at "deselected".
 *
 * Both inputs get the glitch filter before they are connected to the matrix.
 */
static void route_pins(void)
{
    pinMode(PIN_RX_CLOCK, INPUT);
    pinMode(PIN_RX_DATA, INPUT);
    add_glitch_filter(PIN_RX_CLOCK);
    add_glitch_filter(PIN_RX_DATA);
    esp_rom_gpio_connect_in_signal(PIN_RX_CLOCK, SPI3_CLK_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(PIN_RX_DATA, SPI3_D_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, SPI3_CS0_IN_IDX, false);
}

/**
 * @brief Program SPI3 as a full-duplex slave with segmented DMA receive.
 *
 * Order follows what ESP-IDF's spi_slave.c does per transaction (hw reset,
 * prepare rx DMA, user start) but set up once and left running.
 */
static void start_slave(void)
{
    periph_module_enable(PERIPH_HSPI_MODULE);
    periph_module_enable(PERIPH_SPI3_DMA_MODULE);

    spi_ll_slave_init(g_hw);                        /* full duplex, slave_mode = 1 */
    spi_ll_slave_set_mode(g_hw, RX_SPI_MODE, true);
    spi_ll_set_rx_lsbfirst(g_hw, true);             /* first wire bit -> bit 0 */
    spi_ll_set_tx_lsbfirst(g_hw, true);
    spi_ll_slave_set_seg_en(g_hw, true);            /* keep going, no end per CS */
    spi_ll_dma_set_rx_eof_generation(g_hw, false);  /* no EOF on a byte count */

    spi_ll_disable_int(g_hw);
    spi_ll_clear_int_stat(g_hw);
    g_hw->dma_int_ena.val = 0;
    g_hw->dma_int_clr.val = 0xFFFFFFFFu;

    build_ring();
    spi_ll_slave_reset(g_hw);
    spi_dma_ll_rx_reset(g_hw, 0);
    spi_ll_dma_rx_fifo_reset(g_hw);
    spi_ll_infifo_full_clr(g_hw);
    spi_dma_ll_rx_start(g_hw, 0, &g_desc[0]);

    spi_ll_clear_int_stat(g_hw);
    spi_ll_user_start(g_hw);

    /* Everything is armed: pull CS low inside the matrix. */
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, SPI3_CS0_IN_IDX, false);
}

/** Most filtered bytes collected before one locked write into g_rx_buffer.
 *  At 983000 bit/s one 1 ms pass brings about 123 bytes, so a pass normally
 *  ends in one write. */
#define PUSH_MAX 512

/** Filtered bytes waiting for the next locked write (see flush_pending).
 *  Allocated from internal heap at task start: as a static array it pushed
 *  dram0_0_seg over by 704 bytes at link time (2026-11-09, see CLAUDE.md). */
static uint8_t *g_pending = NULL;
/** Number of bytes in g_pending. */
static int g_pending_n = 0;

/**
 * @brief Write g_pending into g_rx_buffer under one lock and count what did
 *        not fit.
 *
 * One mutex round per pass instead of one per byte: per-byte writes made the
 * line receive task cost about 3.4 us a byte at 983000 (TASK run time, bench
 * 2026-11-09).
 */
static HOT_PATH_CODE void flush_pending(void)
{
    if (g_pending_n == 0)
    {
        return;
    }
    int written = ring_buffer_write_some(&g_rx_buffer, g_pending, g_pending_n);
    g_stats.bytes += (uint32_t)written;
    /* console_task is not keeping up. The bytes are gone; say so in STAT
     * instead of letting a frame fail its FCS in silence. */
    g_stats.overflow += (uint32_t)(g_pending_n - written);
    g_pending_n = 0;
}

/**
 * @brief Filter one filled DMA buffer into g_pending.
 *
 * Idle line is all ones. A second 0xFF in a row carries no information for
 * the HDLC decoder (one_count is already past six), so repeats are dropped
 * here. That keeps a fast idle line from flooding the ring buffer.
 *
 * @param[in] data  Bytes from the DMA buffer.
 * @param[in] n     Number of bytes.
 */
static HOT_PATH_CODE void push_bytes(const uint8_t *data, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint8_t b = data[i];
        if (b == HDLC_IDLE && g_last_pushed == HDLC_IDLE)
        {
            g_stats.idle_dropped++;
            continue;
        }
        if (g_pending_n == PUSH_MAX)
        {
            flush_pending();
        }
        g_pending[g_pending_n++] = b;
        g_last_pushed = b;
    }
}

/**
 * @brief Count and clear DMA error flags; restart the DMA after a FIFO overrun.
 */
static void check_errors(void)
{
    if (g_hw->dma_int_raw.infifo_full_err)
    {
        g_stats.fifo_full++;
        g_hw->dma_int_clr.infifo_full_err = 1;
        spi_ll_infifo_full_clr(g_hw);
        spi_dma_ll_rx_restart(g_hw, 0);
    }
    if (g_hw->dma_int_raw.in_err_eof)
    {
        g_stats.dma_errors++;
        g_hw->dma_int_clr.in_err_eof = 1;
    }
    /* The DMA met a descriptor it may not use (owner = software) or ran out
     * of them. Both mean the task fell a whole lap behind or mishandled a
     * descriptor; counted so STAT shows it instead of silent garbage. */
    if (g_hw->dma_int_raw.inlink_dscr_error || g_hw->dma_int_raw.inlink_dscr_empty)
    {
        g_stats.dma_errors++;
        g_hw->dma_int_clr.inlink_dscr_error = 1;
        g_hw->dma_int_clr.inlink_dscr_empty = 1;
    }
}

void spi_rx_task_run(void *params)
{
    (void)params;

    memset(&g_stats, 0, sizeof(g_stats));
    g_pending = (uint8_t *)heap_caps_malloc(PUSH_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_pending == NULL)
    {
        /* Without it there is no receive path at all; say so and stop. */
        Serial.println("spi_rx_task: no internal RAM for the receive batch");
        vTaskDelete(NULL);
        return;
    }
    route_pins();
    start_slave();

    int64_t last_pass = esp_timer_get_time();

    while (true)
    {
        int taken = 0;
        int64_t pass_start = esp_timer_get_time();
        uint32_t gap = (uint32_t)(pass_start - last_pass);

        last_pass = pass_start;
        if (gap > g_stats.pass_gap_max_us)
        {
            g_stats.pass_gap_max_us = gap;
        }
        if (gap > SPI_RX_GAP_WARN_US)
        {
            g_stats.pass_gaps_over++;
        }
        /* Walk the ring in order. Two ways a word reaches us:
         *  - the DMA closed the descriptor (owner = software): the normal
         *    case while data keeps coming;
         *  - the DMA wrote the word but has not closed the descriptor yet
         *    (owner still DMA, buffer no longer sentinel): the LAST word
         *    before a far end stops its clock. Measured 2026-11-08: with the
         *    Pico silent after a 9-byte frame, word 1 held 49 62 58 7E in
         *    memory with owner = DMA, and nothing would ever close it. We
         *    take such a word at once ("peek") and remember that, so the
         *    close that comes with the next transmission is not delivered
         *    twice; the descriptor goes back to the DMA on the next lap. */
        while (true)
        {
            int idx = g_next_index;
            lldesc_t *d = &g_desc[idx];

            if (d->owner == OWNER_SOFTWARE)
            {
                if (!g_peeked[idx])
                {
                    push_bytes(g_dma_buf[idx], DESC_BYTES);
                }
                give_back(idx);
            }
            else if (!g_peeked[idx] && word_written(idx))
            {
                push_bytes(g_dma_buf[idx], DESC_BYTES);
                g_peeked[idx] = true;
                g_peek_fifo[(g_peek_head + g_peek_count) % DESC_COUNT] = idx;
                g_peek_count++;
                g_stats.peeked++;
            }
            else
            {
                break;
            }
            g_next_index = (idx + 1) % DESC_COUNT;
            taken++;
        }
        if ((uint32_t)taken > g_stats.desc_max)
        {
            g_stats.desc_max = (uint32_t)taken;
        }

        flush_pending();

        /* Hand back every peeked descriptor the DMA has closed since. */
        while (g_peek_count > 0 && g_desc[g_peek_fifo[g_peek_head]].owner == OWNER_SOFTWARE)
        {
            give_back(g_peek_fifo[g_peek_head]);
            g_peek_head = (g_peek_head + 1) % DESC_COUNT;
            g_peek_count--;
        }

        check_errors();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
