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
    // 2026-11-28 (D-136 T7-4a) tap, then touch again and move (or hold): the one finger tap's
    // key position stays pressed until the finger lifts (CONFIG_MCK_IQS5XX_TAP_DRAG).
    bool tap_drag;
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

// 2026-11-28 (D-136 T7-1) CONFIG_MCK_IQS5XX_IC_TUNING: values of the IC itself (IQS5xx
// registers, datasheet Rev 2.1 5.6 / 5.9 / 6.1), mainly for slow, fine moves.
struct mck_trackpad_ic {
    // A touch that stays within this many pixels is stationary (0x0672); the IC then runs
    // at the idle touch rate.
    uint8_t stationary_threshold;
    // Dynamic XY filter: the damping (beta / 256) below the lower speed (0x0637), and that
    // speed in pixels per cycle (0x0638).
    uint8_t bottom_beta;
    uint8_t lower_speed;
    // Cycle while a touch is stationary (ms, 0x057C).
    uint16_t idle_touch_ms;
    // A one / two finger tap must be released within this (ms, 0x06B9).
    uint16_t tap_time_ms;
};

// The values in use. Until changed, those the IC had after the driver's setup (the module's
// own values, or the devicetree's bottom-beta / stationary-threshold).
void mck_trackpad_get_ic(struct mck_trackpad_ic *ic);

// Those values from the IC (false until the trackpad has been set up).
bool mck_trackpad_get_default_ic(struct mck_trackpad_ic *ic);

// Use new values (clamped). They are written to the IC in its next communication window,
// i.e. on the next touch. Not saved.
void mck_trackpad_set_ic(const struct mck_trackpad_ic *ic);

// Use and save them a moment later (CONFIG_MCK_TRACKPAD_SETTINGS, "mck/tp/ic").
void mck_trackpad_set_and_save_ic(const struct mck_trackpad_ic *ic);

// Back to the IC's own values and forget the saved ones.
void mck_trackpad_reset_ic(void);
