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
 */

#define DT_DRV_COMPAT mck_iqs5xx

#include <stdlib.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>

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

static void iqs5xx_button_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, button_release_work);

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
};

#if IS_ENABLED(CONFIG_MCK_IQS5XX_BLOCK_READ)
// MCK: 0x000D (gesture events 0) .. 0x0015 (relative Y low byte) are consecutive.
static int iqs5xx_read_status(const struct device *dev, struct iqs5xx_status *st) {
    uint8_t buf[IQS5XX_REL_Y + 2 - IQS5XX_GESTURE_EVENTS_0];
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

static void iqs5xx_work_handler(struct k_work *work) {
    struct iqs5xx_data *data = CONTAINER_OF(work, struct iqs5xx_data, work);
    const struct device *dev = data->dev;
    const struct iqs5xx_config *config = dev->config;
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

    bool tp_movement = (st.sys_info_1 & IQS5XX_TP_MOVEMENT) != 0;
    bool scroll = (st.gesture_events_1 & IQS5XX_SCROLL) != 0;
    if (!scroll) {
        // Clear accumulators if we're not actively scrolling.
        data->scroll_x_acc = 0;
        data->scroll_y_acc = 0;
    }

    uint16_t button_code;
    bool button_pressed = false;
    if (st.gesture_events_0 & IQS5XX_SINGLE_TAP) {
        button_pressed = true;
        button_code = INPUT_BTN_0;
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
    } else if (scroll) {
        // TODO: Expose this divisor.
        int16_t scroll_div = 32;

        // Only one scrolling direction is valid at a time.
        // End the communication right after reporting the movement.
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
            goto end_comm;
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

            goto end_comm;
        }
    } else if (tp_movement) {
        // MCK: The finger count is read with the status (block read) and is not used yet.
        if (rel_x != 0 || rel_y != 0) {
            input_report_rel(dev, INPUT_REL_X, rel_x, false, K_FOREVER);
            input_report_rel(dev, INPUT_REL_Y, rel_y, true, K_FOREVER);
        }
    }

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

    // GRT65W: Filter settings are left as the module's non-volatile defaults.

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
        IQS5XX_GESTURE_POSITIONS_INIT(n)                                                           \
        .gesture_release_ms = DT_INST_PROP(n, gesture_release_ms),                                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, iqs5xx_init, NULL, &iqs5xx_data_##n, &iqs5xx_config_##n, POST_KERNEL, \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(IQS5XX_INIT)
