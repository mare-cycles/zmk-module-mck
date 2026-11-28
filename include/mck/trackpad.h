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

// The values the firmware was built with (Kconfig).
void mck_trackpad_get_default_motion(struct mck_trackpad_motion *motion);

// &tp_scroll: while held (pressed = true .. false), scroll only is swapped.
void mck_trackpad_scroll_mode(bool pressed);

struct mck_trackpad_info {
    bool gestures;
    uint8_t gesture_position_count;
};

// What the first mck,iqs5xx trackpad does (zero when there is none).
void mck_trackpad_get_info(struct mck_trackpad_info *info);

// 2026-11-28 (D-136 T5) CONFIG_MCK_TRACKPAD_SETTINGS: kept in the Zephyr setting
// "mck/tp/motion". Use the settings and save them a moment later (once for many changes).
void mck_trackpad_set_and_save_motion(const struct mck_trackpad_motion *motion);

// Back to the Kconfig values and forget the saved ones.
void mck_trackpad_reset_motion(void);
