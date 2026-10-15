/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util_macro.h>

/* Black box event types. Values are stored in flash: append, never renumber. */
enum zmk_studio_log_diag_type {
    ZMK_DIAG_NONE = 0,
    ZMK_DIAG_BOOT = 1,         /* a: boot # (low byte), c: hwinfo reset cause flags */
    ZMK_DIAG_FAULT = 2,        /* a: k_fatal_error_reason, b: uptime s (low byte), c: pc */
    ZMK_DIAG_FAULT_LR = 3,     /* c: lr of the fault above */
    ZMK_DIAG_USB = 4,          /* a: enum zmk_usb_conn_state */
    ZMK_DIAG_ENDPOINT = 5,     /* a: enum zmk_transport, b: BLE profile */
    ZMK_DIAG_BLE_PROFILE = 6,  /* a: active profile */
    ZMK_DIAG_BLE_CONN = 7,     /* a: profile or 0xff, b: role (0 central, 1 peripheral), c: addr */
    ZMK_DIAG_BLE_CONN_ERR = 8, /* a: profile or 0xff, b: HCI error, c: addr */
    ZMK_DIAG_BLE_DISC = 9,     /* a: profile or 0xff, b: HCI reason, c: addr */
    ZMK_DIAG_SPLIT = 10,       /* a: 1 connected, 0 disconnected */
    ZMK_DIAG_BATTERY = 11,     /* a: state of charge % */
    ZMK_DIAG_ACTIVITY = 12,    /* a: enum zmk_activity_state */
    ZMK_DIAG_DROPPED = 13,     /* c: events lost because the RAM ring overflowed */

    /* Types from 0x80 up are free for keyboard-specific code. */
    ZMK_DIAG_CUSTOM_BASE = 0x80,
};

/* What the previous boot left behind, for code that keeps its own records (such as a
 * keyboard-specific journal). Valid from SYS_INIT(APPLICATION, 2) on. */
struct zmk_studio_log_fault {
    uint32_t reason; /* enum k_fatal_error_reason */
    uint32_t pc;
    uint32_t lr;
    uint32_t uptime_ms; /* when it crashed, in the previous boot */
    uint32_t thread;
    char thread_name[16];
};

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_RESTART_DIAG)
/** Boots since the last power loss, from 1; 0 if unknown. */
uint32_t zmk_studio_log_boot_number(void);

/**
 * The hwinfo reset cause flags (RESET_*) read at this boot. The module clears the hardware
 * register after reading it, so read it from here rather than from hwinfo.
 * @retval 0 on success, -ENOTSUP without ZMK_STUDIO_LOG_RESET_REASON, or the hwinfo error.
 */
int zmk_studio_log_reset_cause(uint32_t *cause);

/** Copies the crash recorded just before this boot into `out`; false if there was none. */
bool zmk_studio_log_boot_fault(struct zmk_studio_log_fault *out);
#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOG_BLACK_BOX)
/**
 * Add an event to the black box. Never blocks; safe from any thread or ISR (not from the fatal
 * error handler). `c` holds 32-bit values such as an address or program counter.
 */
void zmk_studio_log_diag_record(uint8_t type, uint8_t a, uint8_t b, uint32_t c);
#else
static inline void zmk_studio_log_diag_record(uint8_t type, uint8_t a, uint8_t b, uint32_t c) {
    (void)type;
    (void)a;
    (void)b;
    (void)c;
}
#endif
