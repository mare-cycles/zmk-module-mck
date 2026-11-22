/*
 * Copyright (c) 2026 mare-cycles
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/*
 * NiMH (eneloop) voltage-per-cell -> percent, as a piecewise-linear curve.
 * Provisional points (to be checked against a meter on real hardware):
 *
 *   >= 1300 mV : 100 %
 *      1200 mV :  50 %
 *      1100 mV :  10 %
 *   <= 1000 mV :   0 %
 */
static inline uint8_t mck_nimh_cell_mv_to_pct(uint16_t cell_mv) {
    if (cell_mv >= 1300) {
        return 100;
    }
    if (cell_mv >= 1200) {
        return (uint8_t)(50 + (cell_mv - 1200) * 50 / 100);
    }
    if (cell_mv >= 1100) {
        return (uint8_t)(10 + (cell_mv - 1100) * 40 / 100);
    }
    if (cell_mv > 1000) {
        return (uint8_t)((cell_mv - 1000) * 10 / 100);
    }
    return 0;
}

/* Battery pack voltage -> percent, for `cells` NiMH cells in series. */
static inline uint8_t mck_nimh_mv_to_pct(uint16_t pack_mv, uint8_t cells) {
    if (cells == 0) {
        cells = 1;
    }
    return mck_nimh_cell_mv_to_pct((uint16_t)(pack_mv / cells));
}

/*
 * Alkaline voltage-per-cell -> percent (light load, through the boost
 * converter). Provisional points, to be checked on real hardware:
 *
 *   >= 1500 mV : 100 %
 *      1400 mV :  75 %
 *      1300 mV :  45 %
 *      1200 mV :  20 %
 *      1100 mV :   5 %
 *   <= 1000 mV :   0 %
 */
static inline uint8_t mck_alkaline_cell_mv_to_pct(uint16_t cell_mv) {
    static const uint16_t mv[] = {1000, 1100, 1200, 1300, 1400, 1500};
    static const uint8_t pct[] = {0, 5, 20, 45, 75, 100};
    if (cell_mv <= mv[0]) {
        return 0;
    }
    for (int i = 1; i < (int)(sizeof(mv) / sizeof(mv[0])); i++) {
        if (cell_mv < mv[i]) {
            return (uint8_t)(pct[i - 1] + (cell_mv - mv[i - 1]) * (pct[i] - pct[i - 1]) /
                                              (mv[i] - mv[i - 1]));
        }
    }
    return 100;
}

static inline uint8_t mck_alkaline_mv_to_pct(uint16_t pack_mv, uint8_t cells) {
    if (cells == 0) {
        cells = 1;
    }
    return mck_alkaline_cell_mv_to_pct((uint16_t)(pack_mv / cells));
}
