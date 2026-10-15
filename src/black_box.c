/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Black box: a small record of keyboard events that survives power loss.
 *
 * Events go into a RAM ring kept in __noinit memory, so ones not yet saved also survive a crash or
 * watchdog reset. A dedicated work queue saves them to flash through the settings subsystem in
 * fixed-size blocks that rotate through a few slots, at most once per SAVE_MIN_SECONDS. A 'D' '1'
 * frame from the configurator prints the saved blocks, oldest first, then any unsaved events.
 *
 * Flash is written only from the work queue, never from the recording path, so recording stays
 * safe from any context.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include <zmk/activity.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/rpc.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/events/battery_state_changed.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/events/split_peripheral_status_changed.h>
#endif

#include <zmk_studio_log/diag.h>
#include <zmk_studio_log/studio_log.h>

#include "internal.h"

#define RING_LEN CONFIG_ZMK_STUDIO_LOG_BLACK_BOX_RING
#define BLOCK_LEN CONFIG_ZMK_STUDIO_LOG_BLACK_BOX_BLOCK
#define SLOTS CONFIG_ZMK_STUDIO_LOG_BLACK_BOX_SLOTS
#define SAVE_MIN_MS (CONFIG_ZMK_STUDIO_LOG_BLACK_BOX_SAVE_MIN_SECONDS * 1000)
#define SAVE_IDLE_MS (CONFIG_ZMK_STUDIO_LOG_BLACK_BOX_SAVE_IDLE_SECONDS * 1000)

#define SETTINGS_ROOT "zdiag"
#define SETTINGS_BLOCKS SETTINGS_ROOT "/b"

#define RING_MAGIC 0x5A534C52u  /* "ZSLR" */
#define BLOCK_MAGIC 0x5A534C4Bu /* "ZSLK" */
#define BLOCK_VERSION 1

#define DIAG_PREFIX_DUMP ((uint8_t)'D')
BUILD_ASSERT(ZMK_RPC_RAW_FRAME_PREFIX_VALID(DIAG_PREFIX_DUMP));

/* 16 bytes. Stored in flash as-is: only append fields at the end, with a new BLOCK_VERSION. */
struct diag_event {
    uint32_t uptime_ms;
    uint16_t boot;
    uint8_t type;
    uint8_t a;
    uint8_t b;
    uint8_t reserved[3];
    uint32_t c;
};
BUILD_ASSERT(sizeof(struct diag_event) == 16);

struct diag_block {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t journal; /* increases by one per saved block; orders the slots */
    uint32_t crc;     /* over everything after this field */
    struct diag_event ev[BLOCK_LEN];
};

/* Event number n lives in ev[n % RING_LEN]. [saved, head) are not in flash yet. */
static __noinit struct {
    uint32_t magic;
    uint32_t head;
    uint32_t saved;
    uint32_t check;
    struct diag_event ev[RING_LEN];
} ring;

static struct k_spinlock ring_lock;

static uint32_t ring_check(void) { return RING_MAGIC ^ ring.head ^ (ring.saved * 2654435761u); }

static bool settings_ready;
static uint32_t next_journal = 1;
static int64_t last_save_ms;
static uint32_t lost_events;

/* Saving and dumping use static buffers on their own thread: settings calls are stack hungry. */
static struct diag_block block_buf;
static K_MUTEX_DEFINE(flash_mutex);

K_THREAD_STACK_DEFINE(diag_stack, 1536);
static struct k_work_q diag_queue;

static void save_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_cb);
static void dump_work_cb(struct k_work *work);
static K_WORK_DEFINE(dump_work, dump_work_cb);

static uint16_t current_boot(void) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RESTART_DIAG)
    return (uint16_t)zmk_studio_log_boot_number();
#else
    return 0;
#endif
}

/* Covers the header fields before `crc` and every event. */
static uint32_t block_crc(const struct diag_block *blk) {
    const uint8_t *base = (const uint8_t *)blk;
    uint32_t crc =
        crc32_ieee(base + offsetof(struct diag_block, version),
                   offsetof(struct diag_block, crc) - offsetof(struct diag_block, version));
    return crc32_ieee_update(crc, base + offsetof(struct diag_block, ev), sizeof(blk->ev));
}

static bool block_valid(const struct diag_block *blk) {
    return blk->magic == BLOCK_MAGIC && blk->version == BLOCK_VERSION && blk->count <= BLOCK_LEN &&
           blk->crc == block_crc(blk);
}

/* When the next save may run, given how many events are waiting. */
static void schedule_save(uint32_t pending, bool urgent) {
    if (!settings_ready || pending == 0) {
        return;
    }

    int64_t wait;
    if (pending >= BLOCK_LEN || urgent) {
        int64_t since = k_uptime_get() - last_save_ms;
        wait = (last_save_ms == 0 || since >= SAVE_MIN_MS) ? 0 : SAVE_MIN_MS - since;
    } else {
        wait = SAVE_IDLE_MS;
    }

    /* Only ever bring a scheduled save forward. */
    if (!k_work_delayable_is_pending(&save_work) ||
        k_ticks_to_ms_floor64(k_work_delayable_remaining_get(&save_work)) > wait) {
        (void)k_work_reschedule_for_queue(&diag_queue, &save_work, K_MSEC(wait));
    }
}

void zmk_studio_log_diag_record(uint8_t type, uint8_t a, uint8_t b, uint32_t c) {
    struct diag_event ev = {
        .uptime_ms = k_uptime_get_32(),
        .boot = current_boot(),
        .type = type,
        .a = a,
        .b = b,
        .c = c,
    };
    uint32_t pending;

    k_spinlock_key_t key = k_spin_lock(&ring_lock);
    if (ring.head - ring.saved >= RING_LEN) {
        /* Flash is behind: drop the oldest unsaved event and count it. */
        ring.saved++;
        lost_events++;
    }
    ring.ev[ring.head % RING_LEN] = ev;
    ring.head++;
    ring.check = ring_check();
    pending = ring.head - ring.saved;
    k_spin_unlock(&ring_lock, key);

    schedule_save(pending, type == ZMK_DIAG_BOOT || type == ZMK_DIAG_FAULT);
}

static void save_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    char key[24];
    uint32_t first, count, lost;

    k_mutex_lock(&flash_mutex, K_FOREVER);

    k_spinlock_key_t lock = k_spin_lock(&ring_lock);
    first = ring.saved;
    count = MIN(ring.head - ring.saved, BLOCK_LEN);
    for (uint32_t i = 0; i < count; i++) {
        block_buf.ev[i] = ring.ev[(first + i) % RING_LEN];
    }
    lost = lost_events;
    lost_events = 0;
    k_spin_unlock(&ring_lock, lock);

    if (count == 0 && lost == 0) {
        k_mutex_unlock(&flash_mutex);
        return;
    }

    /* Record ring overflow in the block itself; the event it displaces is saved next time. */
    if (lost) {
        count = MIN(count, BLOCK_LEN - 1);
        block_buf.ev[count] = (struct diag_event){.uptime_ms = k_uptime_get_32(),
                                                  .boot = current_boot(),
                                                  .type = ZMK_DIAG_DROPPED,
                                                  .c = lost};
    }

    uint16_t stored = (uint16_t)(count + (lost ? 1 : 0));
    memset(&block_buf.ev[stored], 0, sizeof(block_buf.ev) - stored * sizeof(block_buf.ev[0]));
    block_buf.magic = BLOCK_MAGIC;
    block_buf.version = BLOCK_VERSION;
    block_buf.count = stored;
    block_buf.journal = next_journal;
    block_buf.crc = block_crc(&block_buf);

    snprintf(key, sizeof(key), SETTINGS_BLOCKS "/%u", (unsigned int)(next_journal % SLOTS));
    int err = settings_save_one(key, &block_buf, sizeof(block_buf));
    last_save_ms = k_uptime_get();

    uint32_t pending;
    lock = k_spin_lock(&ring_lock);
    if (err == 0) {
        next_journal++;
        /* If the ring overflowed during the write, `saved` already moved on; never move it back. */
        if ((int32_t)(first + count - ring.saved) > 0) {
            ring.saved = first + count;
        }
        ring.check = ring_check();
    } else {
        lost_events += lost;
    }
    pending = ring.head - ring.saved;
    k_spin_unlock(&ring_lock, lock);

    k_mutex_unlock(&flash_mutex);

    if (err) {
        printk("zmk_diag black box save failed err=%d\n", err);
        (void)k_work_reschedule_for_queue(&diag_queue, &save_work, K_MSEC(SAVE_MIN_MS));
    } else {
        schedule_save(pending, false);
    }
}

/* Only used here. Settings calls this with its own lock held, so it must not take flash_mutex:
 * the save and dump paths take flash_mutex first and then the settings lock. */
static struct diag_block load_buf;

/* Settings load: find the newest saved block so new ones continue after it. */
static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "b", &next) || !next || len != sizeof(load_buf)) {
        return 0;
    }
    if (read_cb(cb_arg, &load_buf, sizeof(load_buf)) == sizeof(load_buf) &&
        block_valid(&load_buf) && load_buf.journal >= next_journal) {
        next_journal = load_buf.journal + 1;
    }
    return 0;
}

static int settings_commit_cb(void) {
    settings_ready = true;
    uint32_t pending = ring.head - ring.saved;
    /* Unsaved events from before a restart, plus this boot's: save them now. */
    schedule_save(pending, true);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_studio_log_diag, SETTINGS_ROOT, NULL, settings_set_cb,
                               settings_commit_cb, NULL);

/* ---- dump ---- */

static uint32_t slot_journal[SLOTS];
static int dump_want_slot;
static bool dump_have_block;

static int dump_slot_number(const char *key) {
    unsigned int slot = 0;

    if (!key || !*key) {
        return -1;
    }
    for (const char *p = key; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        slot = slot * 10 + (unsigned int)(*p - '0');
    }
    return slot < SLOTS ? (int)slot : -1;
}

/* Pass 1 (dump_want_slot < 0): note each slot's journal. Pass 2: load one slot into block_buf. */
static int dump_load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                        void *param) {
    ARG_UNUSED(param);
    int slot = dump_slot_number(key);

    if (slot < 0 || len != sizeof(block_buf)) {
        return 0;
    }
    if (dump_want_slot >= 0 && slot != dump_want_slot) {
        return 0;
    }
    if (read_cb(cb_arg, &block_buf, sizeof(block_buf)) != sizeof(block_buf) ||
        !block_valid(&block_buf)) {
        return 0;
    }
    if (dump_want_slot < 0) {
        slot_journal[slot] = block_buf.journal;
    } else {
        dump_have_block = true;
    }
    return 0;
}

/* The dump runs on its own thread, so it can wait for the transport instead of dropping lines. */
static int dump_send(const char *line) {
    for (int tries = 0; tries < 200; tries++) {
        int err = zmk_studio_log_send_line(line);
        if (err != -EAGAIN) {
            return err;
        }
        k_msleep(10);
    }
    return -EAGAIN;
}

static const char *usb_state_name(uint8_t s) {
    static const char *const names[] = {"none", "powered", "hid"};
    return s < ARRAY_SIZE(names) ? names[s] : "?";
}

static void format_event(char *out, size_t size, const char *where, uint32_t journal,
                         const struct diag_event *ev) {
    char detail[64];
    int8_t profile = ev->a == 0xff ? -1 : (int8_t)ev->a;

    switch (ev->type) {
    case ZMK_DIAG_BOOT:
        snprintf(detail, sizeof(detail), "boot reset_cause=0x%08x", (unsigned int)ev->c);
        break;
    case ZMK_DIAG_FAULT:
        snprintf(detail, sizeof(detail), "crash reason=%u pc=0x%08x after=%us", ev->a,
                 (unsigned int)ev->c, ev->b);
        break;
    case ZMK_DIAG_FAULT_LR:
        snprintf(detail, sizeof(detail), "crash lr=0x%08x", (unsigned int)ev->c);
        break;
    case ZMK_DIAG_USB:
        snprintf(detail, sizeof(detail), "usb=%s", usb_state_name(ev->a));
        break;
    case ZMK_DIAG_ENDPOINT:
        snprintf(detail, sizeof(detail), "output=%s profile=%u",
                 ev->a == ZMK_TRANSPORT_USB ? "usb" : "ble", ev->b);
        break;
    case ZMK_DIAG_BLE_PROFILE:
        snprintf(detail, sizeof(detail), "ble_profile=%u", ev->a);
        break;
    case ZMK_DIAG_BLE_CONN:
        snprintf(detail, sizeof(detail), "ble_connect profile=%d role=%s addr=..%06x", profile,
                 ev->b ? "peripheral" : "central", (unsigned int)(ev->c & 0xffffff));
        break;
    case ZMK_DIAG_BLE_CONN_ERR:
        snprintf(detail, sizeof(detail), "ble_connect_failed profile=%d err=0x%02x addr=..%06x",
                 profile, ev->b, (unsigned int)(ev->c & 0xffffff));
        break;
    case ZMK_DIAG_BLE_DISC:
        snprintf(detail, sizeof(detail), "ble_disconnect profile=%d reason=0x%02x addr=..%06x",
                 profile, ev->b, (unsigned int)(ev->c & 0xffffff));
        break;
    case ZMK_DIAG_SPLIT:
        snprintf(detail, sizeof(detail), "split=%s", ev->a ? "connected" : "disconnected");
        break;
    case ZMK_DIAG_BATTERY:
        snprintf(detail, sizeof(detail), "battery=%u%%", ev->a);
        break;
    case ZMK_DIAG_ACTIVITY:
        snprintf(detail, sizeof(detail), "activity=%s",
                 ev->a == ZMK_ACTIVITY_ACTIVE ? "active"
                 : ev->a == ZMK_ACTIVITY_IDLE ? "idle"
                                              : "sleep");
        break;
    case ZMK_DIAG_DROPPED:
        snprintf(detail, sizeof(detail), "dropped=%u (ring overflowed)", (unsigned int)ev->c);
        break;
    default:
        snprintf(detail, sizeof(detail), "event=0x%02x a=%u b=%u c=0x%08x", ev->type, ev->a, ev->b,
                 (unsigned int)ev->c);
        break;
    }

    snprintf(out, size, "zmk_diag %s%u boot=%u t=%u.%03us %s", where, (unsigned int)journal,
             ev->boot, (unsigned int)(ev->uptime_ms / 1000), (unsigned int)(ev->uptime_ms % 1000),
             detail);
}

static void dump_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    char line[CONFIG_ZMK_STUDIO_LOG_LINE_MAX];
    uint32_t saved_events = 0, unsaved_events = 0;

    if (dump_send("zmk_diag dump begin") != 0) {
        return;
    }

    k_mutex_lock(&flash_mutex, K_FOREVER);

    memset(slot_journal, 0, sizeof(slot_journal));
    dump_want_slot = -1;
    (void)settings_load_subtree_direct(SETTINGS_BLOCKS, dump_load_cb, NULL);

    /* Print slots oldest first; journal numbers only increase. */
    uint32_t after = 0;
    for (;;) {
        int pick = -1;
        for (int s = 0; s < SLOTS; s++) {
            if (slot_journal[s] > after && (pick < 0 || slot_journal[s] < slot_journal[pick])) {
                pick = s;
            }
        }
        if (pick < 0) {
            break;
        }
        after = slot_journal[pick];

        dump_want_slot = pick;
        dump_have_block = false;
        (void)settings_load_subtree_direct(SETTINGS_BLOCKS, dump_load_cb, NULL);
        if (!dump_have_block || block_buf.journal != after) {
            continue;
        }
        for (uint16_t i = 0; i < block_buf.count; i++) {
            format_event(line, sizeof(line), "#", block_buf.journal, &block_buf.ev[i]);
            (void)dump_send(line);
            saved_events++;
        }
    }

    k_mutex_unlock(&flash_mutex);

    /* Events still only in RAM. */
    k_spinlock_key_t key = k_spin_lock(&ring_lock);
    uint32_t from = ring.saved, to = ring.head;
    k_spin_unlock(&ring_lock, key);

    for (uint32_t n = from; n != to; n++) {
        struct diag_event ev;

        key = k_spin_lock(&ring_lock);
        bool still_there = ring.head - n <= RING_LEN;
        ev = ring.ev[n % RING_LEN];
        k_spin_unlock(&ring_lock, key);

        if (!still_there) {
            continue;
        }
        format_event(line, sizeof(line), "unsaved#", n - from + 1, &ev);
        (void)dump_send(line);
        unsaved_events++;
    }

    snprintf(line, sizeof(line), "zmk_diag dump end saved=%u unsaved=%u",
             (unsigned int)saved_events, (unsigned int)unsaved_events);
    (void)dump_send(line);

    /* Follow the dump with the live log, as the configurator expects. */
    zmk_studio_log_set_streaming(true);
}

static void dump_frame_cb(const uint8_t *payload, size_t len) {
    if (len >= 2 && payload[1] == (uint8_t)'1') {
        (void)k_work_submit_to_queue(&diag_queue, &dump_work);
    }
}

ZMK_RPC_RAW_FRAME_HANDLER(zmk_studio_log_dump, DIAG_PREFIX_DUMP, dump_frame_cb);

/* ---- event sources ---- */

static int diag_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ep = as_zmk_endpoint_changed(eh);
    if (ep) {
        zmk_studio_log_diag_record(
            ZMK_DIAG_ENDPOINT, ep->endpoint.transport,
            ep->endpoint.transport == ZMK_TRANSPORT_BLE ? ep->endpoint.ble.profile_index : 0, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);
    if (act) {
        zmk_studio_log_diag_record(ZMK_DIAG_ACTIVITY, act->state, 0, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_BLE)
    const struct zmk_ble_active_profile_changed *prof = as_zmk_ble_active_profile_changed(eh);
    if (prof) {
        zmk_studio_log_diag_record(ZMK_DIAG_BLE_PROFILE, prof->index, 0, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);
    if (usb) {
        zmk_studio_log_diag_record(ZMK_DIAG_USB, usb->conn_state, 0, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    const struct zmk_battery_state_changed *bat = as_zmk_battery_state_changed(eh);
    if (bat) {
        zmk_studio_log_diag_record(ZMK_DIAG_BATTERY, bat->state_of_charge, 0, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    const struct zmk_split_peripheral_status_changed *split =
        as_zmk_split_peripheral_status_changed(eh);
    if (split) {
        zmk_studio_log_diag_record(ZMK_DIAG_SPLIT, split->connected, 0, 0);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_studio_log_diag, diag_listener);
ZMK_SUBSCRIPTION(zmk_studio_log_diag, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(zmk_studio_log_diag, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(zmk_studio_log_diag, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(zmk_studio_log_diag, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_SUBSCRIPTION(zmk_studio_log_diag, zmk_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
ZMK_SUBSCRIPTION(zmk_studio_log_diag, zmk_split_peripheral_status_changed);
#endif

#if IS_ENABLED(CONFIG_BT_CONN)
static uint8_t conn_profile(struct bt_conn *conn) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    int idx = zmk_ble_profile_index(bt_conn_get_dst(conn));
    return idx >= 0 ? (uint8_t)idx : 0xff;
#else
    return 0xff;
#endif
}

static uint32_t conn_addr(struct bt_conn *conn) {
    const bt_addr_le_t *addr = bt_conn_get_dst(conn);
    return addr ? sys_get_le24(addr->a.val) : 0;
}

static void on_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        zmk_studio_log_diag_record(ZMK_DIAG_BLE_CONN_ERR, conn_profile(conn), err, conn_addr(conn));
        return;
    }

    struct bt_conn_info info;
    uint8_t role = 0;
    if (bt_conn_get_info(conn, &info) == 0) {
        role = info.role == BT_CONN_ROLE_PERIPHERAL;
    }
    zmk_studio_log_diag_record(ZMK_DIAG_BLE_CONN, conn_profile(conn), role, conn_addr(conn));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    zmk_studio_log_diag_record(ZMK_DIAG_BLE_DISC, conn_profile(conn), reason, conn_addr(conn));
}

BT_CONN_CB_DEFINE(zmk_studio_log_diag_conn) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};
#endif

/* ---- init ---- */

/* Before anything can record: keep a retained ring, or start an empty one. */
static int ring_init(void) {
    if (ring.magic != RING_MAGIC || ring.check != ring_check() ||
        ring.head - ring.saved > RING_LEN) {
        memset(&ring, 0, sizeof(ring));
        ring.magic = RING_MAGIC;
        ring.check = ring_check();
    }
    return 0;
}

SYS_INIT(ring_init, PRE_KERNEL_1, 0);

static int diag_queue_init(void) {
    k_work_queue_start(&diag_queue, diag_stack, K_THREAD_STACK_SIZEOF(diag_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
    k_thread_name_set(&diag_queue.thread, "zmk_diag");
    return 0;
}

/* Before restart diagnostics record the boot event (APPLICATION 1). */
SYS_INIT(diag_queue_init, APPLICATION, 0);
