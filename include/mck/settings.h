/*
 * MCK runtime settings: battery type and idle / sleep times.
 * Kept in flash on each half; the split central sends changes to the
 * peripherals (CONFIG_MCK_SETTINGS_SPLIT_RELAY).
 *
 * Copyright (c) 2026 mare-cycles
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum mck_battery_type {
    MCK_BATTERY_TYPE_NIMH = 0,
    MCK_BATTERY_TYPE_ALKALINE = 1,
};

struct mck_settings_values {
    bool has_power;
    uint32_t idle_ms;
    uint32_t sleep_ms;
    bool has_battery_type;
    enum mck_battery_type battery_type;
};

/** Battery type for the percent curve (NiMH when settings are off). */
enum mck_battery_type mck_battery_type_get(void);

/** Fills the values in effect; has_* say what this build supports. */
void mck_settings_get(struct mck_settings_values *out);

/**
 * Applies the has_* parts of `values` on this half, saves them, and (on the
 * split central) sends them to the peripherals.
 */
int mck_settings_set(const struct mck_settings_values *values);
