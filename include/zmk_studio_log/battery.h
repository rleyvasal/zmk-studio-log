/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 'K' frame: a battery snapshot as text. 'G' '1' from the host asks for one now. */
#define ZMK_STUDIO_LOG_PREFIX_BATTERY ((uint8_t)'K')
#define ZMK_STUDIO_LOG_PREFIX_BATTERY_REQUEST ((uint8_t)'G')

struct zmk_studio_log_battery_half {
    /* Percent, or -1 when not known (no reading yet, or only charge voltage). */
    int level;
    /* On external power: charging, or full. */
    bool powered;
    /* The charger reports the charge complete. */
    bool full;
    /* Last measured battery voltage in millivolts, or 0 when not known. Only this
     * (central) half's is known; a peripheral reports a level only. */
    int millivolts;
};

/*
 * Format a snapshot: "left=<half>" for this half and, on a split central,
 * " right=<half>" for the first peripheral (right is NULL otherwise). A half is
 * "78" on battery, "78 usb" while charging, "100 usb full" when full, "usb" on
 * power with no level known, and "--" when it has not reported. On a split, the
 * central is called left and peripheral 0 right, as on most splits.
 * When left's millivolts are known, " mv=<millivolts>" ends the line, for example
 * "left=78 usb right=15 mv=4123". Readers that only look for left= and right=
 * are unaffected.
 *
 * Returns the length written, or a negative error.
 */
int zmk_studio_log_battery_format(char *buf, size_t cap,
                                  const struct zmk_studio_log_battery_half *left,
                                  const struct zmk_studio_log_battery_half *right);

/* Format the current snapshot (what a 'K' frame carries). */
int zmk_studio_log_battery_snapshot(char *buf, size_t cap);

/* Send a 'K' frame now instead of at the next interval. */
void zmk_studio_log_battery_send_now(void);
