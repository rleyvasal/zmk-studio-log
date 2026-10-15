/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Boots since the last power loss, counting from 1; 0 if unknown. Needs RESTART_DIAG. */
uint32_t zmk_studio_log_boot_number(void);
