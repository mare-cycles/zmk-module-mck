/*
 * Copyright (c) 2026 mare-cycles
 * SPDX-License-Identifier: MIT
 *
 * Trackpad motion settings of the mck,iqs5xx driver (CONFIG_MCK_IQS5XX_MOTION).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mck_trackpad_motion {
    // Average of the last N moves (1 = off, up to 9).
    uint8_t correction;
    // Acceleration strength (0 = off, 1-9).
    uint8_t accel;
    // Inertia length after lifting a quick move (0 = off, 1-9).
    uint8_t inertia_cursor;
    uint8_t inertia_scroll;
    // One finger scrolls instead of moving the cursor.
    bool scroll_only;
};

// The settings in use (the Kconfig defaults until changed).
void mck_trackpad_get_motion(struct mck_trackpad_motion *motion);

// Use new settings; out of range values are clamped. Not saved.
void mck_trackpad_set_motion(const struct mck_trackpad_motion *motion);

// &tp_scroll: while held (pressed = true .. false), scroll only is swapped.
void mck_trackpad_scroll_mode(bool pressed);
