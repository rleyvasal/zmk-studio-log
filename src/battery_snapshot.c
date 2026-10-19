/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Battery snapshots for a host battery app, on the Studio connection: a 'K'
 * frame every CONFIG_ZMK_STUDIO_LOG_BATTERY_INTERVAL_SEC, and one right away
 * when the host sends 'G' '1', so an app can open the port, ask, and close.
 *
 * Works on upstream ZMK. A ZMK with CONFIG_ZMK_BATTERY_CHARGE_STATE_API also
 * reports the cell level while charging and "full"; one with
 * CONFIG_ZMK_BATTERY_CHARGE_STATUS also the peripheral's charge state.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/studio/rpc.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#include <zmk_studio_log/battery.h>
#include <zmk_studio_log/studio_log.h>

#define HAS_PERIPHERAL                                                                             \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                                                  \
     IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING))

static int format_half(char *buf, size_t cap, const struct zmk_studio_log_battery_half *half) {
    if (half->level < 0) {
        return snprintk(buf, cap, "%s", half->powered ? "usb" : "--");
    }
    if (!half->powered) {
        return snprintk(buf, cap, "%d", half->level);
    }
    return snprintk(buf, cap, half->full ? "%d usb full" : "%d usb", half->level);
}

int zmk_studio_log_battery_format(char *buf, size_t cap,
                                  const struct zmk_studio_log_battery_half *left,
                                  const struct zmk_studio_log_battery_half *right) {
    char l[24], r[24];

    if (!buf || cap == 0 || !left) {
        return -EINVAL;
    }
    format_half(l, sizeof(l), left);
    if (right) {
        format_half(r, sizeof(r), right);
    }

    char mv[16] = "";

    if (left->millivolts > 0) {
        snprintk(mv, sizeof(mv), " mv=%d", left->millivolts);
    }
    if (!right) {
        return snprintk(buf, cap, "left=%s%s", l, mv);
    }
    return snprintk(buf, cap, "left=%s right=%s%s", l, r, mv);
}

#if HAS_PERIPHERAL
static struct zmk_studio_log_battery_half peripheral = {.level = -1};
#endif

static struct zmk_studio_log_battery_half local_half(void) {
    struct zmk_studio_log_battery_half half = {0};

#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATE_API)
    enum zmk_battery_charge_state state = zmk_battery_charge_state();

    half.powered = state != ZMK_BATTERY_CHARGE_STATE_DISCHARGING;
    half.full = state == ZMK_BATTERY_CHARGE_STATE_FULL;
    half.level = zmk_battery_reported_soc();
    if (half.powered && zmk_battery_last_cell_soc() < 0) {
        half.level = -1;
    }
    /* ZMK Next exposes the last ADC reading next to the charge state API. */
    half.millivolts = zmk_battery_millivolts();
#else
    /* Without ZMK's cell tracking, a reading on USB is only charge voltage. */
#if IS_ENABLED(CONFIG_ZMK_USB)
    half.powered = zmk_usb_is_powered();
#endif
    half.level = half.powered ? -1 : zmk_battery_state_of_charge();
#endif
    return half;
}

int zmk_studio_log_battery_snapshot(char *buf, size_t cap) {
    struct zmk_studio_log_battery_half left = local_half();

#if HAS_PERIPHERAL
    return zmk_studio_log_battery_format(buf, cap, &left, &peripheral);
#else
    return zmk_studio_log_battery_format(buf, cap, &left, NULL);
#endif
}

static void send_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_work, send_work_cb);

static void send_work_cb(struct k_work *work) {
    char line[48];
    int len = zmk_studio_log_battery_snapshot(line, sizeof(line));

    /* -EAGAIN: no host on the Studio port right now; try again next interval. */
    if (len > 0) {
        (void)zmk_studio_log_send_frame(ZMK_STUDIO_LOG_PREFIX_BATTERY, line,
                                        MIN((size_t)len, sizeof(line) - 1));
    }
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_SECONDS(CONFIG_ZMK_STUDIO_LOG_BATTERY_INTERVAL_SEC));
}

void zmk_studio_log_battery_send_now(void) { k_work_reschedule(&send_work, K_NO_WAIT); }

static void battery_request_cb(const uint8_t *payload, size_t len) {
    if (len >= 2 && payload[1] == (uint8_t)'1') {
        zmk_studio_log_battery_send_now();
    }
}

ZMK_RPC_RAW_FRAME_HANDLER(zmk_studio_log_battery_request, ZMK_STUDIO_LOG_PREFIX_BATTERY_REQUEST,
                          battery_request_cb);

#if HAS_PERIPHERAL
static int peripheral_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *level =
        as_zmk_peripheral_battery_state_changed(eh);

    if (level != NULL) {
        if (level->source == 0) {
            peripheral.level = level->state_of_charge;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
    const struct zmk_peripheral_battery_charging_changed *charge =
        as_zmk_peripheral_battery_charging_changed(eh);

    if (charge != NULL && charge->source == 0) {
        peripheral.powered = charge->charging;
        peripheral.full = charge->full;
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_studio_log_battery, peripheral_listener);
ZMK_SUBSCRIPTION(zmk_studio_log_battery, zmk_peripheral_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BATTERY_CHARGE_STATUS)
ZMK_SUBSCRIPTION(zmk_studio_log_battery, zmk_peripheral_battery_charging_changed);
#endif
#endif // HAS_PERIPHERAL

static int battery_snapshot_init(void) {
    k_work_schedule(&send_work, K_SECONDS(2));
    return 0;
}

SYS_INIT(battery_snapshot_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
