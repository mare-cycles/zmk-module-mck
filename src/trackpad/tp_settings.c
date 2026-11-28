/*
 * Keep the trackpad movement settings (mck,iqs5xx, CONFIG_MCK_IQS5XX_MOTION) in flash:
 * the Zephyr setting "mck/tp/motion". MCK Aquarium changes them through mck__trackpad.
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
#define TP_SETTINGS_VERSION 1

// What is saved. Add fields at the end (and keep reading the shorter old ones).
struct tp_saved_motion {
    uint8_t version;
    uint8_t correction;
    uint8_t accel;
    uint8_t inertia_cursor;
    uint8_t inertia_scroll;
    uint8_t scroll_only;
} __packed;

static int tp_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (strcmp(name, "motion") != 0) {
        return -ENOENT;
    }
    struct tp_saved_motion saved;
    if (len != sizeof(saved)) {
        LOG_WRN("Trackpad settings of an unknown size (%zu), not used", len);
        return 0;
    }
    int rc = read_cb(cb_arg, &saved, sizeof(saved));
    if (rc < 0) {
        return rc;
    }
    if (saved.version != TP_SETTINGS_VERSION) {
        LOG_WRN("Trackpad settings version %u, not used", saved.version);
        return 0;
    }
    mck_trackpad_set_motion(&(struct mck_trackpad_motion){
        .correction = saved.correction,
        .accel = saved.accel,
        .inertia_cursor = saved.inertia_cursor,
        .inertia_scroll = saved.inertia_scroll,
        .scroll_only = saved.scroll_only != 0,
    });
    LOG_INF("Trackpad settings loaded: correction %u, accel %u, inertia %u / %u, scroll only %u",
            saved.correction, saved.accel, saved.inertia_cursor, saved.inertia_scroll,
            saved.scroll_only);
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
