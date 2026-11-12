/**************************************************************************
** BOARD - what both firmwares on one HDLC-ESP32-S2 board share           **
**                                                                       **
** See board.h for the slots, the button, the LEDs and the settings.     **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "board.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <stdlib.h>

/* The shared NVS namespace. Keys: "ssid", "pass", "rate"; "fw1"/"fw2" and
 * "id1"/"id2" (what each slot was last seen running); "mig" (the one-time copy
 * from the old per-firmware keys has been done). */
static const char *const NAMESPACE = "board";

static const char *const FW_NAME[2] = { "HDLCGW", "XMSGGW" };
static const uint8_t LED_PIN[2] = { BOARD_PIN_LED_HDLCGW, BOARD_PIN_LED_XMSGGW };

/* Flash offsets of the two app partitions in the default partition table,
 * only for the text FIRMWARE prints; the code finds them by type. */
static const uint32_t SLOT_ADDR[2] = { 0x10000u, 0x150000u };

/* How many bytes of the image's ELF SHA-256 (esp_app_desc_t.app_elf_sha256,
 * written per build by esptool elf2image --elf-sha256-offset) identify it. */
#define ID_BYTES 8

/* The button counts as held after this long low at start, sampled every BUTTON_STEP_MS. */
#define BUTTON_HELD_MS 50u
#define BUTTON_STEP_MS 5u

/* A button still held after this long is taken as stuck or shorted and ignored. */
#define BUTTON_STUCK_MS 10000u


typedef enum
{
    ASK_NONE,
    ASK_SSID,
    ASK_PASS,
    ASK_NODE
} ask_step;

/* Everything this module keeps between calls. In PSRAM (board_start), not in
 * fixed variables: HDLCGW's internal RAM is full to the last few bytes (the
 * line and USB rings live there, hot_path.h), and these 60 bytes did not fit
 * (link error "dram0_0_seg overflowed by 40 bytes", 2026-11-12). Only the
 * pointer is in internal RAM. NULL before board_start() or when the 60 bytes
 * could not be had: then nothing here acts. */
/* Most networks the WiFi question lists (strongest first, as the scan gives them). */
#define SCAN_MAX 20

typedef struct
{
    board_firmware me;
    int slot;                    /* running slot, 0 or 1; -1 when not an OTA slot */
    volatile bool trial;         /* chosen by a switch and not yet kept */
    bool led_on;
    const char *version;         /* board_start()'s, a string literal of the caller */
    const board_ask_ops *ops;    /* board_ask_start()'s */
    ask_step ask;
    char new_ssid[BOARD_SSID_SIZE];   /* typed name, kept until the password is in */
    int found;                        /* networks the scan listed, 0..SCAN_MAX */
    char found_ssid[SCAN_MAX][BOARD_SSID_SIZE];   /* their names, for an answer by number */
} board_state;

static board_state *S = NULL;

/**
 * Keep a newly chosen firmware on trial (board.h). The Arduino core calls this
 * weak C function in initArduino(); its own answer, false, marks every new
 * firmware good at once, so the bootloader could never go back. With true the
 * core leaves the firmware PENDING_VERIFY and board_tick() keeps it after
 * BOARD_TRIAL_MS.
 */
extern "C" bool verifyRollbackLater(void)
{
    return true;
}

/**
 * @brief The app partition of one slot.
 * @param[in] slot  0 or 1.
 * @return The partition, or NULL when the table has none.
 */
static const esp_partition_t *slot_partition(int slot)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot),
                                    NULL);
}

/**
 * @brief Which slot a partition is.
 * @return 0 or 1, or -1 when it is not one of the two OTA slots.
 */
static int partition_slot(const esp_partition_t *part)
{
    if (part == NULL || part->type != ESP_PARTITION_TYPE_APP)
    {
        return -1;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
    {
        return 0;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
    {
        return 1;
    }
    return -1;
}

/** @brief Turn both LEDs on or off, or just ours. */
static void leds(bool own, bool other)
{
    digitalWrite(LED_PIN[S->me], own ? HIGH : LOW);
    digitalWrite(LED_PIN[1 - (int)S->me], other ? HIGH : LOW);
}

/**
 * @brief Whether the button is held now: low for BUTTON_HELD_MS without a break.
 */
static bool button_held(void)
{
    uint32_t low_ms = 0;
    while (low_ms < BUTTON_HELD_MS)
    {
        if (digitalRead(BOARD_PIN_BUTTON) != LOW)
        {
            return false;
        }
        delay(BUTTON_STEP_MS);
        low_ms += BUTTON_STEP_MS;
    }
    return true;
}

/**
 * @brief Whether a slot holds a firmware image; fills its description.
 */
static bool slot_has_firmware(int slot, esp_app_desc_t *desc)
{
    const esp_partition_t *part = slot_partition(slot);
    return part != NULL && esp_ota_get_partition_description(part, desc) == ESP_OK;
}

/**
 * @brief Remember which firmware this slot holds, for FIRMWARE. Written only
 *        when it changed, to spare the flash.
 * @param[in] version  Our version text.
 */
static void record_slot(const char *version)
{
    Preferences p;
    char key[4] = { 'f', 'w', '1', '\0' };
    char ikey[4] = { 'i', 'd', '1', '\0' };
    char name[40];
    char old[40];
    uint8_t old_id[ID_BYTES];
    const esp_app_desc_t *me = esp_app_get_description();

    if (S->slot < 0 || !p.begin(NAMESPACE, false))
    {
        return;
    }
    key[2] = (char)('1' + S->slot);
    ikey[2] = (char)('1' + S->slot);
    (void)snprintf(name, sizeof name, "%s %s", FW_NAME[S->me], version);
    old[0] = '\0';
    if (p.isKey(key))
    {
        (void)p.getString(key, old, sizeof old);
    }
    if (strcmp(old, name) != 0)
    {
        (void)p.putString(key, name);
    }
    memset(old_id, 0, sizeof old_id);
    if (p.isKey(ikey))
    {
        (void)p.getBytes(ikey, old_id, sizeof old_id);
    }
    if (memcmp(old_id, me->app_elf_sha256, ID_BYTES) != 0)
    {
        (void)p.putBytes(ikey, me->app_elf_sha256, ID_BYTES);
    }
    p.end();
}

/**
 * @brief The name a slot was last seen running, when its image is still the same one.
 * @param[in]  slot  0 or 1.
 * @param[in]  desc  The image's description.
 * @param[out] name  At least 40 bytes; "" when not known.
 */
static void slot_name(int slot, const esp_app_desc_t *desc, char *name, size_t cap)
{
    Preferences p;
    char key[4] = { 'f', 'w', (char)('1' + slot), '\0' };
    char ikey[4] = { 'i', 'd', (char)('1' + slot), '\0' };
    uint8_t id[ID_BYTES];

    name[0] = '\0';
    if (!p.begin(NAMESPACE, true))
    {
        return;
    }
    if (p.isKey(key) && p.isKey(ikey) && p.getBytes(ikey, id, sizeof id) == sizeof id &&
        memcmp(id, desc->app_elf_sha256, ID_BYTES) == 0)
    {
        (void)p.getString(key, name, cap);
    }
    p.end();
}

/**
 * @brief Copy one string key from an old namespace, when it is there and not empty.
 * @return true when something was copied into out.
 */
static bool old_string(const char *ns, const char *key, char *out, size_t cap)
{
    Preferences p;
    out[0] = '\0';
    if (!p.begin(ns, true))
    {
        return false;
    }
    if (p.isKey(key))
    {
        (void)p.getString(key, out, cap);
    }
    p.end();
    return out[0] != '\0';
}

/**
 * @brief Once per board: copy the WiFi network and the line rate from the
 *        keys each firmware kept before (XMSGGW's first, it was set later and
 *        on purpose; then HDLCGW's). The old keys are left as they were, so an
 *        older build of either firmware still finds them.
 */
static void migrate_settings(void)
{
    Preferences p;
    char ssid[BOARD_SSID_SIZE];
    char pass[BOARD_PASS_SIZE];

    if (!p.begin(NAMESPACE, false))
    {
        return;
    }
    if (p.isKey("mig"))
    {
        p.end();
        return;
    }
    if (!p.isKey("ssid"))
    {
        bool found = old_string("xmsggw", "wssid", ssid, sizeof ssid);
        const char *ns = "xmsggw";
        const char *pkey = "wpass";
        if (!found)
        {
            found = old_string("hdlcgw", "ssid", ssid, sizeof ssid);
            ns = "hdlcgw";
            pkey = "pass";
        }
        if (found)
        {
            (void)old_string(ns, pkey, pass, sizeof pass);
            (void)p.putString("ssid", ssid);
            (void)p.putString("pass", pass);
            memset(pass, 0, sizeof pass);
        }
    }
    if (!p.isKey("rate"))
    {
        Preferences old;
        uint32_t rate = 0;
        if (old.begin("xmsggw", true))
        {
            rate = old.getULong("rate", 0);
            old.end();
        }
        if (rate != 0)
        {
            (void)p.putULong("rate", rate);
        }
    }
    (void)p.putUChar("mig", 1);
    p.end();
}

void board_start(board_firmware me, const char *version)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();

    S = (board_state *)heap_caps_calloc(1, sizeof *S, MALLOC_CAP_SPIRAM);
    if (S == NULL)
    {
        S = (board_state *)calloc(1, sizeof *S);
    }
    if (S == NULL)
    {
        return;   /* no LEDs, no button, no switch; the firmware itself still runs */
    }
    S->led_on = true;
    S->ask = ASK_NONE;
    S->me = me;
    S->version = version;
    S->slot = partition_slot(running);
    pinMode(LED_PIN[0], OUTPUT);
    pinMode(LED_PIN[1], OUTPUT);
    leds(true, false);
    pinMode(BOARD_PIN_BUTTON, INPUT_PULLUP);

    S->trial = (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
               state == ESP_OTA_IMG_PENDING_VERIFY);

    record_slot(version);
    migrate_settings();

    delay(2); /* let the pull-up lift the pin before the first read */
    if (S->slot >= 0 && button_held())
    {
        uint32_t held_ms = 0;
        esp_app_desc_t desc;
        leds(true, true);
        while (digitalRead(BOARD_PIN_BUTTON) == LOW && held_ms < BUTTON_STUCK_MS)
        {
            delay(10);
            held_ms += 10u;
        }
        if (held_ms < BUTTON_STUCK_MS && slot_has_firmware(1 - S->slot, &desc) &&
            esp_ota_set_boot_partition(slot_partition(1 - S->slot)) == ESP_OK)
        {
            leds(false, false);
            esp_restart();
        }
        /* Nothing to switch to, or the button is stuck: say so with three
         * quick blinks and carry on with this firmware. */
        for (int i = 0; i < 3; i++)
        {
            leds(false, false);
            delay(150);
            leds(true, false);
            delay(150);
        }
    }
}

void board_tick(uint32_t now_ms)
{
    if (S == NULL || !S->trial)
    {
        return;
    }
    if (now_ms >= BOARD_TRIAL_MS)
    {
        (void)esp_ota_mark_app_valid_cancel_rollback();
        S->trial = false;
        S->led_on = true;
        leds(true, false);
        return;
    }
    S->led_on = !S->led_on;
    leds(S->led_on, false);
}

/**
 * @brief One FIRMWARE line for a slot.
 */
static size_t report_slot(int slot, char *out, size_t cap)
{
    esp_app_desc_t desc;
    char name[40];
    esp_ota_img_states_t state;
    const esp_partition_t *part = slot_partition(slot);
    int n;

    if (slot == S->slot)
    {
        n = snprintf(out, cap, "  Slot %d (0x%lx): %s %s - running now%s%s\r\n", slot + 1,
                     (unsigned long)SLOT_ADDR[slot], FW_NAME[S->me], S->version,
                     S->trial ? " (on trial: kept after 10 s)" : "",
                     ((int)S->me != slot) ? " (it belongs in the other slot)" : "");
    }
    else if (!slot_has_firmware(slot, &desc))
    {
        n = snprintf(out, cap, "  Slot %d (0x%lx): empty\r\n", slot + 1,
                     (unsigned long)SLOT_ADDR[slot]);
    }
    else
    {
        bool failed = part != NULL && esp_ota_get_state_partition(part, &state) == ESP_OK &&
                      (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED);
        slot_name(slot, &desc, name, sizeof name);
        n = snprintf(out, cap, "  Slot %d (0x%lx): %s%s\r\n", slot + 1,
                     (unsigned long)SLOT_ADDR[slot],
                     name[0] != '\0' ? name : "a firmware that has not run on this board yet",
                     failed ? " - did not start the last time, the board went back" : "");
    }
    if (n < 0)
    {
        return 0;
    }
    return ((size_t)n < cap) ? (size_t)n : cap - 1u;
}

void board_firmware_report(char *out, size_t cap)
{
    size_t used = 0;
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    int next = partition_slot(boot);
    int n;

    if (cap == 0)
    {
        return;
    }
    out[0] = '\0';
    if (S == NULL)
    {
        (void)snprintf(out, cap, "Firmware slots: not known (no memory at start).\r\n");
        return;
    }
    n = snprintf(out, cap, "Firmware (slot 1 = HDLCGW, slot 2 = XMSGGW):\r\n");
    used = (n > 0 && (size_t)n < cap) ? (size_t)n : cap - 1u;
    used += report_slot(0, out + used, cap - used);
    used += report_slot(1, out + used, cap - used);
    if (next >= 0 && next != S->slot)
    {
        n = snprintf(out + used, cap - used, "  Slot %d starts at the next restart.\r\n",
                     next + 1);
        used += (n > 0 && (size_t)n < cap - used) ? (size_t)n : 0u;
    }
    (void)snprintf(out + used, cap - used,
                   "  FIRMWARE SWITCH, or hold the button while the board starts, "
                   "to run the other one.\r\n");
}

int board_firmware_switch(char *out, size_t cap)
{
    esp_app_desc_t desc;
    esp_err_t err;
    int other;

    if (S == NULL || S->slot < 0)
    {
        (void)snprintf(out, cap, "This firmware is not in slot 1 or 2; nothing to switch.\r\n");
        return -1;
    }
    other = 1 - S->slot;
    if (!slot_has_firmware(other, &desc))
    {
        (void)snprintf(out, cap, "Slot %d is empty; install a firmware there first.\r\n",
                       other + 1);
        return -1;
    }
    /* We are running well enough to take a command: keep this one, so a
     * failed start of the other comes back here. */
    if (S->trial)
    {
        (void)esp_ota_mark_app_valid_cancel_rollback();
        S->trial = false;
    }
    err = esp_ota_set_boot_partition(slot_partition(other));
    if (err != ESP_OK)
    {
        (void)snprintf(out, cap, "Could not choose slot %d (%s); still running this one.\r\n",
                       other + 1, esp_err_to_name(err));
        return -1;
    }
    (void)snprintf(out, cap,
                   "Restarting into slot %d. If it restarts within 10 s, the board comes "
                   "back here.\r\n",
                   other + 1);
    return 0;
}

int board_wifi(char *ssid, char *pass)
{
    Preferences p;
    ssid[0] = '\0';
    pass[0] = '\0';
    if (p.begin(NAMESPACE, true))
    {
        if (p.isKey("ssid"))
        {
            (void)p.getString("ssid", ssid, BOARD_SSID_SIZE);
        }
        if (p.isKey("pass"))
        {
            (void)p.getString("pass", pass, BOARD_PASS_SIZE);
        }
        p.end();
    }
    return (ssid[0] != '\0') ? 0 : -1;
}

/**
 * @brief Store one string key in the shared namespace.
 * @return 0 or -1.
 */
static int put_text(const char *key, const char *value, size_t max)
{
    Preferences p;
    size_t len = strlen(value);
    size_t written = 0;
    bool opened;
    if (len > max)
    {
        return -1;
    }
    opened = p.begin(NAMESPACE, false);
    if (opened)
    {
        written = p.putString(key, value);
        p.end();
    }
    /* An empty password writes 0 bytes, so the open must be checked too. */
    return (opened && written == len) ? 0 : -1;
}

bool board_no_wifi(void)
{
    Preferences p;
    bool no = false;
    if (p.begin(NAMESPACE, true))
    {
        no = p.isKey("nowifi") && p.getUChar("nowifi", 0) != 0u;
        p.end();
    }
    return no;
}

/** @brief Store or clear the "no WiFi wanted" answer (key "nowifi"). */
static void set_no_wifi(bool no)
{
    Preferences p;
    if (!p.begin(NAMESPACE, false))
    {
        return;
    }
    if (no)
    {
        (void)p.putUChar("nowifi", 1);
    }
    else if (p.isKey("nowifi"))
    {
        (void)p.remove("nowifi");
    }
    p.end();
}

int board_set_wifi_ssid(const char *ssid)
{
    if (ssid[0] == '\0')
    {
        return -1;
    }
    /* A network named on purpose overrides an earlier "no WiFi" answer. */
    set_no_wifi(false);
    return put_text("ssid", ssid, BOARD_SSID_SIZE - 1u);
}

int board_set_wifi_pass(const char *pass)
{
    return put_text("pass", pass, BOARD_PASS_SIZE - 1u);
}

int board_clear_wifi(void)
{
    Preferences p;
    if (!p.begin(NAMESPACE, false))
    {
        return -1;
    }
    (void)p.remove("ssid");
    (void)p.remove("pass");
    (void)p.remove("nowifi");   /* back to the start: the console asks again */
    p.end();
    return 0;
}

uint32_t board_rate(void)
{
    Preferences p;
    uint32_t rate = 0;
    if (p.begin(NAMESPACE, true))
    {
        rate = p.getULong("rate", 0);
        p.end();
    }
    return rate;
}

int board_set_rate(uint32_t rate)
{
    Preferences p;
    size_t written = 0;
    if (p.begin(NAMESPACE, false))
    {
        written = p.putULong("rate", rate);
        p.end();
    }
    return (written == sizeof rate) ? 0 : -1;
}

/* ------------------------------------------------- missing settings */



static const char ASK_SSID_TEXT[] =
    "WiFi network: its number above or its name; NONE for no WiFi (not asked again);\r\n"
    "Enter to skip for now: ";
static const char ASK_PASS_TEXT[] = "WiFi password (Enter for an open network): ";
static const char ASK_NODE_TEXT[] = "Node number, 1 to 65535 (Enter to skip): ";

int board_parse_node(const char *text, uint16_t *node)
{
    uint32_t value = 0;
    size_t i = 0;
    if (text[0] == '\0')
    {
        return -2;
    }
    for (i = 0; text[i] != '\0'; i++)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return -2;
        }
        if (value <= 65535u)
        {
            value = (value * 10u) + (uint32_t)(text[i] - '0');
        }
    }
    if (value < 1u || value > 65535u)
    {
        return -1;
    }
    *node = (uint16_t)value;
    return 0;
}

/**
 * @brief List the networks the board can see, numbered, and keep their names
 *        so the answer can be a number. Blocks about 3 s (the radio listens on
 *        every channel); HDLCGW's WIFI SCAN does the same from the console.
 *        Only called while no network is stored, so the WiFi task is not
 *        using the radio.
 */
static void scan_networks(void)
{
    char text[96];
    int16_t n;

    S->found = 0;
    S->ops->print("Looking for networks (about 3 s)...\r\n");
    n = WiFi.scanNetworks(false, true);
    if (n <= 0)
    {
        S->ops->print("  none found\r\n");
        WiFi.scanDelete();
        return;
    }
    for (int16_t i = 0; i < n && S->found < SCAN_MAX; i++)
    {
        String name = WiFi.SSID((uint8_t)i);
        if (name.length() == 0u || name.length() > BOARD_SSID_SIZE - 1u)
        {
            continue;   /* a hidden network has no name to pick */
        }
        memcpy(S->found_ssid[S->found], name.c_str(), name.length() + 1u);
        S->found++;
        (void)snprintf(text, sizeof text, "  %2d  %-32s %4d dBm  %s\r\n", S->found,
                       name.c_str(), (int)WiFi.RSSI((uint8_t)i),
                       (WiFi.encryptionType((uint8_t)i) == WIFI_AUTH_OPEN) ? "open" : "password");
        S->ops->print(text);
    }
    WiFi.scanDelete();
}

/** @brief Ask for the node number when this firmware has one and it is missing; else end. */
static bool ask_node_or_end(void)
{
    if (S->ops->node != NULL && S->ops->node() == 0u)
    {
        S->ask = ASK_NODE;
        S->ops->print("This board has no node number, so the XMSG link cannot start.\r\n");
        S->ops->print(ASK_NODE_TEXT);
        return true;
    }
    S->ask = ASK_NONE;
    return false;
}

bool board_ask_start(const board_ask_ops *ops)
{
    char ssid[BOARD_SSID_SIZE];
    char pass[BOARD_PASS_SIZE];
    int stored;

    if (S == NULL)
    {
        return false;
    }
    S->ops = ops;
    S->ask = ASK_NONE;
    if (ops == NULL || ops->print == NULL)
    {
        return false;
    }
    stored = board_wifi(ssid, pass);
    memset(pass, 0, sizeof pass);
    if (stored != 0 && !board_no_wifi() && ops->set_ssid != NULL && ops->set_pass != NULL)
    {
        S->ask = ASK_SSID;
        ops->print("No WiFi network is stored.\r\n");
        scan_networks();
        ops->print(ASK_SSID_TEXT);
        return true;
    }
    return ask_node_or_end();
}

bool board_ask_active(void)
{
    return S != NULL && S->ask != ASK_NONE;
}

bool board_ask_hidden(void)
{
    return S != NULL && S->ask == ASK_PASS;
}

void board_ask_answer(const char *line)
{
    char text[96];
    uint16_t node = 0;
    size_t len = strlen(line);

    if (S == NULL)
    {
        return;
    }
    switch (S->ask)
    {
    case ASK_SSID:
        if (len == 0u)
        {
            S->ops->print("Skipped; asked again next time. WIFI SSID and WIFI PASS set it.\r\n");
            (void)ask_node_or_end();
            return;
        }
        if (strcasecmp(line, "NONE") == 0)
        {
            set_no_wifi(true);
            S->ops->print("No WiFi; not asked again. WIFI SSID <name> sets a network later.\r\n");
            (void)ask_node_or_end();
            return;
        }
        if (board_parse_node(line, &node) == 0 && node <= (uint16_t)S->found)
        {
            /* A number from the list. (A network whose name is all digits
             * cannot be picked by typing it; pick its number instead.) */
            memcpy(S->new_ssid, S->found_ssid[node - 1u], BOARD_SSID_SIZE);
        }
        else if (len > BOARD_SSID_SIZE - 1u)
        {
            S->ops->print("At most 32 characters.\r\n");
            S->ops->print(ASK_SSID_TEXT);
            return;
        }
        else
        {
            memcpy(S->new_ssid, line, len + 1u);
        }
        (void)snprintf(text, sizeof text, "Network %s.\r\n", S->new_ssid);
        S->ops->print(text);
        S->ask = ASK_PASS;
        S->ops->print(ASK_PASS_TEXT);
        return;
    case ASK_PASS:
        if (len > BOARD_PASS_SIZE - 1u)
        {
            S->ops->print("At most 64 characters.\r\n");
            S->ops->print(ASK_PASS_TEXT);
            return;
        }
        if (S->ops->set_ssid(S->new_ssid) != 0 || S->ops->set_pass(line) != 0)
        {
            S->ops->print("Could not store the network.\r\n");
        }
        else
        {
            (void)snprintf(text, sizeof text, "Network %s stored; joining it now.\r\n", S->new_ssid);
            S->ops->print(text);
            if (S->ops->wifi_join != NULL)
            {
                S->ops->wifi_join();
            }
        }
        memset(S->new_ssid, 0, sizeof S->new_ssid);
        (void)ask_node_or_end();
        return;
    case ASK_NODE:
        if (len == 0u)
        {
            S->ops->print("Skipped; NODE <n> sets it later.\r\n");
            S->ask = ASK_NONE;
            return;
        }
        if (board_parse_node(line, &node) != 0)
        {
            S->ops->print("A number from 1 to 65535.\r\n");
            S->ops->print(ASK_NODE_TEXT);
            return;
        }
        if (S->ops->set_node(node) != 0)
        {
            S->ops->print("Could not store the node number.\r\n");
        }
        else
        {
            (void)snprintf(text, sizeof text,
                           "Node number %u stored. RESET starts the link with it.\r\n",
                           (unsigned)node);
            S->ops->print(text);
        }
        S->ask = ASK_NONE;
        return;
    case ASK_NONE:
    default:
        return;
    }
}

void board_wifi_scan(void (*print)(const char *text))
{
    char text[96];
    int16_t n;

    if (print == NULL)
    {
        return;
    }
    if (WiFi.getMode() == WIFI_OFF)
    {
        (void)WiFi.mode(WIFI_STA);   /* a scan needs the radio on as a station */
    }
    print("Scanning (about 3 s)...\r\n");
    n = WiFi.scanNetworks(false, true);
    if (n <= 0)
    {
        (void)snprintf(text, sizeof text, "No networks found (%d).\r\n", (int)n);
        print(text);
        WiFi.scanDelete();
        return;
    }
    for (int16_t i = 0; i < n; i++)
    {
        (void)snprintf(text, sizeof text, "  %-32s ch %2d  %4d dBm  %s\r\n",
                       WiFi.SSID((uint8_t)i).c_str(), (int)WiFi.channel((uint8_t)i),
                       (int)WiFi.RSSI((uint8_t)i),
                       (WiFi.encryptionType((uint8_t)i) == WIFI_AUTH_OPEN) ? "open" : "secured");
        print(text);
    }
    WiFi.scanDelete();
}

void board_wifi_show(void (*print)(const char *text))
{
    char ssid[BOARD_SSID_SIZE];
    char pass[BOARD_PASS_SIZE];
    char text[BOARD_SSID_SIZE + BOARD_PASS_SIZE + 48];

    if (print == NULL)
    {
        return;
    }
    if (board_wifi(ssid, pass) < 0)
    {
        print("No WiFi network stored.\r\n");
        return;
    }
    (void)snprintf(text, sizeof text, "Network: %s\r\nPassword: %s\r\n", ssid,
                   (pass[0] != '\0') ? pass : "(none: open network)");
    print(text);
    memset(pass, 0, sizeof pass);
    memset(text, 0, sizeof text);
}
