/*
 * mck__device: model / connection / battery info for MCK Aquarium,
 * over the custom ZMK Studio RPC.
 *
 * Based on the handler of cormoran/zmk-module-template-with-custom-studio-rpc (zmk-module-template:keep)
 * Copyright (c) 2024 The ZMK Contributors
 * Copyright (c) 2026 cormoran
 * Copyright (c) 2026 mare-cycles
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/studio/custom.h>

#include <mck/device/device.pb.h>
#include <mck/version.h>

#if IS_ENABLED(CONFIG_MCK_SETTINGS)
#include <mck/settings.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#define MCK_PERIPHERAL_COUNT ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT
#else
#define MCK_PERIPHERAL_COUNT 0
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MCK_DEVICE_IDENTIFIER "mck__device"

static struct zmk_rpc_custom_subsystem_meta device_subsystem_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/mare-cycles/zmk-module-mck"),
    // Read-only status: no unlock needed.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(mck__device, &device_subsystem_meta, device_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(mck__device, mck_device_Response);

/* ---- state collection --------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && DT_HAS_CHOSEN(zmk_battery)
static const struct device *const battery_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
#endif

static struct zmk_endpoint_instance selected_endpoint(void) {
#ifdef ZMK_ENDPOINT_NONE_COUNT
    // ZMK main (Zephyr 4.x)
    return zmk_endpoint_get_selected();
#else
    // ZMK v0.3 (Zephyr 3.5)
    return zmk_endpoints_selected();
#endif
}

static void fill_central_battery(mck_device_Battery *b) {
    memset(b, 0, sizeof(*b));
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && DT_HAS_CHOSEN(zmk_battery)
    if (!device_is_ready(battery_dev)) {
        return;
    }
    // ZMK's battery.c fetches the sensor periodically; read the cached values.
    struct sensor_value voltage;
    if (sensor_channel_get(battery_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage) == 0) {
        b->millivolts = voltage.val1 * 1000 + voltage.val2 / 1000;
    }
    b->percent = zmk_battery_state_of_charge();
    b->known = b->millivolts > 0 || b->percent > 0;
#endif
}

static void fill_peripheral_batteries(mck_device_Status *s) {
    s->peripheral_batteries_count = MIN(MCK_PERIPHERAL_COUNT, ARRAY_SIZE(s->peripheral_batteries));
    s->peripheral_connected_count = s->peripheral_batteries_count;
    for (pb_size_t i = 0; i < s->peripheral_batteries_count; i++) {
        mck_device_Battery *b = &s->peripheral_batteries[i];
        memset(b, 0, sizeof(*b));
        s->peripheral_connected[i] = false;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
        uint8_t level = 0;
        if (zmk_split_central_get_peripheral_battery_level(i, &level) == 0) {
            // ZMK reports 0 when the peripheral disconnects
            b->known = level > 0;
            b->percent = level;
            s->peripheral_connected[i] = level > 0;
        }
#endif
    }
}

static void fill_status(mck_device_Status *s) {
    // memset (not a compound literal) so padding bytes are zero too: the
    // notification code compares whole structs with memcmp().
    memset(s, 0, sizeof(*s));

    struct zmk_endpoint_instance ep = selected_endpoint();
    switch (ep.transport) {
    case ZMK_TRANSPORT_USB:
        s->transport = mck_device_Transport_TRANSPORT_USB;
        break;
    case ZMK_TRANSPORT_BLE:
        s->transport = mck_device_Transport_TRANSPORT_BLE;
        break;
    default:
        s->transport = mck_device_Transport_TRANSPORT_NONE;
        break;
    }

#if IS_ENABLED(CONFIG_ZMK_BLE)
    int profile = zmk_ble_active_profile_index();
    s->ble_profile_index = profile < 0 ? 0 : (uint32_t)profile;
    s->ble_profile_connected = zmk_ble_active_profile_is_connected();
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
    s->usb_powered = zmk_usb_is_powered();
#endif

    s->has_central_battery = true; // nanopb: sub-messages are skipped unless has_* is set
    fill_central_battery(&s->central_battery);
    fill_peripheral_batteries(s);
}

static void copy_str(char *dst, size_t size, const char *src) {
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/* ---- requests ----------------------------------------------------------- */

static void handle_get_info(mck_device_Response *resp) {
    resp->which_response_type = mck_device_Response_get_info_tag;
    mck_device_GetInfoResponse *r = &resp->response_type.get_info;
    *r = (mck_device_GetInfoResponse)mck_device_GetInfoResponse_init_zero;

    const char *model = CONFIG_MCK_DEVICE_MODEL;
    if (model[0] == '\0') {
        model = CONFIG_ZMK_KEYBOARD_NAME;
    }
    copy_str(r->model, sizeof(r->model), model);
    copy_str(r->board_revision, sizeof(r->board_revision), CONFIG_MCK_DEVICE_BOARD_REVISION);
    copy_str(r->module_version, sizeof(r->module_version), MCK_MODULE_VERSION);
    r->peripheral_count = MCK_PERIPHERAL_COUNT;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    r->ble_profile_count = ZMK_BLE_PROFILE_COUNT;
#endif
}

static void handle_get_status(mck_device_Response *resp) {
    resp->which_response_type = mck_device_Response_get_status_tag;
    fill_status(&resp->response_type.get_status);
}

static void fill_settings(mck_device_Response *resp) {
    resp->which_response_type = mck_device_Response_settings_tag;
    mck_device_Settings *s = &resp->response_type.settings;
    *s = (mck_device_Settings)mck_device_Settings_init_zero;
#if IS_ENABLED(CONFIG_MCK_SETTINGS)
    struct mck_settings_values v;
    mck_settings_get(&v);
    s->power_supported = v.has_power;
    s->idle_ms = v.idle_ms;
    s->sleep_ms = v.sleep_ms;
    s->battery_type_supported = v.has_battery_type;
    s->battery_type = v.battery_type == MCK_BATTERY_TYPE_ALKALINE
                          ? mck_device_BatteryType_BATTERY_TYPE_ALKALINE
                          : mck_device_BatteryType_BATTERY_TYPE_NIMH;
#endif
}

static void handle_set_settings(const mck_device_SetSettingsRequest *req,
                                mck_device_Response *resp) {
#if IS_ENABLED(CONFIG_MCK_SETTINGS)
    struct mck_settings_values now;
    mck_settings_get(&now);
    struct mck_settings_values v = {
        .has_power = now.has_power && req->has_settings,
        .idle_ms = req->settings.idle_ms,
        .sleep_ms = req->settings.sleep_ms,
        .has_battery_type = now.has_battery_type && req->has_settings,
        .battery_type = req->settings.battery_type == mck_device_BatteryType_BATTERY_TYPE_ALKALINE
                            ? MCK_BATTERY_TYPE_ALKALINE
                            : MCK_BATTERY_TYPE_NIMH,
    };
    mck_settings_set(&v);
#else
    ARG_UNUSED(req);
#endif
    fill_settings(resp);
}

static void set_error(mck_device_Response *resp, const char *message) {
    resp->which_response_type = mck_device_Response_error_tag;
    resp->response_type.error = (mck_device_ErrorResponse)mck_device_ErrorResponse_init_zero;
    copy_str(resp->response_type.error.message, sizeof(resp->response_type.error.message),
             message);
}

static bool device_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                      pb_callback_t *encode_response) {
    // Static buffer: the response is encoded after this function returns.
    mck_device_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(mck__device, encode_response);

    mck_device_Request req = mck_device_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, mck_device_Request_fields, &req)) {
        LOG_WRN("Failed to decode mck__device request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case mck_device_Request_get_info_tag:
        handle_get_info(resp);
        break;
    case mck_device_Request_get_status_tag:
        handle_get_status(resp);
        break;
    case mck_device_Request_get_settings_tag:
        fill_settings(resp);
        break;
    case mck_device_Request_set_settings_tag:
        handle_set_settings(&req.request_type.set_settings, resp);
        break;
    default:
        LOG_WRN("Unsupported mck__device request type: %d", req.which_request_type);
        set_error(resp, "Unsupported request");
        break;
    }
    return true;
}

/* ---- notifications ------------------------------------------------------ */

static bool find_subsystem_index(uint8_t *index) {
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);
    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, MCK_DEVICE_IDENTIFIER) == 0) {
            *index = (uint8_t)i;
            return true;
        }
    }
    return false;
}

static bool encode_notification(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const mck_device_Notification *n = (const mck_device_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_submessage(stream, mck_device_Notification_fields, n);
}

static mck_device_Status last_sent_status;
static bool has_sent_status;

static void notify_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    static mck_device_Notification notification;
    notification = (mck_device_Notification)mck_device_Notification_init_zero;
    notification.which_notification_type = mck_device_Notification_status_changed_tag;
    fill_status(&notification.notification_type.status_changed);

    // Skip if nothing the app shows has changed (several events can fire for one change).
    if (has_sent_status && memcmp(&last_sent_status, &notification.notification_type.status_changed,
                                  sizeof(last_sent_status)) == 0) {
        return;
    }

    uint8_t index;
    if (!find_subsystem_index(&index)) {
        return;
    }

    last_sent_status = notification.notification_type.status_changed;
    has_sent_status = true;

    // Encoding runs inside raise_zmk_studio_custom_notification(); the RPC layer
    // drops the notification when no Studio client is connected.
    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = index,
        .encode_payload = {.funcs.encode = encode_notification, .arg = &notification},
    });
}

static K_WORK_DELAYABLE_DEFINE(notify_work, notify_work_cb);

static int mck_device_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    // Merge bursts (e.g. endpoint + profile change) into one notification.
    k_work_reschedule(&notify_work, K_MSEC(CONFIG_MCK_DEVICE_NOTIFY_DELAY_MS));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mck_device, mck_device_listener);
ZMK_SUBSCRIPTION(mck_device, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(mck_device, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(mck_device, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_SUBSCRIPTION(mck_device, zmk_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(mck_device, zmk_peripheral_battery_state_changed);
#endif
