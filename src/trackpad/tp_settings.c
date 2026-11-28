/*
 * Keep the trackpad movement settings (mck,iqs5xx, CONFIG_MCK_IQS5XX_MOTION) in flash:
 * the Zephyr setting "mck/tp/motion". MCK Aquarium changes them through mck__trackpad.
 *
 * 2026-11-28 (D-136 T7-1): also the values of the IC itself, "mck/tp/ic".
 *
 * 2026-11-28 (D-136 T5). A change is used at once and saved
 * CONFIG_MCK_TRACKPAD_SAVE_DELAY_MS later, so a slider that sends many changes
 * writes the flash once. settings_reset (it erases all the settings) brings back
 * the values the firmware was built with.
 *
 * Copyright (c) 2026 mare-cycles
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <mck/trackpad.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TP_SETTINGS_KEY "mck/tp/motion"
// 2: tap_drag (2026-11-28, D-136 T7-4a). Version 1 (without it) is still read.
#define TP_SETTINGS_VERSION 2
#define TP_SETTINGS_V1_SIZE 6

// What is saved. Add fields at the end (and keep reading the shorter old ones).
struct tp_saved_motion {
    uint8_t version;
    uint8_t correction;
    uint8_t accel;
    uint8_t inertia_cursor;
    uint8_t inertia_scroll;
    uint8_t scroll_only;
    uint8_t tap_drag;
} __packed;

#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
// 2026-11-28 (D-136 T7-1) the IC values ("mck/tp/ic"). Same rules as the motion.
#define TP_IC_SETTINGS_KEY "mck/tp/ic"
#define TP_IC_SETTINGS_VERSION 1

struct tp_saved_ic {
    uint8_t version;
    uint8_t stationary_threshold;
    uint8_t bottom_beta;
    uint8_t lower_speed;
    uint16_t idle_touch_ms;
    uint16_t tap_time_ms;
} __packed;

static int tp_settings_load_ic(size_t len, settings_read_cb read_cb, void *cb_arg) {
    struct tp_saved_ic saved;
    if (len != sizeof(saved)) {
        LOG_WRN("Trackpad IC settings of an unknown size (%zu), not used", len);
        return 0;
    }
    int rc = read_cb(cb_arg, &saved, sizeof(saved));
    if (rc < 0) {
        return rc;
    }
    if (saved.version != TP_IC_SETTINGS_VERSION) {
        LOG_WRN("Trackpad IC settings version %u, not used", saved.version);
        return 0;
    }
    mck_trackpad_set_ic(&(struct mck_trackpad_ic){
        .stationary_threshold = saved.stationary_threshold,
        .bottom_beta = saved.bottom_beta,
        .lower_speed = saved.lower_speed,
        .idle_touch_ms = saved.idle_touch_ms,
        .tap_time_ms = saved.tap_time_ms,
    });
    LOG_INF("Trackpad IC settings loaded: stationary %u, bottom beta %u, lower speed %u, "
            "idle touch %u ms, tap %u ms",
            saved.stationary_threshold, saved.bottom_beta, saved.lower_speed,
            saved.idle_touch_ms, saved.tap_time_ms);
    return 0;
}
#endif

static int tp_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
    if (strcmp(name, "ic") == 0) {
        return tp_settings_load_ic(len, read_cb, cb_arg);
    }
#endif
    if (strcmp(name, "motion") != 0) {
        return -ENOENT;
    }
    struct tp_saved_motion saved;
    if (len != sizeof(saved) && len != TP_SETTINGS_V1_SIZE) {
        LOG_WRN("Trackpad settings of an unknown size (%zu), not used", len);
        return 0;
    }
    int rc = read_cb(cb_arg, &saved, len);
    if (rc < 0) {
        return rc;
    }
    if (len == TP_SETTINGS_V1_SIZE && saved.version == 1) {
        // Saved before tap and drag: it starts as the firmware sets it.
        struct mck_trackpad_motion def;
        mck_trackpad_get_default_motion(&def);
        saved.tap_drag = def.tap_drag ? 1 : 0;
    } else if (len != sizeof(saved) || saved.version != TP_SETTINGS_VERSION) {
        LOG_WRN("Trackpad settings version %u, not used", saved.version);
        return 0;
    }
    mck_trackpad_set_motion(&(struct mck_trackpad_motion){
        .correction = saved.correction,
        .accel = saved.accel,
        .inertia_cursor = saved.inertia_cursor,
        .inertia_scroll = saved.inertia_scroll,
        .scroll_only = saved.scroll_only != 0,
        .tap_drag = saved.tap_drag != 0,
    });
    LOG_INF("Trackpad settings loaded: correction %u, accel %u, inertia %u / %u, scroll only %u, "
            "tap and drag %u",
            saved.correction, saved.accel, saved.inertia_cursor, saved.inertia_scroll,
            saved.scroll_only, saved.tap_drag);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mck_tp, "mck/tp", NULL, tp_settings_load_cb, NULL, NULL);

static void tp_save_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct mck_trackpad_motion m;
    mck_trackpad_get_motion(&m);
    struct tp_saved_motion saved = {
        .version = TP_SETTINGS_VERSION,
        .correction = m.correction,
        .accel = m.accel,
        .inertia_cursor = m.inertia_cursor,
        .inertia_scroll = m.inertia_scroll,
        .scroll_only = m.scroll_only ? 1 : 0,
        .tap_drag = m.tap_drag ? 1 : 0,
    };
    int err = settings_save_one(TP_SETTINGS_KEY, &saved, sizeof(saved));
    if (err < 0) {
        LOG_WRN("Failed to save the trackpad settings: %d", err);
    }
}

static K_WORK_DELAYABLE_DEFINE(tp_save_work, tp_save_work_cb);

void mck_trackpad_set_and_save_motion(const struct mck_trackpad_motion *motion) {
    mck_trackpad_set_motion(motion);
    k_work_reschedule(&tp_save_work, K_MSEC(CONFIG_MCK_TRACKPAD_SAVE_DELAY_MS));
}

void mck_trackpad_reset_motion(void) {
    struct mck_trackpad_motion m;
    mck_trackpad_get_default_motion(&m);
    mck_trackpad_set_motion(&m);
    k_work_cancel_delayable(&tp_save_work);
    int err = settings_delete(TP_SETTINGS_KEY);
    if (err < 0) {
        LOG_WRN("Failed to forget the trackpad settings: %d", err);
    }
}

#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
static void tp_ic_save_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct mck_trackpad_ic ic;
    mck_trackpad_get_ic(&ic);
    struct tp_saved_ic saved = {
        .version = TP_IC_SETTINGS_VERSION,
        .stationary_threshold = ic.stationary_threshold,
        .bottom_beta = ic.bottom_beta,
        .lower_speed = ic.lower_speed,
        .idle_touch_ms = ic.idle_touch_ms,
        .tap_time_ms = ic.tap_time_ms,
    };
    int err = settings_save_one(TP_IC_SETTINGS_KEY, &saved, sizeof(saved));
    if (err < 0) {
        LOG_WRN("Failed to save the trackpad IC settings: %d", err);
    }
}

static K_WORK_DELAYABLE_DEFINE(tp_ic_save_work, tp_ic_save_work_cb);

void mck_trackpad_set_and_save_ic(const struct mck_trackpad_ic *ic) {
    mck_trackpad_set_ic(ic);
    k_work_reschedule(&tp_ic_save_work, K_MSEC(CONFIG_MCK_TRACKPAD_SAVE_DELAY_MS));
}

void mck_trackpad_reset_ic(void) {
    struct mck_trackpad_ic def;
    if (mck_trackpad_get_default_ic(&def)) {
        mck_trackpad_set_ic(&def);
    }
    k_work_cancel_delayable(&tp_ic_save_work);
    int err = settings_delete(TP_IC_SETTINGS_KEY);
    if (err < 0) {
        LOG_WRN("Failed to forget the trackpad IC settings: %d", err);
    }
}
#endif
