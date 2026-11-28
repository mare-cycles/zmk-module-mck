/*
 * Copyright (c) 2025 Mariano Uvalle
 * SPDX-License-Identifier: MIT
 *
 * Azoteq IQS5xx trackpad driver (devicetree compatible "mck,iqs5xx").
 *
 * Origin: AYM1607/zmk-driver-azoteq-iqs5xx 27321f0 (MIT, see LICENSE.iqs5xx next to this
 * file). Changes made for the GRT65W are marked "GRT65W:", changes made when the driver
 * moved into zmk-module-mck are marked "MCK:".
 *
 * MCK: 2026-11-25 (D-136 T1) moved here from mare-cycles/zmk-config-grt65w (drivers/input),
 * so that the trackpad features of MCK Aquarium can live next to it. Behavior is unchanged,
 * except that the status registers are read in one I2C transaction (MCK_IQS5XX_BLOCK_READ).
 *
 * MCK: 2026-11-25 (D-136 T2) the taps can press key positions (gesture-positions) instead of
 * reporting BTN_0 / BTN_1.
 *
 * MCK: 2026-11-26 (D-136 T3) 3 / 4 finger taps and swipes from the fingers' absolute data
 * (MCK_IQS5XX_GESTURES), and a touch log for tuning them (MCK_IQS5XX_TOUCH_LOG).
 *
 * MCK: 2026-11-26 (D-136 T4) correction, acceleration, inertia and scroll only
 * (MCK_IQS5XX_MOTION, off by default).
 *
 * MCK: 2026-11-28 (D-136 T7-1) some values of the IC itself (stationary threshold, dynamic
 * filter, idle touch rate, tap time) can be changed at runtime (MCK_IQS5XX_IC_TUNING); until
 * then the IC keeps its own.
 *
 * MCK: 2026-11-28 (D-136 T7-4a) tap and drag (MCK_IQS5XX_TAP_DRAG).
 */

#define DT_DRV_COMPAT mck_iqs5xx

#include <stdlib.h>
#include <string.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>

#include <mck/trackpad.h>

#include "iqs5xx.h"

LOG_MODULE_REGISTER(mck_iqs5xx, CONFIG_INPUT_LOG_LEVEL);

static int iqs5xx_setup_device(const struct device *dev);
static void iqs5xx_log_config(const struct device *dev);

// GRT65W: In a low-power mode the IQS5xx NACKs the first addressing attempt and ACKs a retry
// sent at least 150us later (datasheet 8.8.2). Retry once so a forced transaction does not fail.
static int iqs5xx_write_read_retry(const struct i2c_dt_spec *i2c, const uint8_t *wr, size_t wr_len,
                                   uint8_t *rd, size_t rd_len) {
    int ret = i2c_write_read_dt(i2c, wr, wr_len, rd, rd_len);
    if (ret < 0) {
        k_busy_wait(200);
        ret = i2c_write_read_dt(i2c, wr, wr_len, rd, rd_len);
    }
    return ret;
}

static int iqs5xx_write_retry(const struct i2c_dt_spec *i2c, const uint8_t *buf, size_t len) {
    int ret = i2c_write_dt(i2c, buf, len);
    if (ret < 0) {
        k_busy_wait(200);
        ret = i2c_write_dt(i2c, buf, len);
    }
    return ret;
}

// MCK: Read len consecutive registers starting at reg (the IQS5xx auto-increments the address).
static int iqs5xx_read_block(const struct device *dev, uint16_t reg, uint8_t *buf, size_t len) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};

    return iqs5xx_write_read_retry(&config->i2c, reg_buf, sizeof(reg_buf), buf, len);
}

// MCK: Unused when the status is read in one block (CONFIG_MCK_IQS5XX_BLOCK_READ).
static __maybe_unused int iqs5xx_read_reg16(const struct device *dev, uint16_t reg,
                                            uint16_t *val) {
    uint8_t buf[2];
    int ret = iqs5xx_read_block(dev, reg, buf, sizeof(buf));
    if (ret < 0) {
        return ret;
    }

    *val = (buf[0] << 8) | buf[1];
    return 0;
}

static int iqs5xx_write_reg16(const struct device *dev, uint16_t reg, uint16_t val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};

    return iqs5xx_write_retry(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_read_reg8(const struct device *dev, uint16_t reg, uint8_t *val) {
    return iqs5xx_read_block(dev, reg, val, 1);
}

static int iqs5xx_write_reg8(const struct device *dev, uint16_t reg, uint8_t val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[3] = {reg >> 8, reg & 0xFF, val};

    return iqs5xx_write_retry(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_end_comm_window(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[3] = {IQS5XX_END_COMM_WINDOW >> 8, IQS5XX_END_COMM_WINDOW & 0xFF, 0x00};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

// MCK: Gestures can press a key position (gesture-positions) instead of a button, so that what
// they do follows the active layer and can be changed in ZMK Studio. The press and the release
// are raised from the system work queue, like the kscan does for the real keys.
static void iqs5xx_raise_position(uint32_t position, bool pressed) {
    raise_zmk_position_state_changed(
        (struct zmk_position_state_changed){.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
                                            .state = pressed,
                                            .position = position,
                                            .timestamp = k_uptime_get()});
}

static void iqs5xx_release_position(struct iqs5xx_data *data) {
    if (data->pressed_position < 0) {
        return;
    }

    uint32_t position = data->pressed_position;
    data->pressed_position = -1;
    LOG_DBG("Releasing gesture position %u", position);
    iqs5xx_raise_position(position, false);
}

// MCK: Press the key position of a gesture (IQS5XX_GESTURE_*) and release it later.
// Returns false when the gesture has no position, so the caller can fall back to a button.
static bool iqs5xx_press_gesture(const struct device *dev, uint8_t gesture) {
    const struct iqs5xx_config *config = dev->config;
    struct iqs5xx_data *data = dev->data;

    if (gesture >= config->gesture_positions_len) {
        return false;
    }

    uint32_t position = config->gesture_positions[gesture];
    if (position >= ZMK_KEYMAP_LEN) {
        LOG_WRN("Gesture %d: position %u is not in the keymap (%d keys)", gesture, position,
                (int)ZMK_KEYMAP_LEN);
        return false;
    }

    // A gesture that comes before the last one was released releases it first.
    k_work_cancel_delayable(&data->button_release_work);
    iqs5xx_release_position(data);

    LOG_DBG("Gesture %d presses position %u", gesture, position);
    data->pressed_position = position;
    iqs5xx_raise_position(position, true);
    // The release work also releases a synthetic button that was still pressed.
    k_work_schedule(&data->button_release_work, K_MSEC(config->gesture_release_ms));
    return true;
}

static void iqs5xx_release_all(struct iqs5xx_data *data);

static void iqs5xx_button_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, button_release_work);

    iqs5xx_release_all(data);
}

// MCK: Release what a gesture pressed: its key position and the synthetic buttons.
static void iqs5xx_release_all(struct iqs5xx_data *data) {
    // MCK: Release the key position of a gesture.
    iqs5xx_release_position(data);

    // TODO: This loop should only deactivate one button.
    // Log a warning when that is not the case.
    for (int i = 0; i < 3; i++) {
        LOG_INF("Releasing synthetic button");
        if (data->buttons_pressed & BIT(i)) {
            input_report_key(data->dev, INPUT_BTN_0 + i, 0, true, K_FOREVER);
            // Turn off the bit.
            // NOTE: This is a potential race.
            data->buttons_pressed &= ~BIT(i);
        }
    }
}

// MCK: The registers the event handler needs, read either in one block or one by one.
struct iqs5xx_status {
    uint8_t gesture_events_0;
    uint8_t gesture_events_1;
    uint8_t sys_info_0;
    uint8_t sys_info_1;
    uint8_t num_fingers;
    int16_t rel_x;
    int16_t rel_y;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_FINGER_DATA)
    struct {
        uint16_t x;
        uint16_t y;
        uint16_t strength;
    } fingers[IQS5XX_MAX_FINGERS];
#endif
};

#if IS_ENABLED(CONFIG_MCK_IQS5XX_FINGER_DATA)
// MCK: Up to the area of finger 5 (0x0038).
#define IQS5XX_STATUS_END (IQS5XX_FINGER_DATA + IQS5XX_FINGER_SIZE * IQS5XX_MAX_FINGERS)
#else
#define IQS5XX_STATUS_END (IQS5XX_REL_Y + 2)
#endif

#if IS_ENABLED(CONFIG_MCK_IQS5XX_BLOCK_READ)
// MCK: 0x000D (gesture events 0) .. 0x0015 (relative Y low byte) are consecutive
// (with MCK_IQS5XX_FINGER_DATA, also the finger data up to 0x0038).
static int iqs5xx_read_status(const struct device *dev, struct iqs5xx_status *st) {
    uint8_t buf[IQS5XX_STATUS_END - IQS5XX_GESTURE_EVENTS_0];
    int ret = iqs5xx_read_block(dev, IQS5XX_GESTURE_EVENTS_0, buf, sizeof(buf));
    if (ret < 0) {
        LOG_ERR("Failed to read the status registers: %d", ret);
        return ret;
    }

#define IQS5XX_AT(reg) (buf[(reg) - IQS5XX_GESTURE_EVENTS_0])
    st->gesture_events_0 = IQS5XX_AT(IQS5XX_GESTURE_EVENTS_0);
    st->gesture_events_1 = IQS5XX_AT(IQS5XX_GESTURE_EVENTS_1);
    st->sys_info_0 = IQS5XX_AT(IQS5XX_SYSTEM_INFO_0);
    st->sys_info_1 = IQS5XX_AT(IQS5XX_SYSTEM_INFO_1);
    st->num_fingers = IQS5XX_AT(IQS5XX_NUM_FINGERS);
    st->rel_x = (int16_t)((IQS5XX_AT(IQS5XX_REL_X) << 8) | IQS5XX_AT(IQS5XX_REL_X + 1));
    st->rel_y = (int16_t)((IQS5XX_AT(IQS5XX_REL_Y) << 8) | IQS5XX_AT(IQS5XX_REL_Y + 1));
#if IS_ENABLED(CONFIG_MCK_IQS5XX_FINGER_DATA)
    for (int i = 0; i < IQS5XX_MAX_FINGERS; i++) {
        const uint16_t f = IQS5XX_FINGER_DATA + IQS5XX_FINGER_SIZE * i;
        st->fingers[i].x = (IQS5XX_AT(f) << 8) | IQS5XX_AT(f + 1);
        st->fingers[i].y = (IQS5XX_AT(f + 2) << 8) | IQS5XX_AT(f + 3);
        st->fingers[i].strength = (IQS5XX_AT(f + 4) << 8) | IQS5XX_AT(f + 5);
    }
#endif
#undef IQS5XX_AT
    return 0;
}
#else
// The original order of reads (one transaction per register).
static int iqs5xx_read_status(const struct device *dev, struct iqs5xx_status *st) {
    int ret = iqs5xx_read_reg8(dev, IQS5XX_SYSTEM_INFO_0, &st->sys_info_0);
    if (ret < 0) {
        LOG_ERR("Failed to read system info 0: %d", ret);
        return ret;
    }

    ret = iqs5xx_read_reg8(dev, IQS5XX_SYSTEM_INFO_1, &st->sys_info_1);
    if (ret < 0) {
        LOG_ERR("Failed to read system info 1: %d", ret);
        return ret;
    }

    ret = iqs5xx_read_reg8(dev, IQS5XX_GESTURE_EVENTS_0, &st->gesture_events_0);
    if (ret < 0) {
        LOG_ERR("Failed to read gesture events: %d", ret);
        return ret;
    }

    ret = iqs5xx_read_reg8(dev, IQS5XX_GESTURE_EVENTS_1, &st->gesture_events_1);
    if (ret < 0) {
        LOG_ERR("Failed to read gesture events 1: %d", ret);
        return ret;
    }

    st->num_fingers = 0;
    st->rel_x = 0;
    st->rel_y = 0;
    bool tp_movement = (st->sys_info_1 & IQS5XX_TP_MOVEMENT) != 0;
    bool scroll = (st->gesture_events_1 & IQS5XX_SCROLL) != 0;
    if (tp_movement || scroll) {
        ret = iqs5xx_read_reg16(dev, IQS5XX_REL_X, (uint16_t *)&st->rel_x);
        if (ret < 0) {
            LOG_ERR("Failed to read relative X: %d", ret);
            return ret;
        }

        ret = iqs5xx_read_reg16(dev, IQS5XX_REL_Y, (uint16_t *)&st->rel_y);
        if (ret < 0) {
            LOG_ERR("Failed to read relative Y: %d", ret);
            return ret;
        }
    }
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_MCK_IQS5XX_TOUCH_LOG)
// MCK: Follow one touch and log it (see CONFIG_MCK_IQS5XX_TOUCH_LOG). Fingers with a touch
// strength are counted; their center is the average of their absolute positions.
static void iqs5xx_touch_log(struct iqs5xx_data *data, const struct iqs5xx_status *st) {
    uint8_t count = 0;
    int32_t sum_x = 0, sum_y = 0;
    for (int i = 0; i < IQS5XX_MAX_FINGERS; i++) {
        if (st->fingers[i].strength > 0) {
            count++;
            sum_x += st->fingers[i].x;
            sum_y += st->fingers[i].y;
        }
    }

    int64_t now = k_uptime_get();
    if (!data->touch.active) {
        if (count == 0) {
            return;
        }
        memset(&data->touch, 0, sizeof(data->touch));
        data->touch.active = true;
        data->touch.start_ms = now;
    }

    data->touch.samples++;
    data->touch.ic_events_0 |= st->gesture_events_0;
    data->touch.ic_events_1 |= st->gesture_events_1;

    if (count != data->touch.fingers) {
        LOG_INF("tp: %u -> %u fingers (reg %u) at %d ms", data->touch.fingers, count,
                st->num_fingers, (int)(now - data->touch.start_ms));
        data->touch.fingers = count;
    }

    if (count > data->touch.max_fingers) {
        data->touch.max_fingers = count;
        data->touch.ref_x = sum_x / count;
        data->touch.ref_y = sum_y / count;
        data->touch.move_x = data->touch.move_y = 0;
        data->touch.peak_x = data->touch.peak_y = 0;
    } else if (count > 0 && count == data->touch.max_fingers) {
        data->touch.move_x = sum_x / count - data->touch.ref_x;
        data->touch.move_y = sum_y / count - data->touch.ref_y;
        data->touch.peak_x = MAX(data->touch.peak_x, abs(data->touch.move_x));
        data->touch.peak_y = MAX(data->touch.peak_y, abs(data->touch.move_y));
    }

    if (count == 0) {
        LOG_INF("tp: touch max %u fingers, %d ms, move (%d, %d), peak (%d, %d), at (%d, %d), "
                "%u samples, ic 0x%02X/0x%02X",
                data->touch.max_fingers, (int)(now - data->touch.start_ms), data->touch.move_x,
                data->touch.move_y, data->touch.peak_x, data->touch.peak_y, data->touch.ref_x,
                data->touch.ref_y, data->touch.samples, data->touch.ic_events_0,
                data->touch.ic_events_1);
        data->touch.active = false;
    }
}
#endif

#if IS_ENABLED(CONFIG_MCK_IQS5XX_GESTURES)
// MCK: 2026-11-26 (D-136 T3) 3 / 4 finger taps and swipes. The values come from a touch log of
// the GRT65W rev1 (taps: 170-350 ms, the fingers' center moves up to 30; swipes: 335 or more).
// Four fingers often flicker between 3 and 4 (every ~20 ms) and the center of all the fingers
// jumps when the count changes, so the movement follows each finger in its slot.
static bool iqs5xx_gestures_enabled(const struct iqs5xx_config *config) {
    return config->gesture_positions_len > IQS5XX_GESTURE_TAP_3;
}

// Returns true while the touch is a 3 / 4 finger one, so the caller reports nothing else.
static bool iqs5xx_gestures_update(const struct device *dev, const struct iqs5xx_status *st) {
    struct iqs5xx_data *data = dev->data;
    uint8_t count = 0;
    for (int i = 0; i < IQS5XX_MAX_FINGERS; i++) {
        if (st->fingers[i].strength > 0) {
            count++;
        }
    }

    int64_t now = k_uptime_get();
    if (!data->gest.active) {
        if (count == 0) {
            return false;
        }
        memset(&data->gest, 0, sizeof(data->gest));
        data->gest.active = true;
        data->gest.start_ms = now;
    }

    // Movement of the fingers that were down in the last sample too. A finger keeps its slot
    // while it stays down, so a finger coming and going (4 fingers flicker between 3 and 4
    // every ~20 ms) does not move the result, unlike the center of all the fingers.
    int32_t dx = 0, dy = 0, both = 0;
    for (int i = 0; i < IQS5XX_MAX_FINGERS; i++) {
        bool down = st->fingers[i].strength > 0;
        if (down && data->gest.last[i].down) {
            dx += (int32_t)st->fingers[i].x - data->gest.last[i].x;
            dy += (int32_t)st->fingers[i].y - data->gest.last[i].y;
            both++;
        }
        data->gest.last[i].down = down;
        data->gest.last[i].x = st->fingers[i].x;
        data->gest.last[i].y = st->fingers[i].y;
    }
    if (both > 0) {
        data->gest.move_x += dx / both;
        data->gest.move_y += dy / both;
        data->gest.peak =
            MAX(data->gest.peak, MAX(abs(data->gest.move_x), abs(data->gest.move_y)));
    }

    int32_t elapsed = (int32_t)(now - data->gest.start_ms);
    if (count > data->gest.max_fingers) {
        // Count the movement from when the most fingers came down (the 3rd / 4th finger
        // lands 15-80 ms after the first), so a 2 finger scroll does not count as a swipe.
        data->gest.max_fingers = count;
        data->gest.move_x = data->gest.move_y = 0;
        data->gest.peak = 0;
    }

    bool multi = data->gest.max_fingers >= 3;
    bool four = data->gest.max_fingers >= 4;

    // Swipe: once, when the center has moved far enough (the larger axis gives the direction).
    if (multi && count > 0 && !data->gest.done &&
        elapsed >= CONFIG_MCK_IQS5XX_GESTURE_SWIPE_SETTLE_MS &&
        elapsed <= CONFIG_MCK_IQS5XX_GESTURE_SWIPE_MAX_MS &&
        data->gest.peak >= CONFIG_MCK_IQS5XX_GESTURE_SWIPE_DIST) {
        int32_t mx = data->gest.move_x, my = data->gest.move_y;
        // 0: up (y decreases), 1: right, 2: down, 3: left.
        uint8_t dir = abs(mx) >= abs(my) ? (mx > 0 ? 1 : 3) : (my < 0 ? 0 : 2);
        uint8_t gesture = (four ? IQS5XX_GESTURE_SWIPE_4_UP : IQS5XX_GESTURE_SWIPE_3_UP) + dir;
        LOG_DBG("Swipe: %u fingers, dir %u, move (%d, %d) at %d ms", data->gest.max_fingers, dir,
                mx, my, elapsed);
        iqs5xx_press_gesture(dev, gesture);
        data->gest.done = true;
    }

    if (count == 0) {
        // Tap: all fingers up, short and hardly moved.
        if (multi && !data->gest.done && elapsed <= CONFIG_MCK_IQS5XX_GESTURE_TAP_MS &&
            data->gest.peak <= CONFIG_MCK_IQS5XX_GESTURE_TAP_MOVE) {
            LOG_DBG("Tap: %u fingers, %d ms, moved %d", data->gest.max_fingers, elapsed,
                    data->gest.peak);
            iqs5xx_press_gesture(dev, four ? IQS5XX_GESTURE_TAP_4 : IQS5XX_GESTURE_TAP_3);
        }
        data->gest.active = false;
    }

    return multi;
}
#endif

// MCK: The original two finger scroll, also used by scroll only and the scroll inertia:
// 32 trackpad units per wheel step, one axis at a time.
static void iqs5xx_report_scroll(const struct device *dev, int16_t rel_x, int16_t rel_y) {
    const struct iqs5xx_config *config = dev->config;
    struct iqs5xx_data *data = dev->data;
    // TODO: Expose this divisor.
    const int16_t scroll_div = 32;

    // Only one scrolling direction is valid at a time.
    if (rel_x != 0) {
        // By default the x axis is already "natural".
        if (!config->natural_scroll_x) {
            rel_x *= -1;
        }
        data->scroll_x_acc += rel_x;
        if (abs(data->scroll_x_acc) >= scroll_div) {
            input_report_rel(dev, INPUT_REL_HWHEEL, data->scroll_x_acc / scroll_div, true,
                             K_FOREVER);
            data->scroll_x_acc %= scroll_div;
        }
        return;
    }
    if (rel_y != 0) {
        if (config->natural_scroll_y) {
            rel_y *= -1;
        }
        data->scroll_y_acc += rel_y;
        if (abs(data->scroll_y_acc) >= scroll_div) {
            input_report_rel(dev, INPUT_REL_WHEEL, data->scroll_y_acc / scroll_div, true,
                             K_FOREVER);
            data->scroll_y_acc %= scroll_div;
        }
    }
}

#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)
// MCK: 2026-11-26 (D-136 T4) correction, acceleration, inertia and scroll only. Only the
// ideas come from the GR-Trackpad65 kit (average of the last N moves, acceleration growing
// with the speed, inertia when lifted); the code is written here. With the defaults
// (correction 1, the rest 0 / off) the movement is reported exactly as before.
// 2026-11-27: speeds per time (not per report), the correction restarts after a pause and
// the inertia slows down by a ratio each step (it slid too far and started on slow moves).
#define IQS5XX_MOTION_NONE 0
#define IQS5XX_MOTION_CURSOR 1
#define IQS5XX_MOTION_SCROLL 2

// One IC report (ms), seen in the motion log (2026-11-27): the unit of the speeds.
#define IQS5XX_REPORT_MS 15
// The correction forgets the moves before a pause longer than this (ms).
#define IQS5XX_CORRECTION_GAP_MS 50
// A lift counts for inertia only when the last move was this recent (ms).
#define IQS5XX_INERTIA_LIFT_MS 40
// The inertia speed is that of the moves in about the last this many ms before the lift.
#define IQS5XX_INERTIA_WINDOW_MS 45
// The inertia stops below this speed (1/256 units per step).
#define IQS5XX_INERTIA_STOP 64
// At most this many inertia steps.
#define IQS5XX_INERTIA_MAX_STEPS 200

static struct mck_trackpad_motion motion_cfg = {
    .correction = CONFIG_MCK_IQS5XX_CORRECTION,
    .accel = CONFIG_MCK_IQS5XX_ACCEL,
    .inertia_cursor = CONFIG_MCK_IQS5XX_INERTIA_CURSOR,
    .inertia_scroll = CONFIG_MCK_IQS5XX_INERTIA_SCROLL,
    .scroll_only = IS_ENABLED(CONFIG_MCK_IQS5XX_SCROLL_ONLY),
    .tap_drag = IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG_ON),
};
static atomic_t scroll_mode_holds;

void mck_trackpad_get_motion(struct mck_trackpad_motion *motion) { *motion = motion_cfg; }

void mck_trackpad_get_default_motion(struct mck_trackpad_motion *motion) {
    *motion = (struct mck_trackpad_motion){
        .correction = CONFIG_MCK_IQS5XX_CORRECTION,
        .accel = CONFIG_MCK_IQS5XX_ACCEL,
        .inertia_cursor = CONFIG_MCK_IQS5XX_INERTIA_CURSOR,
        .inertia_scroll = CONFIG_MCK_IQS5XX_INERTIA_SCROLL,
        .scroll_only = IS_ENABLED(CONFIG_MCK_IQS5XX_SCROLL_ONLY),
        .tap_drag = IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG_ON),
    };
}

void mck_trackpad_set_motion(const struct mck_trackpad_motion *motion) {
    motion_cfg.correction = CLAMP(motion->correction, 1, 9);
    motion_cfg.accel = MIN(motion->accel, 9);
    motion_cfg.inertia_cursor = MIN(motion->inertia_cursor, 9);
    motion_cfg.inertia_scroll = MIN(motion->inertia_scroll, 9);
    motion_cfg.scroll_only = motion->scroll_only;
    motion_cfg.tap_drag = IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG) && motion->tap_drag;
}

void mck_trackpad_scroll_mode(bool pressed) {
    if (pressed) {
        atomic_inc(&scroll_mode_holds);
    } else if (atomic_get(&scroll_mode_holds) > 0) {
        atomic_dec(&scroll_mode_holds);
    }
}

static bool iqs5xx_scroll_only(void) {
    return motion_cfg.scroll_only != (atomic_get(&scroll_mode_holds) > 0);
}

#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION_LOG)
// MCK: 2026-11-27 (D-136 T4) one line per report, to see why slow moves feel jerky and why
// the inertia starts: kind (c cursor, s scroll, z cursor report with no move, i inertia),
// ms since the last line, the move from the IC and what was reported.
static void iqs5xx_motion_log(char kind, int32_t rel_x, int32_t rel_y, int32_t out_x,
                              int32_t out_y) {
    static int64_t last_ms;
    int64_t now = k_uptime_get();
    uint32_t dt = (uint32_t)MIN(now - last_ms, 99999);
    last_ms = now;
    LOG_INF("m %c %u %d %d %d %d", kind, dt, rel_x, rel_y, out_x, out_y);
}
#else
#define iqs5xx_motion_log(...)
#endif

// Report a move in 1/256 units, keeping the part below one unit in *rem_x / *rem_y.
// Returns the reported move in *out_x / *out_y.
static void iqs5xx_emit_cursor(const struct device *dev, int32_t vx, int32_t vy, int32_t *rem_x,
                               int32_t *rem_y, int32_t *out_x, int32_t *out_y) {
    *rem_x += vx;
    *rem_y += vy;
    *out_x = *rem_x / 256;
    *out_y = *rem_y / 256;
    *rem_x -= *out_x * 256;
    *rem_y -= *out_y * 256;
    if (*out_x != 0 || *out_y != 0) {
        input_report_rel(dev, INPUT_REL_X, *out_x, false, K_FOREVER);
        input_report_rel(dev, INPUT_REL_Y, *out_y, true, K_FOREVER);
    }
}

#if IS_ENABLED(CONFIG_MCK_IQS5XX_LIFT_HOLD)
// MCK: 2026-11-28 (D-136 T7-1) a finger rolls as it lifts, and its last 1-2 events move the
// position by 5-11 (event log: 11 of 28 touches), so the cursor jumped off a button it was
// slowly brought onto. Slow moves (each axis <= CONFIG_MCK_IQS5XX_LIFT_HOLD_MAX) wait
// CONFIG_MCK_IQS5XX_LIFT_HOLD_EVENTS events (about 15 ms each) before they are reported, and
// are dropped if the finger lifts first. A faster move reports the waiting ones with it.

// Report the waiting moves (all, or those that waited long enough) in one report.
static void iqs5xx_hold_release(const struct device *dev, bool all) {
    struct iqs5xx_data *data = dev->data;
    int32_t vx = 0, vy = 0;
    uint8_t k = 0;
    while (k < data->motion.hold_len &&
           (all || data->motion.ev_count - data->motion.hold_ev[k] >=
                       CONFIG_MCK_IQS5XX_LIFT_HOLD_EVENTS)) {
        vx += data->motion.hold_x[k];
        vy += data->motion.hold_y[k];
        k++;
    }
    if (k == 0) {
        return;
    }
    data->motion.hold_len -= k;
    for (uint8_t i = 0; i < data->motion.hold_len; i++) {
        data->motion.hold_x[i] = data->motion.hold_x[i + k];
        data->motion.hold_y[i] = data->motion.hold_y[i + k];
        data->motion.hold_ev[i] = data->motion.hold_ev[i + k];
    }
    int32_t out_x, out_y;
    iqs5xx_emit_cursor(dev, vx, vy, &data->motion.rem_x, &data->motion.rem_y, &out_x, &out_y);
    iqs5xx_motion_log('h', 0, 0, out_x, out_y);
}

// The finger lifted (or more fingers came): forget the waiting moves.
static void iqs5xx_hold_drop(struct iqs5xx_data *data) {
    if (data->motion.hold_len > 0) {
        int32_t vx = 0, vy = 0;
        for (uint8_t i = 0; i < data->motion.hold_len; i++) {
            vx += data->motion.hold_x[i];
            vy += data->motion.hold_y[i];
        }
        iqs5xx_motion_log('d', vx / 256, vy / 256, 0, 0);
    }
    data->motion.hold_len = 0;
}
#endif

// The ms this move took: since the move before of the same kind in this touch, or one IC
// report for the first one. 2026-11-27: the IC reports about every 15 ms, but a slow move
// only every 30-200 ms (one bigger move each), so the speed is measured per time.
static uint16_t iqs5xx_motion_dt(const struct iqs5xx_data *data, uint8_t kind) {
    if (data->motion.kind != kind || data->motion.vel_len == 0) {
        return IQS5XX_REPORT_MS;
    }
    return (uint16_t)CLAMP(k_uptime_get() - data->motion.last_ms, 1, 1000);
}

static void iqs5xx_motion_record(struct iqs5xx_data *data, uint8_t kind, int32_t vx, int32_t vy,
                                 uint16_t dt) {
    if (data->motion.kind != kind) {
        data->motion.vel_len = 0;
    }
    data->motion.kind = kind;
    data->motion.vel_x[data->motion.vel_pos] = vx;
    data->motion.vel_y[data->motion.vel_pos] = vy;
    data->motion.vel_dt[data->motion.vel_pos] = dt;
    data->motion.vel_pos = (data->motion.vel_pos + 1) % ARRAY_SIZE(data->motion.vel_x);
    data->motion.vel_len = MIN(data->motion.vel_len + 1, ARRAY_SIZE(data->motion.vel_x));
    data->motion.last_ms = k_uptime_get();
}

// One finger movement: correction, then acceleration.
static void iqs5xx_cursor(const struct device *dev, int16_t rel_x, int16_t rel_y) {
    struct iqs5xx_data *data = dev->data;
    const uint8_t size = ARRAY_SIZE(data->motion.hist_x);
    uint16_t dt = iqs5xx_motion_dt(data, IQS5XX_MOTION_CURSOR);

    // The correction averages only a move that goes on: after a pause, start again.
    if (dt > IQS5XX_CORRECTION_GAP_MS) {
        data->motion.hist_len = 0;
    }
    data->motion.hist_x[data->motion.hist_pos] = rel_x;
    data->motion.hist_y[data->motion.hist_pos] = rel_y;
    data->motion.hist_pos = (data->motion.hist_pos + 1) % size;
    data->motion.hist_len = MIN(data->motion.hist_len + 1, size);

    uint8_t n = MAX(MIN(motion_cfg.correction, data->motion.hist_len), 1);
    int32_t sum_x = 0, sum_y = 0;
    for (int i = 0; i < n; i++) {
        uint8_t idx = (data->motion.hist_pos + size - 1 - i) % size;
        sum_x += data->motion.hist_x[idx];
        sum_y += data->motion.hist_y[idx];
    }
    int32_t vx = sum_x * 256 / n;
    int32_t vy = sum_y * 256 / n;

    if (motion_cfg.accel > 0) {
        // Up to 1 + accel * 2 / 9 times for a speed of 32 or more per IC report (15 ms).
        int32_t speed = MAX(abs(vx), abs(vy)) * IQS5XX_REPORT_MS / MAX(dt, IQS5XX_REPORT_MS);
        speed = MIN(speed / 256, 32);
        int32_t gain = 256 + motion_cfg.accel * speed * 256 / (9 * 16);
        vx = vx * gain / 256;
        vy = vy * gain / 256;
    }

    iqs5xx_motion_record(data, IQS5XX_MOTION_CURSOR, vx, vy, dt);
#if IS_ENABLED(CONFIG_MCK_IQS5XX_LIFT_HOLD)
    if (MAX(abs(rel_x), abs(rel_y)) <= CONFIG_MCK_IQS5XX_LIFT_HOLD_MAX) {
        if (data->motion.hold_len == ARRAY_SIZE(data->motion.hold_x)) {
            iqs5xx_hold_release(dev, true);
        }
        uint8_t k = data->motion.hold_len++;
        data->motion.hold_x[k] = vx;
        data->motion.hold_y[k] = vy;
        data->motion.hold_ev[k] = data->motion.ev_count;
        iqs5xx_motion_log('w', rel_x, rel_y, 0, 0);
        return;
    }
    iqs5xx_hold_release(dev, true);
#endif
    int32_t out_x, out_y;
    iqs5xx_emit_cursor(dev, vx, vy, &data->motion.rem_x, &data->motion.rem_y, &out_x, &out_y);
    iqs5xx_motion_log('c', rel_x, rel_y, out_x, out_y);
}

static void iqs5xx_inertia_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, motion.inertia_work);
    const struct device *dev = data->dev;

    // Full speed on the first step, then slow down by in_keep / 256 each step (fast at first,
    // then gently to a stop), until below IQS5XX_INERTIA_STOP.
    int32_t vx = data->motion.in_vx;
    int32_t vy = data->motion.in_vy;
    data->motion.in_vx = vx * data->motion.in_keep / 256;
    data->motion.in_vy = vy * data->motion.in_keep / 256;
    data->motion.in_step++;

    if (data->motion.in_kind == IQS5XX_MOTION_CURSOR) {
        int32_t out_x, out_y;
        iqs5xx_emit_cursor(dev, vx, vy, &data->motion.in_rem_x, &data->motion.in_rem_y, &out_x,
                           &out_y);
        iqs5xx_motion_log('i', vx, vy, out_x, out_y);
    } else {
        data->motion.in_rem_x += vx;
        data->motion.in_rem_y += vy;
        int32_t out_x = data->motion.in_rem_x / 256;
        int32_t out_y = data->motion.in_rem_y / 256;
        data->motion.in_rem_x -= out_x * 256;
        data->motion.in_rem_y -= out_y * 256;
        iqs5xx_report_scroll(dev, out_x, out_y);
    }

    if (data->motion.in_step < IQS5XX_INERTIA_MAX_STEPS &&
        MAX(abs(data->motion.in_vx), abs(data->motion.in_vy)) >= IQS5XX_INERTIA_STOP) {
        k_work_schedule(&data->motion.inertia_work, K_MSEC(CONFIG_MCK_IQS5XX_INERTIA_INTERVAL_MS));
    }
}

// Follow the touch: a new touch stops the inertia; lifting a quick move starts it.
static void iqs5xx_motion_touch(const struct device *dev, uint8_t fingers) {
    struct iqs5xx_data *data = dev->data;

#if IS_ENABLED(CONFIG_MCK_IQS5XX_LIFT_HOLD)
    // MCK: 2026-11-28 count the events of a one finger touch and report the slow moves that
    // waited long enough; a lift (or another finger) drops the ones still waiting.
    if (fingers == 1) {
        data->motion.ev_count++;
        iqs5xx_hold_release(dev, false);
    } else {
        iqs5xx_hold_drop(data);
    }
#endif

    if (fingers > 0) {
        if (!data->motion.touching) {
            data->motion.touching = true;
            k_work_cancel_delayable(&data->motion.inertia_work);
            data->motion.hist_len = data->motion.hist_pos = 0;
            data->motion.rem_x = data->motion.rem_y = 0;
            data->motion.vel_len = data->motion.vel_pos = 0;
            data->motion.kind = IQS5XX_MOTION_NONE;
        }
        return;
    }
    if (!data->motion.touching) {
        return;
    }
    data->motion.touching = false;

    uint8_t kind = data->motion.kind;
    uint8_t length = kind == IQS5XX_MOTION_CURSOR   ? motion_cfg.inertia_cursor
                     : kind == IQS5XX_MOTION_SCROLL ? motion_cfg.inertia_scroll
                                                    : 0;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION_LOG)
    // The ms since the last move, then the averaged speed (1/256) when it is computed.
    LOG_INF("m lift kind %u, %u ms since the last move, %u moves", kind,
            (uint32_t)MIN(k_uptime_get() - data->motion.last_ms, 99999), data->motion.vel_len);
#endif
    if (length == 0 || data->motion.vel_len == 0 ||
        k_uptime_get() - data->motion.last_ms > IQS5XX_INERTIA_LIFT_MS) {
        return;
    }

    // The speed of the last moves (newest first, about IQS5XX_INERTIA_WINDOW_MS), per step.
    const uint8_t size = ARRAY_SIZE(data->motion.vel_x);
    int32_t vx = 0, vy = 0, took = 0;
    for (int i = 0; i < data->motion.vel_len && took < IQS5XX_INERTIA_WINDOW_MS; i++) {
        uint8_t idx = (data->motion.vel_pos + size - 1 - i) % size;
        vx += data->motion.vel_x[idx];
        vy += data->motion.vel_y[idx];
        took += data->motion.vel_dt[idx];
    }
    took = MAX(took, 1);
    vx = vx * CONFIG_MCK_IQS5XX_INERTIA_INTERVAL_MS / took;
    vy = vy * CONFIG_MCK_IQS5XX_INERTIA_INTERVAL_MS / took;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION_LOG)
    LOG_INF("m lift speed (%d, %d)/256 per step, from %d ms of moves", vx, vy, took);
#endif
    if (MAX(abs(vx), abs(vy)) < CONFIG_MCK_IQS5XX_INERTIA_MIN_SPEED * 256) {
        return;
    }
    if (kind == IQS5XX_MOTION_SCROLL) {
        // The scroll moves one axis at a time: keep the larger one.
        if (abs(vx) >= abs(vy)) {
            vy = 0;
        } else {
            vx = 0;
        }
    }

    // Keep 1 - 1 / (1 + length) of the speed each step: in all about (1 + length) times the
    // first step (length 5: 0.84, about 6 times, stops in about 0.4 s).
    uint16_t keep = 256 - 256 / (1 + length);
    LOG_DBG("Inertia: kind %u, speed (%d, %d)/256, keep %u/256", kind, vx, vy, keep);
    data->motion.in_kind = kind;
    data->motion.in_vx = vx;
    data->motion.in_vy = vy;
    data->motion.in_step = 0;
    data->motion.in_keep = keep;
    data->motion.in_rem_x = data->motion.in_rem_y = 0;
    k_work_schedule(&data->motion.inertia_work, K_MSEC(CONFIG_MCK_IQS5XX_INERTIA_INTERVAL_MS));
}
#endif // IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)

#if IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG)
// MCK: 2026-11-28 (D-136 T7-4a) tap and drag, as the GR-Trackpad65 kit does: a one finger
// touch that starts within CONFIG_MCK_IQS5XX_TAP_DRAG_WINDOW_MS of a one finger tap and moves
// (CONFIG_MCK_IQS5XX_TAP_DRAG_MOVE) or stays (CONFIG_MCK_IQS5XX_TAP_DRAG_HOLD_MS) keeps the
// one finger tap's key position (BTN_0 without gesture-positions) pressed until it lifts.
// A second touch that lifts at once stays a tap (a double click).
#define IQS5XX_DRAG_IDLE 0 // no touch yet, or the last one ended
#define IQS5XX_DRAG_WAIT 1 // a touch after a tap: a drag if it moves or stays
#define IQS5XX_DRAG_ON 2   // dragging
#define IQS5XX_DRAG_NO 3   // this touch is not a drag

static void iqs5xx_drag_press(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    struct iqs5xx_data *data = dev->data;

    // The tap before may still be pressed (it is released 100 ms later): release it first.
    k_work_cancel_delayable(&data->button_release_work);
    iqs5xx_release_all(data);

    if (config->gesture_positions_len > IQS5XX_GESTURE_TAP_1 &&
        config->gesture_positions[IQS5XX_GESTURE_TAP_1] < ZMK_KEYMAP_LEN) {
        data->pressed_position = config->gesture_positions[IQS5XX_GESTURE_TAP_1];
        iqs5xx_raise_position(data->pressed_position, true);
    } else {
        input_report_key(dev, INPUT_BTN_0, 1, true, K_FOREVER);
        data->buttons_pressed |= BIT(0);
    }
    LOG_INF("Tap and drag: pressed");
}

// Follow the touch for tap and drag. Returns true when an IC tap of this event belongs to a
// drag (it is not a click then).
static bool iqs5xx_tap_drag(const struct device *dev, const struct iqs5xx_status *st) {
    struct iqs5xx_data *data = dev->data;
    const int64_t now = k_uptime_get();
    const uint8_t fingers = st->num_fingers;

    switch (data->motion.drag_state) {
    case IQS5XX_DRAG_IDLE:
        if (fingers == 1 && motion_cfg.tap_drag && data->motion.drag_tap_ms != 0 &&
            now - data->motion.drag_tap_ms <= CONFIG_MCK_IQS5XX_TAP_DRAG_WINDOW_MS) {
            data->motion.drag_state = IQS5XX_DRAG_WAIT;
            data->motion.drag_start_ms = now;
            data->motion.drag_move = 0;
        } else if (fingers > 0) {
            data->motion.drag_state = IQS5XX_DRAG_NO;
        }
        break;
    case IQS5XX_DRAG_WAIT:
        if (fingers == 1) {
            data->motion.drag_move += abs(st->rel_x) + abs(st->rel_y);
            if (data->motion.drag_move >= CONFIG_MCK_IQS5XX_TAP_DRAG_MOVE ||
                now - data->motion.drag_start_ms >= CONFIG_MCK_IQS5XX_TAP_DRAG_HOLD_MS) {
                data->motion.drag_state = IQS5XX_DRAG_ON;
                data->motion.drag_tap_ms = 0;
                iqs5xx_drag_press(dev);
            }
        } else {
            data->motion.drag_state = fingers == 0 ? IQS5XX_DRAG_IDLE : IQS5XX_DRAG_NO;
        }
        break;
    case IQS5XX_DRAG_ON:
        if (fingers != 1) {
            k_work_cancel_delayable(&data->button_release_work);
            iqs5xx_release_all(data);
            LOG_INF("Tap and drag: released");
            data->motion.drag_state = fingers == 0 ? IQS5XX_DRAG_IDLE : IQS5XX_DRAG_NO;
            return (st->gesture_events_0 & IQS5XX_SINGLE_TAP) != 0;
        }
        break;
    default: // IQS5XX_DRAG_NO
        if (fingers == 0) {
            data->motion.drag_state = IQS5XX_DRAG_IDLE;
        }
        break;
    }
    return false;
}
#endif // IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG)

#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
// MCK: 2026-11-28 (D-136 T7-1) values of the IC itself (struct mck_trackpad_ic), changed at
// runtime through mck__trackpad. Slow moves arrived every 100-200 ms: a touch within the
// stationary threshold (20 px) makes the IC run at the idle touch rate (50 ms), and below the
// lower speed the dynamic filter damps hard (bottom beta 7 / 256).
// The IC only takes writes while its communication window is open, so a change bumps
// ic_changes and each trackpad writes the values on its next event (or setup).
static struct k_spinlock ic_lock;
static struct mck_trackpad_ic ic_cfg;
static struct mck_trackpad_ic ic_default;
static bool ic_default_read;
// ic_cfg was set (saved values loaded, or a change); until then it follows the IC.
static bool ic_cfg_set;
static atomic_t ic_changes;

void mck_trackpad_get_ic(struct mck_trackpad_ic *ic) {
    k_spinlock_key_t key = k_spin_lock(&ic_lock);
    *ic = ic_cfg;
    k_spin_unlock(&ic_lock, key);
}

bool mck_trackpad_get_default_ic(struct mck_trackpad_ic *ic) {
    k_spinlock_key_t key = k_spin_lock(&ic_lock);
    *ic = ic_default;
    bool read = ic_default_read;
    k_spin_unlock(&ic_lock, key);
    return read;
}

void mck_trackpad_set_ic(const struct mck_trackpad_ic *ic) {
    const struct mck_trackpad_ic v = {
        .stationary_threshold = MAX(ic->stationary_threshold, 1),
        .bottom_beta = MAX(ic->bottom_beta, 1),
        .lower_speed = MAX(ic->lower_speed, 1),
        .idle_touch_ms = CLAMP(ic->idle_touch_ms, 5, 1000),
        .tap_time_ms = CLAMP(ic->tap_time_ms, 20, 1000),
    };
    k_spinlock_key_t key = k_spin_lock(&ic_lock);
    ic_cfg = v;
    ic_cfg_set = true;
    k_spin_unlock(&ic_lock, key);
    atomic_inc(&ic_changes);
}

static int iqs5xx_ic_read(const struct device *dev, struct mck_trackpad_ic *ic) {
    int ret = iqs5xx_read_reg8(dev, IQS5XX_STATIONARY_THRESH, &ic->stationary_threshold);
    ret = ret < 0 ? ret : iqs5xx_read_reg8(dev, IQS5XX_BOTTOM_BETA, &ic->bottom_beta);
    ret = ret < 0 ? ret : iqs5xx_read_reg8(dev, IQS5XX_DYNAMIC_LOWER_SPEED, &ic->lower_speed);
    ret = ret < 0 ? ret : iqs5xx_read_reg16(dev, IQS5XX_IDLE_TOUCH_RATE, &ic->idle_touch_ms);
    ret = ret < 0 ? ret : iqs5xx_read_reg16(dev, IQS5XX_TAP_TIME, &ic->tap_time_ms);
    return ret;
}

static int iqs5xx_ic_write(const struct device *dev, const struct mck_trackpad_ic *ic) {
    int ret = iqs5xx_write_reg8(dev, IQS5XX_STATIONARY_THRESH, ic->stationary_threshold);
    ret = ret < 0 ? ret : iqs5xx_write_reg8(dev, IQS5XX_BOTTOM_BETA, ic->bottom_beta);
    ret = ret < 0 ? ret : iqs5xx_write_reg8(dev, IQS5XX_DYNAMIC_LOWER_SPEED, ic->lower_speed);
    ret = ret < 0 ? ret : iqs5xx_write_reg16(dev, IQS5XX_IDLE_TOUCH_RATE, ic->idle_touch_ms);
    ret = ret < 0 ? ret : iqs5xx_write_reg16(dev, IQS5XX_TAP_TIME, ic->tap_time_ms);
    return ret;
}

// In the communication window: on setup read the IC's own values (once) and write the values
// in use if they were set; on an event write them only when they changed since.
static void iqs5xx_ic_apply(const struct device *dev, bool setup) {
    struct iqs5xx_data *data = dev->data;

    if (setup && !ic_default_read) {
        struct mck_trackpad_ic def;
        if (iqs5xx_ic_read(dev, &def) == 0) {
            k_spinlock_key_t key = k_spin_lock(&ic_lock);
            ic_default = def;
            ic_default_read = true;
            if (!ic_cfg_set) {
                ic_cfg = def;
            }
            k_spin_unlock(&ic_lock, key);
            LOG_INF("IC values: stationary %u, bottom beta %u, lower speed %u, idle touch %u ms, "
                    "tap %u ms",
                    def.stationary_threshold, def.bottom_beta, def.lower_speed,
                    def.idle_touch_ms, def.tap_time_ms);
        } else {
            LOG_WRN("Failed to read the IC values");
        }
    }

    const atomic_val_t changes = atomic_get(&ic_changes);
    if (!setup && data->ic_written == changes) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&ic_lock);
    const bool set = ic_cfg_set;
    const struct mck_trackpad_ic v = ic_cfg;
    k_spin_unlock(&ic_lock, key);

    if (set) {
        int ret = iqs5xx_ic_write(dev, &v);
        if (ret < 0) {
            // Try again on the next event.
            LOG_WRN("Failed to write the IC values: %d", ret);
            return;
        }
        LOG_INF("IC values set: stationary %u, bottom beta %u, lower speed %u, idle touch %u ms, "
                "tap %u ms",
                v.stationary_threshold, v.bottom_beta, v.lower_speed, v.idle_touch_ms,
                v.tap_time_ms);
    }
    data->ic_written = changes;
}
#endif // IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)

static void iqs5xx_work_handler(struct k_work *work) {
    struct iqs5xx_data *data = CONTAINER_OF(work, struct iqs5xx_data, work);
    const struct device *dev = data->dev;
    // MCK: Only the gestures use it here now (the scroll moved to iqs5xx_report_scroll).
    const struct iqs5xx_config *config __maybe_unused = dev->config;
    struct iqs5xx_status st;
    int ret;

    if (!data->config_logged) {
        iqs5xx_log_config(dev);
        data->config_logged = true;
    }

    ret = iqs5xx_read_status(dev, &st);
    if (ret < 0) {
        goto end_comm;
    }

    // Handle reset indication.
    if (st.sys_info_0 & IQS5XX_SHOW_RESET) {
        LOG_INF("Device reset detected");
        // Acknowledge reset.
        iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONTROL_0, IQS5XX_ACK_RESET);
        // GRT65W: A reset (e.g. the watchdog) reloads the non-volatile defaults, which turns
        // event mode off. Apply the setup again; it also ends the communication window.
        ret = iqs5xx_setup_device(dev);
        if (ret < 0) {
            LOG_ERR("Failed to set up again after reset: %d", ret);
            goto end_comm;
        }
        return;
    }

#if IS_ENABLED(CONFIG_MCK_IQS5XX_TOUCH_LOG)
    iqs5xx_touch_log(data, &st);
#endif

#if IS_ENABLED(CONFIG_MCK_IQS5XX_EVENT_LOG)
    // MCK: 2026-11-28 (D-136 T7-1) every event as the IC sent it, also those the cursor does
    // not use (no TP_MOVEMENT): ms since the last event, fingers, system info 1 (bit 0
    // TP_MOVEMENT), gesture events 0 / 1, relative X / Y, finger 1 absolute X / Y.
    {
        static int64_t last_ev_ms;
        int64_t now = k_uptime_get();
        LOG_INF("e %u %u %02x %02x %02x %d %d %u %u", (uint32_t)MIN(now - last_ev_ms, 99999),
                st.num_fingers, st.sys_info_1, st.gesture_events_0, st.gesture_events_1, st.rel_x,
                st.rel_y, st.fingers[0].x, st.fingers[0].y);
        last_ev_ms = now;
    }
#endif

    // MCK: While 3 / 4 fingers are down (until all are up), report only their gestures.
    bool multi_finger = false;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_GESTURES)
    if (iqs5xx_gestures_enabled(config)) {
        multi_finger = iqs5xx_gestures_update(dev, &st);
    }
#endif

    bool scroll_only = false;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)
    iqs5xx_motion_touch(dev, st.num_fingers);
    scroll_only = iqs5xx_scroll_only();
    if (multi_finger) {
        // No inertia after a 3 / 4 finger touch.
        data->motion.kind = IQS5XX_MOTION_NONE;
    }
#endif

    bool tp_movement = (st.sys_info_1 & IQS5XX_TP_MOVEMENT) != 0;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_KEEP_SLOW_MOVES)
    // MCK: 2026-11-28 (D-136 T7-1) in a slow move TP_MOVEMENT comes and goes (the IC calls the
    // touch stationary in between), and the moves of the events without it were dropped: the
    // cursor stuttered as it slowed down (event log: 1-2 tenths of the move lost). Keep them
    // while at least 2 of the last 4 events had TP_MOVEMENT; a resting finger (rare, single
    // TP_MOVEMENT events) still moves nothing.
    {
        const bool recent = __builtin_popcount(data->movement_hist & 0x0F) >= 2;
        data->movement_hist =
            st.num_fingers > 0 ? (uint8_t)(data->movement_hist << 1) | (tp_movement ? 1 : 0) : 0;
        tp_movement = tp_movement || (recent && st.num_fingers > 0);
    }
#endif
    bool scroll = (st.gesture_events_1 & IQS5XX_SCROLL) != 0;
    if (!scroll && !(scroll_only && tp_movement)) {
        // Clear accumulators if we're not actively scrolling.
        data->scroll_x_acc = 0;
        data->scroll_y_acc = 0;
    }

    uint16_t button_code;
    bool button_pressed = false;
    bool drag_tap = false;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG)
    // MCK: A tap at the end of a drag is not a click.
    drag_tap = iqs5xx_tap_drag(dev, &st);
#endif
    if ((st.gesture_events_0 & IQS5XX_SINGLE_TAP) && !drag_tap) {
        button_pressed = true;
        button_code = INPUT_BTN_0;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_TAP_DRAG)
        // MCK: A touch soon after this tap may become a drag.
        data->motion.drag_tap_ms = k_uptime_get();
#endif
    } else if (st.gesture_events_1 & IQS5XX_TWO_FINGER_TAP) {
        button_pressed = true;
        button_code = INPUT_BTN_1;
    }

    bool hold_became_active = (st.gesture_events_0 & IQS5XX_PRESS_AND_HOLD) && !data->active_hold;
    bool hold_released = !(st.gesture_events_0 & IQS5XX_PRESS_AND_HOLD) && data->active_hold;

    int16_t rel_x = st.rel_x;
    int16_t rel_y = st.rel_y;

    // Handle movement and gestures.
    //
    // Each one of these branches needs to send the last report it makes as
    // sync to ensure that the input subsystem processes things in order.
    if (hold_became_active) {
        LOG_INF("Hold became active");
        input_report_key(dev, LEFT_BUTTON_CODE, 1, true, K_FOREVER);
        data->active_hold = true;
    } else if (hold_released) {
        LOG_INF("Hold became inactive");
        input_report_key(dev, LEFT_BUTTON_CODE, 0, true, K_FOREVER);
        data->active_hold = false;
    } else if (multi_finger) {
        // MCK: No cursor, scroll or IC tap during a 3 / 4 finger touch.
        data->scroll_x_acc = 0;
        data->scroll_y_acc = 0;
    } else if (button_pressed &&
               iqs5xx_press_gesture(dev, button_code == INPUT_BTN_0 ? IQS5XX_GESTURE_TAP_1
                                                                     : IQS5XX_GESTURE_TAP_2)) {
        // MCK: The tap pressed its key position (gesture-positions).
    } else if (button_pressed) {
        // Cancel any pending release.
        k_work_cancel_delayable(&data->button_release_work);

        // Press the button immediately.
        input_report_key(dev, button_code, 1, true, K_FOREVER);
        data->buttons_pressed |= BIT(button_code - INPUT_BTN_0);

        // Schedule release after 100ms.
        k_work_schedule(&data->button_release_work, K_MSEC(100));
    } else if (scroll || (scroll_only && tp_movement)) {
        // MCK: Two finger scroll, or one finger with scroll only (CONFIG_MCK_IQS5XX_MOTION).
        iqs5xx_report_scroll(dev, rel_x, rel_y);
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)
        if (rel_x != 0 || rel_y != 0) {
            iqs5xx_motion_record(data, IQS5XX_MOTION_SCROLL, rel_x * 256, rel_y * 256,
                                 iqs5xx_motion_dt(data, IQS5XX_MOTION_SCROLL));
        }
        iqs5xx_motion_log('s', rel_x, rel_y, 0, 0);
#endif
    } else if (tp_movement) {
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)
        // MCK: Correction and acceleration (CONFIG_MCK_IQS5XX_MOTION).
        if (rel_x != 0 || rel_y != 0) {
            iqs5xx_cursor(dev, rel_x, rel_y);
        } else {
            iqs5xx_motion_log('z', 0, 0, 0, 0);
        }
#else
        if (rel_x != 0 || rel_y != 0) {
            input_report_rel(dev, INPUT_REL_X, rel_x, false, K_FOREVER);
            input_report_rel(dev, INPUT_REL_Y, rel_y, true, K_FOREVER);
        }
#endif
    }

#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
    // MCK: Write the IC values changed since the last event (the window is open now).
    iqs5xx_ic_apply(dev, false);
#endif

end_comm:
    // End communication window.
    iqs5xx_end_comm_window(dev);
}

static void iqs5xx_rdy_handler(const struct device *port, struct gpio_callback *cb,
                               gpio_port_pins_t pins) {
    struct iqs5xx_data *data = CONTAINER_OF(cb, struct iqs5xx_data, rdy_cb);

    k_work_submit(&data->work);
}

// GRT65W: Set and clear bits of a register, keeping every other bit as the
// module's non-volatile defaults left them (e.g. REATI, WDT, PALM_REJECT).
static int iqs5xx_update_reg8(const struct device *dev, uint16_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur;
    int ret = iqs5xx_read_reg8(dev, reg, &cur);
    if (ret < 0) {
        return ret;
    }

    return iqs5xx_write_reg8(dev, reg, (cur & ~mask) | (val & mask));
}

// GRT65W: Log the registers this driver touches, so the result can be checked over USB logging.
static void iqs5xx_log_config(const struct device *dev) {
    static const struct {
        uint16_t reg;
        const char *name;
    } regs[] = {
        {IQS5XX_SYSTEM_CONFIG_0, "SYSTEM_CONFIG_0"},
        {IQS5XX_SYSTEM_CONFIG_1, "SYSTEM_CONFIG_1"},
        {IQS5XX_XY_CONFIG_0, "XY_CONFIG_0"},
        {IQS5XX_FILTER_SETTINGS, "FILTER_SETTINGS"},
        {IQS5XX_BOTTOM_BETA, "BOTTOM_BETA"},
        {IQS5XX_STATIONARY_THRESH, "STATIONARY_THRESH"},
        {IQS5XX_SINGLE_FINGER_GESTURES_CONF, "SINGLE_FINGER_GESTURES"},
        {IQS5XX_MULTI_FINGER_GESTURES_CONF, "MULTI_FINGER_GESTURES"},
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        uint8_t val;
        if (iqs5xx_read_reg8(dev, regs[i].reg, &val) == 0) {
            LOG_INF("%s (0x%04X) = 0x%02X", regs[i].name, regs[i].reg, val);
        } else {
            LOG_WRN("%s (0x%04X) read failed", regs[i].name, regs[i].reg);
        }
    }
}

static int iqs5xx_setup_device(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    int ret;

    // Enable event mode and trackpad events.
    // GRT65W: Only add the bits this driver needs; keep the other event bits.
    const uint8_t events = IQS5XX_EVENT_MODE | IQS5XX_TP_EVENT | IQS5XX_GESTURE_EVENT;
    ret = iqs5xx_update_reg8(dev, IQS5XX_SYSTEM_CONFIG_1, events, events);
    if (ret < 0) {
        LOG_ERR("Failed to configure event mode: %d", ret);
        return ret;
    }

    // GRT65W: Only override the tuning when it is set in the devicetree.
    // Otherwise keep the module's own values (GR-Trackpad65: bottom beta 7, threshold 20).
    if (config->bottom_beta >= 0) {
        ret = iqs5xx_write_reg8(dev, IQS5XX_BOTTOM_BETA, config->bottom_beta);
        if (ret < 0) {
            LOG_ERR("Failed to set bottom beta: %d", ret);
            return ret;
        }
    }

    if (config->stationary_threshold >= 0) {
        ret = iqs5xx_write_reg8(dev, IQS5XX_STATIONARY_THRESH, config->stationary_threshold);
        if (ret < 0) {
            LOG_ERR("Failed to set bottom stationary threshold: %d", ret);
            return ret;
        }
    }

    // MCK: 2026-11-28 (D-136 T7-2) the same for the idle touch rate and the tap time.
    if (config->idle_touch_rate_ms >= 0) {
        ret = iqs5xx_write_reg16(dev, IQS5XX_IDLE_TOUCH_RATE, config->idle_touch_rate_ms);
        if (ret < 0) {
            LOG_ERR("Failed to set the idle touch rate: %d", ret);
            return ret;
        }
    }

    if (config->tap_time_ms >= 0) {
        ret = iqs5xx_write_reg16(dev, IQS5XX_TAP_TIME, config->tap_time_ms);
        if (ret < 0) {
            LOG_ERR("Failed to set the tap time: %d", ret);
            return ret;
        }
    }

    // GRT65W: Filter settings are left as the module's non-volatile defaults.

#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
    // MCK: Read the IC's own values once, and write the values in use if they were changed
    // (again after a reset, which reloads the module's values).
    iqs5xx_ic_apply(dev, true);
#endif

    uint8_t single_finger_gestures = 0;
    single_finger_gestures |= config->one_finger_tap ? IQS5XX_SINGLE_TAP : 0;
    single_finger_gestures |= config->press_and_hold ? IQS5XX_PRESS_AND_HOLD : 0;
    // Configure single finger gestures.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SINGLE_FINGER_GESTURES_CONF, single_finger_gestures);
    if (ret < 0) {
        LOG_ERR("Failed to configure single finger gestures: %d", ret);
        return ret;
    }

    // Configure the hold time for the press and hold gesture.
    // GRT65W: Only when the gesture is used.
    if (config->press_and_hold) {
        ret = iqs5xx_write_reg16(dev, IQS5XX_HOLD_TIME, config->press_and_hold_time);
        if (ret < 0) {
            LOG_ERR("Failed to configure the hold time: %d", ret);
            return ret;
        }
    }

    uint8_t two_finger_gestures = 0;
    two_finger_gestures |= config->two_finger_tap ? IQS5XX_TWO_FINGER_TAP : 0;
    two_finger_gestures |= config->scroll ? IQS5XX_SCROLL : 0;
    // Configure multi finger gestures.
    ret = iqs5xx_write_reg8(dev, IQS5XX_MULTI_FINGER_GESTURES_CONF, two_finger_gestures);
    if (ret < 0) {
        LOG_ERR("Failed to configure multi finger gestures: %d", ret);
        return ret;
    }

    // Configure axes.
    // GRT65W: Only touch the three axis bits, so PALM_REJECT keeps the module's setting.
    uint8_t xy_config = 0;
    xy_config |= config->flip_x ? IQS5XX_FLIP_X : 0;
    xy_config |= config->flip_y ? IQS5XX_FLIP_Y : 0;
    xy_config |= config->switch_xy ? IQS5XX_SWITCH_XY_AXIS : 0;
    ret = iqs5xx_update_reg8(dev, IQS5XX_XY_CONFIG_0,
                             IQS5XX_FLIP_X | IQS5XX_FLIP_Y | IQS5XX_SWITCH_XY_AXIS, xy_config);
    if (ret < 0) {
        LOG_ERR("Failed to configure axes: %d", ret);
        return ret;
    }

    // Configure system settings.
    // GRT65W: Only set SETUP_COMPLETE, so REATI / ALP_REATI / WDT keep the module's settings.
    ret = iqs5xx_update_reg8(dev, IQS5XX_SYSTEM_CONFIG_0, IQS5XX_SETUP_COMPLETE,
                             IQS5XX_SETUP_COMPLETE);
    if (ret < 0) {
        LOG_ERR("Failed to configure system: %d", ret);
        return ret;
    }

    // GRT65W: Log the result on the next event, when a USB terminal is likely attached.
    ((struct iqs5xx_data *)dev->data)->config_logged = false;

    // End communication window.
    ret = iqs5xx_end_comm_window(dev);
    if (ret < 0) {
        LOG_ERR("Failed to end comm window during initialization: %d", ret);
        return ret;
    }

    return 0;
}

static int iqs5xx_init(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    struct iqs5xx_data *data = dev->data;
    int ret;

    if (!i2c_is_ready_dt(&config->i2c)) {
        LOG_ERR("I2C device not ready");
        return -ENODEV;
    }

    data->dev = dev;
    data->pressed_position = -1;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_MOTION)
    k_work_init_delayable(&data->motion.inertia_work, iqs5xx_inertia_work_handler);
#endif
    k_work_init(&data->work, iqs5xx_work_handler);
    k_work_init_delayable(&data->button_release_work, iqs5xx_button_release_work_handler);

    // Configure reset GPIO if available.
    if (config->reset_gpio.port) {
        if (!gpio_is_ready_dt(&config->reset_gpio)) {
            LOG_ERR("Reset GPIO not ready");
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure reset GPIO: %d", ret);
            return ret;
        }

        // Reset the device.
        gpio_pin_set_dt(&config->reset_gpio, 1);
        k_msleep(1);
        gpio_pin_set_dt(&config->reset_gpio, 0);
        k_msleep(10);
    }

    // Configure RDY GPIO.
    if (!gpio_is_ready_dt(&config->rdy_gpio)) {
        LOG_ERR("RDY GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&config->rdy_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure RDY GPIO: %d", ret);
        return ret;
    }

    gpio_init_callback(&data->rdy_cb, iqs5xx_rdy_handler, BIT(config->rdy_gpio.pin));
    ret = gpio_add_callback(config->rdy_gpio.port, &data->rdy_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add RDY callback: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_EDGE_RISING);
    if (ret < 0) {
        LOG_ERR("Failed to configure RDY interrupt: %d", ret);
        return ret;
    }

    // Wait for device to be ready.
    k_msleep(100);

    // Setup device configuration.
    ret = iqs5xx_setup_device(dev);
    if (ret < 0) {
        LOG_ERR("Failed to setup device: %d", ret);
        return ret;
    }

    data->initialized = true;
    LOG_INF("IQS5xx trackpad initialized");

    return 0;
}

// MCK: gesture-positions, when set.
#define IQS5XX_GESTURE_POSITIONS_DEFINE(n)                                                         \
    IF_ENABLED(DT_INST_NODE_HAS_PROP(n, gesture_positions),                                        \
               (BUILD_ASSERT(DT_INST_PROP_LEN(n, gesture_positions) <= IQS5XX_GESTURE_COUNT,       \
                             "gesture-positions has more entries than there are gestures");        \
                static const uint32_t iqs5xx_gesture_positions_##n[] =                             \
                    DT_INST_PROP(n, gesture_positions);))

#define IQS5XX_GESTURE_POSITIONS_INIT(n)                                                           \
    IF_ENABLED(DT_INST_NODE_HAS_PROP(n, gesture_positions),                                        \
               (.gesture_positions = iqs5xx_gesture_positions_##n,                                 \
                .gesture_positions_len = DT_INST_PROP_LEN(n, gesture_positions), ))

#define IQS5XX_INIT(n)                                                                             \
    IQS5XX_GESTURE_POSITIONS_DEFINE(n)                                                             \
    static struct iqs5xx_data iqs5xx_data_##n;                                                     \
    static const struct iqs5xx_config iqs5xx_config_##n = {                                        \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .rdy_gpio = GPIO_DT_SPEC_INST_GET(n, rdy_gpios),                                           \
        .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),                               \
        .one_finger_tap = DT_INST_PROP(n, one_finger_tap),                                         \
        .press_and_hold = DT_INST_PROP(n, press_and_hold),                                         \
        .two_finger_tap = DT_INST_PROP(n, two_finger_tap),                                         \
        .scroll = DT_INST_PROP(n, scroll),                                                         \
        .natural_scroll_x = DT_INST_PROP(n, natural_scroll_x),                                     \
        .natural_scroll_y = DT_INST_PROP(n, natural_scroll_y),                                     \
        .press_and_hold_time = DT_INST_PROP_OR(n, press_and_hold_time, 250),                       \
        .switch_xy = DT_INST_PROP(n, switch_xy),                                                   \
        .flip_x = DT_INST_PROP(n, flip_x),                                                         \
        .flip_y = DT_INST_PROP(n, flip_y),                                                         \
        .bottom_beta = DT_INST_PROP_OR(n, bottom_beta, -1),                                        \
        .stationary_threshold = DT_INST_PROP_OR(n, stationary_threshold, -1),                      \
        .idle_touch_rate_ms = DT_INST_PROP_OR(n, idle_touch_rate_ms, -1),                          \
        .tap_time_ms = DT_INST_PROP_OR(n, tap_time_ms, -1),                                        \
        IQS5XX_GESTURE_POSITIONS_INIT(n)                                                           \
        .gesture_release_ms = DT_INST_PROP(n, gesture_release_ms),                                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, iqs5xx_init, NULL, &iqs5xx_data_##n, &iqs5xx_config_##n, POST_KERNEL, \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(IQS5XX_INIT)

// MCK: 2026-11-28 (D-136 T5) for mck__trackpad: what the first trackpad does.
void mck_trackpad_get_info(struct mck_trackpad_info *info) {
    *info = (struct mck_trackpad_info){0};
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
    const struct iqs5xx_config *config = DEVICE_DT_GET(DT_DRV_INST(0))->config;
    info->gesture_position_count = config->gesture_positions_len;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_GESTURES)
    info->gestures = iqs5xx_gestures_enabled(config);
#endif
#endif
}
