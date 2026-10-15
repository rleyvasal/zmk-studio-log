/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wire protocol, carried as Studio raw frames (see zmk_rpc_send_raw_frame()):
 *
 *   host -> device  'C' '1'          turn the log stream on (replays recent history first)
 *   host -> device  'C' '0'          turn it off
 *   device -> host  'L' <utf-8 text> one log line, no trailing newline
 *
 * The first line after 'C' '1' is "zmk_studio_log v<N> ..." so a host can tell that the
 * firmware supports the stream.
 */
#define ZMK_STUDIO_LOG_PROTOCOL_VERSION 1

#define ZMK_STUDIO_LOG_PREFIX_LINE ((uint8_t)'L')
#define ZMK_STUDIO_LOG_PREFIX_CONTROL ((uint8_t)'C')

/** True while the host has the stream turned on and the transport can carry it. */
bool zmk_studio_log_is_streaming(void);

/**
 * Send one line right away, bypassing the live queue. Unlike printk(), this works even while the
 * stream is off, as long as a configurator is connected, so it suits replies to host requests
 * such as a diagnostic dump.
 *
 * @retval 0 if queued for the host, -EAGAIN if the transport is busy or unavailable, -EMSGSIZE.
 */
int zmk_studio_log_send_line(const char *line);

/**
 * Send an application-defined raw frame on the Studio transport, for example a battery snapshot.
 * `prefix` must satisfy ZMK_RPC_RAW_FRAME_PREFIX_VALID() and must not be 'L' or 'C'.
 */
int zmk_studio_log_send_frame(uint8_t prefix, const void *data, size_t len);
