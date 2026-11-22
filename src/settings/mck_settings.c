/*
 * MCK runtime settings: battery type and idle / sleep times (MCK Aquarium).
 *
 * - Battery type: saved under the Zephyr setting "mck/bat"; the
 *   mck,aaa-battery driver reads it for its percent curve.
 * - Idle / sleep (CONFIG_MCK_SETTINGS_POWER): ZMK's activity timeouts, set
 *   with zmk_activity_set_idle_ms() / zmk_activity_set_sleep_ms() of
 *   cormoran's ZMK branch main+dya (which also saves them).
 * - Split (CONFIG_MCK_SETTINGS_SPLIT_RELAY): every half keeps its own copy.
 *   The central sends a change to the peripherals as a split relay event
 *   ("mks"), and sends it again when a peripheral (re)connects -- seen as its
 *   battery report going from 0 to a value (ZMK sets it to 0 on disconnect).
 *   Relay pattern after cormoran/zmk-feature-watchdog (MIT).
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

#include <zmk/event_manager.h>

#include <mck/settings.h>

#if IS_ENABLED(CONFIG_MCK_SETTINGS_POWER)
#include <zmk/activity.h>
#endif

#if IS_ENABLED(CONFIG_MCK_SETTINGS_SPLIT_RELAY) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/events/battery_state_changed.h>
#endif
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define HAS_BATTERY_TYPE IS_ENABLED(CONFIG_MCK_AAA_BATTERY)

static enum mck_battery_type battery_type = MCK_BATTERY_TYPE_NIMH;

enum mck_battery_type mck_battery_type_get(void) { return battery_type; }

void mck_settings_get(struct mck_settings_values *out) {
    memset(out, 0, sizeof(*out));
#if IS_ENABLED(CONFIG_MCK_SETTINGS_POWER)
    out->has_power = true;
    out->idle_ms = zmk_activity_get_idle_ms();
    out->sleep_ms = zmk_activity_get_sleep_ms();
#endif
    out->has_battery_type = HAS_BATTERY_TYPE;
    out->battery_type = battery_type;
}

/* ---- saving --------------------------------------------------------------- */

static int mck_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
    if (strcmp(name, "bat") == 0 && len == sizeof(uint8_t)) {
        uint8_t v;
        int rc = read_cb(cb_arg, &v, sizeof(v));
        if (rc >= 0 && v <= MCK_BATTERY_TYPE_ALKALINE) {
            battery_type = (enum mck_battery_type)v;
        }
        return MIN(rc, 0);
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(mck, "mck", NULL, mck_settings_load_cb, NULL, NULL);

/* Applies on this half only. */
static int apply_local(const struct mck_settings_values *v) {
    int err = 0;
#if IS_ENABLED(CONFIG_MCK_SETTINGS_POWER)
    if (v->has_power) {
        zmk_activity_set_idle_ms(v->idle_ms);
        // false when this build has no deep sleep (CONFIG_ZMK_SLEEP=n)
        zmk_activity_set_sleep_ms(v->sleep_ms);
    }
#endif
    if (HAS_BATTERY_TYPE && v->has_battery_type && v->battery_type != battery_type &&
        v->battery_type <= MCK_BATTERY_TYPE_ALKALINE) {
        battery_type = v->battery_type;
        uint8_t raw = (uint8_t)battery_type;
        err = settings_save_one("mck/bat", &raw, sizeof(raw));
        if (err < 0) {
            LOG_WRN("Failed to save the battery type: %d", err);
        }
    }
    return err;
}

/* ---- split relay ---------------------------------------------------------- */

#if IS_ENABLED(CONFIG_MCK_SETTINGS_SPLIT_RELAY)

#define FLAG_POWER BIT(0)
#define FLAG_BATTERY_TYPE BIT(1)

struct mck_settings_relay {
    // ZMK_RELAY_EVENT_SOURCE_SELF when raised on this half
    uint8_t source;
    uint8_t flags;
    uint8_t battery_type;
    uint32_t idle_ms;
    uint32_t sleep_ms;
};

ZMK_EVENT_DECLARE(mck_settings_relay);
ZMK_EVENT_IMPL(mck_settings_relay);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(mck_settings_relay, mks, source);

static void send_to_peripherals(void) {
    struct mck_settings_values v;
    mck_settings_get(&v);
    raise_mck_settings_relay((struct mck_settings_relay){
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
        .flags = (v.has_power ? FLAG_POWER : 0) | (v.has_battery_type ? FLAG_BATTERY_TYPE : 0),
        .battery_type = (uint8_t)v.battery_type,
        .idle_ms = v.idle_ms,
        .sleep_ms = v.sleep_ms,
    });
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
// A peripheral (re)connected: give it the settings again.
static uint8_t last_level[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT];

static void resend_work_cb(struct k_work *work) { send_to_peripherals(); }
static K_WORK_DELAYABLE_DEFINE(resend_work, resend_work_cb);

static int on_peripheral_battery(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev && ev->source < ARRAY_SIZE(last_level)) {
        if (last_level[ev->source] == 0 && ev->state_of_charge > 0) {
            k_work_reschedule(&resend_work, K_SECONDS(2));
        }
        last_level[ev->source] = ev->state_of_charge;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mck_settings_peripheral, on_peripheral_battery);
ZMK_SUBSCRIPTION(mck_settings_peripheral, zmk_peripheral_battery_state_changed);
#endif

#else // peripheral

ZMK_RELAY_EVENT_HANDLE(mck_settings_relay, mks, source);

static int on_settings_relay(const zmk_event_t *eh) {
    const struct mck_settings_relay *ev = as_mck_settings_relay(eh);
    if (!ev || ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    struct mck_settings_values v = {
        .has_power = (ev->flags & FLAG_POWER) != 0,
        .idle_ms = ev->idle_ms,
        .sleep_ms = ev->sleep_ms,
        .has_battery_type = (ev->flags & FLAG_BATTERY_TYPE) != 0,
        .battery_type = (enum mck_battery_type)ev->battery_type,
    };
    LOG_INF("MCK settings from the central: idle %u ms, sleep %u ms, battery %u", v.idle_ms,
            v.sleep_ms, v.battery_type);
    apply_local(&v);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mck_settings_relay_apply, on_settings_relay);
ZMK_SUBSCRIPTION(mck_settings_relay_apply, mck_settings_relay);

#endif // CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#endif // CONFIG_MCK_SETTINGS_SPLIT_RELAY

int mck_settings_set(const struct mck_settings_values *values) {
    int err = apply_local(values);
#if IS_ENABLED(CONFIG_MCK_SETTINGS_SPLIT_RELAY) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    send_to_peripherals();
#endif
    return err;
}
