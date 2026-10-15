/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Turn the live stream on, as a 'C' '1' frame would. Used after a black box dump. */
void zmk_studio_log_start_streaming(void);

/* Boots since the last power loss, counting from 1; 0 if unknown. Needs RESTART_DIAG. */
uint32_t zmk_studio_log_boot_number(void);
