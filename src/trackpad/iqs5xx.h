/*
 * Copyright (c) 2025 Mariano Uvalle
 * SPDX-License-Identifier: MIT
 *
 * Register map and driver state of the IQS5xx driver (iqs5xx.c). See LICENSE.iqs5xx.
 */

#pragma once

#include <zephyr/device.h>

#define IQS5XX_NUM_FINGERS 0x0011
#define IQS5XX_REL_X 0x0012          // 2 bytes.
#define IQS5XX_REL_Y 0x0014          // 2 bytes.
#define IQS5XX_ABS_X 0x0016          // 2 bytes.
#define IQS5XX_ABS_Y 0x0018          // 2 bytes.
#define IQS5XX_TOUCH_STRENGTH 0x001A // 2 bytes.
#define IQS5XX_TOUCH_AREA 0x001C
// MCK: Fingers 1-5 each have abs X (2), abs Y (2), touch strength (2) and area (1) from 0x0016.
#define IQS5XX_FINGER_DATA IQS5XX_ABS_X
#define IQS5XX_FINGER_SIZE 7
#define IQS5XX_MAX_FINGERS 5

#define IQS5XX_BOTTOM_BETA 0x0637
#define IQS5XX_STATIONARY_THRESH 0x0672
// MCK: 2026-11-28 (D-136 T7-1) more of the values for slow moves and taps.
#define IQS5XX_DYNAMIC_LOWER_SPEED 0x0638
#define IQS5XX_IDLE_TOUCH_RATE 0x057C // 2 bytes, ms.
#define IQS5XX_TAP_TIME 0x06B9        // 2 bytes, ms.

#define IQS5XX_END_COMM_WINDOW 0xEEEE

#define IQS5XX_SYSTEM_CONTROL_0 0x0431
// System Control 0 bits.
#define IQS5XX_ACK_RESET BIT(7)
#define IQS5XX_AUTO_ATI BIT(5)
#define IQS5XX_ALP_RESEED BIT(4)
#define IQS5XX_RESEED BIT(3)

#define IQS5XX_SYSTEM_CONFIG_0 0x058E
// System Config 0 bits.
#define IQS5XX_MANUAL_CONTROL BIT(7)
#define IQS5XX_SETUP_COMPLETE BIT(6)
#define IQS5XX_WDT BIT(5)
#define IQS5XX_SW_INPUT_EVENT BIT(4)
#define IQS5XX_ALP_REATI BIT(3)
#define IQS5XX_REATI BIT(2)
#define IQS5XX_SW_INPUT_SELECT BIT(1)
#define IQS5XX_SW_INPUT BIT(0)

#define IQS5XX_SYSTEM_CONFIG_1 0x058F
// System Config 1 bits.
#define IQS5XX_EVENT_MODE BIT(0)
#define IQS5XX_GESTURE_EVENT BIT(1)
#define IQS5XX_TP_EVENT BIT(2)
#define IQS5XX_REATI_EVENT BIT(3)
#define IQS5XX_ALP_PROX_EVENT BIT(4)
#define IQS5XX_SNAP_EVENT BIT(5)
#define IQS5XX_TOUCH_EVENT BIT(6)
#define IQS5XX_PROX_EVENT BIT(7)

// Filter settings register.
#define IQS5XX_FILTER_SETTINGS 0x0632
// Filter settings bits.
#define IQS5XX_IIR_FILTER BIT(0)
#define IQS5XX_MAV_FILTER BIT(1)
#define IQS5XX_IIR_SELECT BIT(2)
#define IQS5XX_ALP_COUNT_FILTER BIT(3)

#define IQS5XX_SYSTEM_INFO_0 0x000F
// System Info 0 bits.
#define IQS5XX_SHOW_RESET BIT(7)
#define IQS5XX_ALP_REATI_OCCURRED BIT(6)
#define IQS5XX_ALP_ATI_ERROR BIT(5)
#define IQS5XX_REATI_OCCURRED BIT(4)
#define IQS5XX_ATI_ERROR BIT(3)

#define IQS5XX_SYSTEM_INFO_1 0x0010
// System Info 1 bits.
#define IQS5XX_SWITCH_STATE BIT(5)
#define IQS5XX_SNAP_TOGGLE BIT(4)
#define IQS5XX_RR_MISSED BIT(3)
#define IQS5XX_TOO_MANY_FINGERS BIT(2)
#define IQS5XX_PALM_DETECT BIT(1)
#define IQS5XX_TP_MOVEMENT BIT(0)

// These 2 registers have the same bit map.
// The first one configures the gestures,
// the second one reports gesture events at runtime.
#define IQS5XX_SINGLE_FINGER_GESTURES_CONF 0x06B7
#define IQS5XX_GESTURE_EVENTS_0 0x000D
// Single finger gesture identifiers.
#define IQS5XX_SINGLE_TAP BIT(0)
#define IQS5XX_PRESS_AND_HOLD BIT(1)
#define IQS5XX_SWIPE_LEFT BIT(2)
#define IQS5XX_SWIPE_RIGHT BIT(3)
#define IQS5XX_SWIPE_UP BIT(4)
#define IQS5XX_SWIPE_DOWN BIT(5)

// Time in ms, 2 registers wide.
// Hold time + tap time is used as
// a threshold for the press and
// hold gesture.
#define IQS5XX_HOLD_TIME 0x06BD
// TODO: Make hold time configurable with KConfig.

// Mouse button helpers.
#define LEFT_BUTTON_BIT BIT(0)
#define RIGHT_BUTTON_BIT BIT(1)
#define MIDDLE_BUTTON_BIT BIT(2)
#define LEFT_BUTTON_CODE INPUT_BTN_0
#define RIGHT_BUTTON_CODE INPUT_BTN_0 + 1
#define MIDDLE_BUTTON_CODE INPUT_BTN_0 + 2

// These 2 registers have the same bit map.
// The first one configures the gestures,
// the second one reports gesture events at runtime.
#define IQS5XX_MULTI_FINGER_GESTURES_CONF 0x06B8
#define IQS5XX_GESTURE_EVENTS_1 0x000E
// Multi finger gesture identifiers.
#define IQS5XX_TWO_FINGER_TAP BIT(0)
#define IQS5XX_SCROLL BIT(1)
#define IQS5XX_ZOOM BIT(2)

// Axes configuration.
#define IQS5XX_XY_CONFIG_0 0x0669
#define IQS5XX_FLIP_X BIT(0)
#define IQS5XX_FLIP_Y BIT(1)
#define IQS5XX_SWITCH_XY_AXIS BIT(2)

struct iqs5xx_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec rdy_gpio;
    struct gpio_dt_spec reset_gpio;

    // Gesture configuration.
    bool one_finger_tap;
    bool press_and_hold;
    bool two_finger_tap;
    uint16_t press_and_hold_time;

    // Scrolling configuration.
    bool scroll;
    bool natural_scroll_x;
    bool natural_scroll_y;

    // Axes configuration.
    bool switch_xy;
    bool flip_x;
    bool flip_y;

    // Sensitivity. configuration.
    // GRT65W: -1 means "not set in the devicetree, keep the module's value".
    int16_t bottom_beta;
    int16_t stationary_threshold;
    // MCK: 2026-11-28 (D-136 T7-2) -1 when not set, like the two above.
    int32_t idle_touch_rate_ms;
    int32_t tap_time_ms;

    // MCK: Key positions pressed by the gestures (gesture-positions, IQS5XX_GESTURE_*).
    // NULL / 0 when the property is not set.
    const uint32_t *gesture_positions;
    uint8_t gesture_positions_len;
    uint16_t gesture_release_ms;
};

// MCK: Indexes into gesture-positions (see dts/bindings/input/mck,iqs5xx.yaml).
#define IQS5XX_GESTURE_TAP_1 0
#define IQS5XX_GESTURE_TAP_2 1
#define IQS5XX_GESTURE_TAP_3 2
#define IQS5XX_GESTURE_TAP_4 3
#define IQS5XX_GESTURE_SWIPE_3_UP 4 // then right, down, left
#define IQS5XX_GESTURE_SWIPE_4_UP 8
#define IQS5XX_GESTURE_COUNT 12

struct iqs5xx_data {
    const struct device *dev;
    struct gpio_callback rdy_cb;
    struct k_work work;
    struct k_work_delayable button_release_work;
    // TODO: Pack flags into a bitfield to save space.
    bool initialized;
    // Flag to indicate if the button was pressed in a previous cycle.
    uint8_t buttons_pressed;
    bool active_hold;
    // Scroll accumulators.
    int16_t scroll_x_acc;
    int16_t scroll_y_acc;
    // GRT65W: Whether the register readback has been logged since the last setup.
    bool config_logged;
    // MCK: The key position a gesture holds pressed, or -1.
    int32_t pressed_position;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
    // MCK: The change count of the IC values (mck_trackpad_set_ic) last written to this IC.
    atomic_val_t ic_written;
#endif
#if IS_ENABLED(CONFIG_MCK_IQS5XX_KEEP_SLOW_MOVES)
    // MCK: TP_MOVEMENT of the last events of this touch, newest in bit 0.
    uint8_t movement_hist;
#endif
#if IS_ENABLED(CONFIG_MCK_IQS5XX_TOUCH_LOG)
    // MCK: One touch (from the first finger down to all fingers up), for the touch log.
    struct {
        bool active;
        int64_t start_ms;
        uint8_t fingers;
        uint8_t max_fingers;
        // Center of the fingers when the most fingers were first down, and its movement
        // while that many fingers stayed down (last and largest, per axis).
        int32_t ref_x, ref_y;
        int32_t move_x, move_y;
        int32_t peak_x, peak_y;
        uint16_t samples;
        uint8_t ic_events_0, ic_events_1;
    } touch;
#endif
#if IS_ENABLED(CONFIG_MCK_IQS5XX_GESTURES)
    // MCK: One touch, for the 3 / 4 finger gestures.
    struct {
        bool active;
        // A swipe was pressed, so nothing more (no tap either) until all fingers are up.
        bool done;
        int64_t start_ms;
        uint8_t max_fingers;
        // Each finger (the IC keeps a finger in its slot) in the last sample, and the movement
        // since the most fingers came down: per sample, the average movement of the fingers
        // that were down in both samples (and the largest axis of that movement so far).
        struct {
            bool down;
            uint16_t x, y;
        } last[IQS5XX_MAX_FINGERS];
        int32_t move_x, move_y;
        int32_t peak;
    } gest;
#endif
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)
    // MCK: Correction, acceleration and inertia (moves in 1/256 trackpad units).
    struct {
        bool touching;
        // The last moves, for the correction (average of the last N).
        int16_t hist_x[9], hist_y[9];
        uint8_t hist_len, hist_pos;
        // The part of a move below one unit, kept for the next report.
        int32_t rem_x, rem_y;
        // The last moves after processing, the ms each took (since the move before), for the
        // inertia speed, and what they were.
        int32_t vel_x[4], vel_y[4];
        uint16_t vel_dt[4];
        uint8_t vel_len, vel_pos;
        uint8_t kind;
        int64_t last_ms;
        // Inertia in progress: the speed per step, slowed down by in_keep / 256 each step.
        struct k_work_delayable inertia_work;
        uint8_t in_kind;
        uint16_t in_step, in_keep;
        int32_t in_vx, in_vy;
        int32_t in_rem_x, in_rem_y;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_LIFT_HOLD)
        // Slow cursor moves waiting a few events (1/256 units, and the event they came in),
        // dropped if the finger lifts first. Events of this one finger touch so far.
        int32_t hold_x[4], hold_y[4];
        uint32_t hold_ev[4];
        uint8_t hold_len;
        uint32_t ev_count;
#endif
    } motion;
#endif
};
