/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Multiplex firmware log lines onto the ZMK Studio RPC transport as 'L' raw frames, so a
 * configurator gets keymap RPC and a live log over one connection.
 *
 * printk() and log backend output are assembled into lines under a spinlock (both can run from
 * any thread or ISR), kept in a small history ring, and, while the host has the stream on, queued
 * for a work item that sends them without ever blocking on the transport.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/printk.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_PRINTK)
#include <zephyr/sys/printk-hooks.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BACKEND)
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>
#endif

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/rpc.h>

#include <zmk_studio_log/studio_log.h>

#include "internal.h"

#ifndef ZMK_RPC_RAW_FRAME_HANDLER
#error                                                                                             \
    "zmk-studio-log needs a ZMK with Studio raw frames; apply patches/0001-studio-raw-frames.patch"
#endif

BUILD_ASSERT(ZMK_RPC_RAW_FRAME_PREFIX_VALID(ZMK_STUDIO_LOG_PREFIX_LINE));

#define LINE_MAX CONFIG_ZMK_STUDIO_LOG_LINE_MAX
#define HISTORY_LEN CONFIG_ZMK_STUDIO_LOG_HISTORY
/* Lines sent per work run; keeps the system work queue responsive. */
#define SEND_BURST 4

static struct k_spinlock line_lock;
static char assembling[LINE_MAX];
static size_t assembling_len;

#if HISTORY_LEN > 0
#define HISTORY_MAGIC 0x5A534C48u /* "ZSLH" */

/* Line number n (counting every committed line) lives in lines[n % HISTORY_LEN] until it is
 * overwritten; next is the number the next committed line gets. */
struct history_store {
    uint32_t magic;
    uint32_t next;
    uint32_t next_check; /* ~next; a mismatch means RAM was not retained */
    char lines[HISTORY_LEN][LINE_MAX];
};

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RETAIN_HISTORY)
/* Not cleared at boot, so the lines before a crash or watchdog reset are still here. */
static __noinit struct history_store hist;
#else
static struct history_store hist;
#endif

/* Replay sends lines [replay_next, replay_end), oldest first. */
static uint32_t replay_next;
static uint32_t replay_end;
#endif

K_MSGQ_DEFINE(studio_log_q, LINE_MAX, CONFIG_ZMK_STUDIO_LOG_QUEUE, 1);

static atomic_t streaming;
static atomic_t panicked;

static void flush_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(flush_work, flush_work_cb);

static bool transport_allowed(void) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_USB_ONLY)
    return zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
#else
    return true;
#endif
}

bool zmk_studio_log_is_streaming(void) { return atomic_get(&streaming) && transport_allowed(); }

static int send_prefixed(uint8_t prefix, const void *data, size_t len) {
    uint8_t payload[LINE_MAX + 1];

    if (len > LINE_MAX) {
        return -EMSGSIZE;
    }
    if (!transport_allowed()) {
        return -EAGAIN;
    }
    payload[0] = prefix;
    memcpy(&payload[1], data, len);
    int ret = zmk_rpc_send_raw_frame(payload, len + 1);
    return ret == -ENOTCONN ? -EAGAIN : ret;
}

static int send_line(const char *line) {
    return send_prefixed(ZMK_STUDIO_LOG_PREFIX_LINE, line, strnlen(line, LINE_MAX));
}

int zmk_studio_log_send_line(const char *line) {
    if (!line) {
        return -EINVAL;
    }
    if (strnlen(line, LINE_MAX + 1) > LINE_MAX) {
        return -EMSGSIZE;
    }
    return send_line(line);
}

int zmk_studio_log_send_frame(uint8_t prefix, const void *data, size_t len) {
    if (!ZMK_RPC_RAW_FRAME_PREFIX_VALID(prefix) || prefix == ZMK_STUDIO_LOG_PREFIX_LINE ||
        prefix == ZMK_STUDIO_LOG_PREFIX_CONTROL || (len > 0 && !data)) {
        return -EINVAL;
    }
    return send_prefixed(prefix, data, len);
}

/* Called with line_lock held. Never blocks: may run in an ISR or the fatal error path. */
static void commit_line(void) {
    if (assembling_len == 0) {
        return;
    }
    assembling[assembling_len] = '\0';
    assembling_len = 0;

#if HISTORY_LEN > 0
    memcpy(hist.lines[hist.next % HISTORY_LEN], assembling, LINE_MAX);
    hist.next++;
    hist.next_check = ~hist.next;
#endif

    if (!atomic_get(&streaming) || atomic_get(&panicked)) {
        return;
    }
    /* A full queue drops the line: logging must never stall the caller. */
    (void)k_msgq_put(&studio_log_q, assembling, K_NO_WAIT);
    (void)k_work_schedule(&flush_work, K_NO_WAIT);
}

static void feed_char(char c) {
    if (c == '\r') {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&line_lock);

    if (c == '\n') {
        commit_line();
    } else {
        if (assembling_len >= LINE_MAX - 1) {
            commit_line();
        }
        assembling[assembling_len++] = c;
    }

    k_spin_unlock(&line_lock, key);
}

#if HISTORY_LEN > 0
/* Copies the next history line to replay into `out`. Returns false when replay is done. */
static bool next_replay_line(char *out) {
    bool more = false;
    k_spinlock_key_t key = k_spin_lock(&line_lock);

    /* Lines committed since replay began may have overwritten the oldest ones; skip those. */
    uint32_t oldest = hist.next > HISTORY_LEN ? hist.next - HISTORY_LEN : 0;
    if (replay_next < oldest) {
        replay_next = oldest;
    }
    if (replay_next < replay_end) {
        memcpy(out, hist.lines[replay_next % HISTORY_LEN], LINE_MAX);
        more = true;
    }

    k_spin_unlock(&line_lock, key);
    return more;
}
#endif

static void flush_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    char line[LINE_MAX];

    if (!atomic_get(&streaming)) {
        k_msgq_purge(&studio_log_q);
        return;
    }

    if (!transport_allowed()) {
        atomic_clear(&streaming);
        k_msgq_purge(&studio_log_q);
        return;
    }

    for (int sent = 0; sent < SEND_BURST; sent++) {
        int err;

#if HISTORY_LEN > 0
        if (next_replay_line(line)) {
            err = send_line(line);
            if (err == -EAGAIN) {
                break;
            }
            k_spinlock_key_t key = k_spin_lock(&line_lock);
            replay_next++;
            k_spin_unlock(&line_lock, key);
            continue;
        }
#endif

        if (k_msgq_peek(&studio_log_q, line) != 0) {
            return;
        }
        err = send_line(line);
        if (err == -EAGAIN) {
            break;
        }
        (void)k_msgq_get(&studio_log_q, line, K_NO_WAIT);
    }

    /* More to send, or the transport was busy: come back shortly. */
    (void)k_work_schedule(&flush_work, K_MSEC(20));
}

static void set_streaming(bool on) {
    if (!on) {
        (void)send_line("zmk_studio_log off");
        atomic_clear(&streaming);
        k_msgq_purge(&studio_log_q);
        return;
    }

    /* The flags tell the configurator which optional features this firmware has. */
    char hello[96];
    snprintk(hello, sizeof(hello),
             "zmk_studio_log v%d history=%d line_max=%d retained=%d blackbox=%d",
             ZMK_STUDIO_LOG_PROTOCOL_VERSION, HISTORY_LEN, LINE_MAX,
             IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RETAIN_HISTORY),
             IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BLACK_BOX));
    (void)send_line(hello);

    k_spinlock_key_t key = k_spin_lock(&line_lock);
#if HISTORY_LEN > 0
    replay_end = hist.next;
    replay_next = hist.next > HISTORY_LEN ? hist.next - HISTORY_LEN : 0;
#endif
    /* Lines committed from here on are queued; older ones come from the replay. */
    k_msgq_purge(&studio_log_q);
    atomic_set(&streaming, 1);
    k_spin_unlock(&line_lock, key);

    (void)k_work_schedule(&flush_work, K_NO_WAIT);
}

void zmk_studio_log_start_streaming(void) {
    if (transport_allowed()) {
        set_streaming(true);
    }
}

static void control_frame_cb(const uint8_t *payload, size_t len) {
    if (len < 2) {
        return;
    }
    if (payload[1] == (uint8_t)'1') {
        if (transport_allowed()) {
            set_streaming(true);
        }
    } else if (payload[1] == (uint8_t)'0') {
        set_streaming(false);
    }
}

ZMK_RPC_RAW_FRAME_HANDLER(zmk_studio_log_control, ZMK_STUDIO_LOG_PREFIX_CONTROL, control_frame_cb);

static int endpoint_listener(const zmk_event_t *eh) {
    /* The host must ask again after moving to another transport. */
    if (!transport_allowed()) {
        atomic_clear(&streaming);
        (void)k_work_schedule(&flush_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_studio_log, endpoint_listener);
ZMK_SUBSCRIPTION(zmk_studio_log, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_PRINTK)
static int printk_hook(int c) {
    feed_char((char)c);
    return c;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BACKEND)
static uint8_t log_output_buf[32];

static int log_char_out(uint8_t *data, size_t length, void *ctx) {
    ARG_UNUSED(ctx);
    for (size_t i = 0; i < length; i++) {
        feed_char((char)data[i]);
    }
    return (int)length;
}

LOG_OUTPUT_DEFINE(studio_log_output, log_char_out, log_output_buf, sizeof(log_output_buf));

static void backend_process(const struct log_backend *const backend, union log_msg_generic *msg) {
    ARG_UNUSED(backend);
    log_output_msg_process(&studio_log_output, &msg->log,
                           LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP);
}

static void backend_panic(const struct log_backend *const backend) {
    ARG_UNUSED(backend);
    /* After a panic, lines still go to history but nothing is scheduled. */
    atomic_set(&panicked, 1);
    log_output_flush(&studio_log_output);
}

static void backend_dropped(const struct log_backend *const backend, uint32_t cnt) {
    ARG_UNUSED(backend);
    log_output_dropped_process(&studio_log_output, cnt);
}

static const struct log_backend_api studio_log_backend_api = {
    .process = backend_process,
    .panic = backend_panic,
    .dropped = IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE) ? NULL : backend_dropped,
};

LOG_BACKEND_DEFINE(zmk_studio_log_backend, studio_log_backend_api, true);
#endif

#if HISTORY_LEN > 0
/* Runs before anything can log, so a retained history is checked before it is appended to. */
static int history_init(void) {
    if (hist.magic == HISTORY_MAGIC && hist.next_check == ~hist.next) {
        /* Retained across a restart. Re-terminate every slot in case one was mid-write. */
        for (size_t i = 0; i < HISTORY_LEN; i++) {
            hist.lines[i][LINE_MAX - 1] = '\0';
        }
        return 0;
    }

    memset(&hist, 0, sizeof(hist));
    hist.magic = HISTORY_MAGIC;
    hist.next_check = ~hist.next;
    return 0;
}

SYS_INIT(history_init, PRE_KERNEL_1, 0);
#endif

static int studio_log_init(void) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_PRINTK)
    __printk_hook_install(printk_hook);
#endif
    return 0;
}

/* APPLICATION level so this hook replaces any console hook installed earlier in boot. */
SYS_INIT(studio_log_init, APPLICATION, 0);
