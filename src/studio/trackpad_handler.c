/*
 * mck__trackpad: the trackpad (mck,iqs5xx) movement settings for MCK Aquarium,
 * over the custom ZMK Studio RPC. 2026-11-28 (D-136 T5).
 * 2026-11-28 (D-136 T7-1): also the values of the IC itself (GetIc / SetIc / ResetIc).
 *
 * Same shape as device_handler.c (after cormoran/zmk-module-template-with-custom-studio-rpc).
 * Copyright (c) 2026 mare-cycles
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/studio/custom.h>

#include <mck/trackpad.h>
#include <mck/trackpad/trackpad.pb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta trackpad_subsystem_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://github.com/mare-cycles/zmk-module-mck"),
    // Same as mck__device and the ball settings of MCK Aquarium: no unlock needed.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(mck__trackpad, &trackpad_subsystem_meta, trackpad_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(mck__trackpad, mck_trackpad_Response);

static void fill_motion(mck_trackpad_Response *resp) {
    resp->which_response_type = mck_trackpad_Response_motion_tag;
    mck_trackpad_Motion *m = &resp->response_type.motion;
    *m = (mck_trackpad_Motion)mck_trackpad_Motion_init_zero;
    struct mck_trackpad_motion v;
    mck_trackpad_get_motion(&v);
    m->correction = v.correction;
    m->accel = v.accel;
    m->inertia_cursor = v.inertia_cursor;
    m->inertia_scroll = v.inertia_scroll;
    m->scroll_only = v.scroll_only;
}

static void handle_get_info(mck_trackpad_Response *resp) {
    resp->which_response_type = mck_trackpad_Response_get_info_tag;
    mck_trackpad_GetInfoResponse *r = &resp->response_type.get_info;
    *r = (mck_trackpad_GetInfoResponse)mck_trackpad_GetInfoResponse_init_zero;
    struct mck_trackpad_info info;
    mck_trackpad_get_info(&info);
    r->motion_supported = true;
    r->tp_scroll_supported = IS_ENABLED(CONFIG_MCK_BEHAVIOR_TP_SCROLL);
    r->gestures_supported = info.gestures;
    r->gesture_position_count = info.gesture_position_count;
    r->ic_supported = IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING);
}

#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
// 2026-11-28 (D-136 T7-1) the values of the IC itself.
static mck_trackpad_Ic to_pb_ic(const struct mck_trackpad_ic *v) {
    return (mck_trackpad_Ic){
        .stationary_threshold = v->stationary_threshold,
        .bottom_beta = v->bottom_beta,
        .lower_speed = v->lower_speed,
        .idle_touch_ms = v->idle_touch_ms,
        .tap_time_ms = v->tap_time_ms,
    };
}

static void fill_ic(mck_trackpad_Response *resp) {
    resp->which_response_type = mck_trackpad_Response_ic_tag;
    mck_trackpad_IcResponse *r = &resp->response_type.ic;
    *r = (mck_trackpad_IcResponse)mck_trackpad_IcResponse_init_zero;
    struct mck_trackpad_ic v;
    mck_trackpad_get_ic(&v);
    r->has_ic = true;
    r->ic = to_pb_ic(&v);
    r->defaults_read = mck_trackpad_get_default_ic(&v);
    if (r->defaults_read) {
        r->has_defaults = true;
        r->defaults = to_pb_ic(&v);
    }
}

static void handle_set_ic(const mck_trackpad_SetIcRequest *req, mck_trackpad_Response *resp) {
    if (req->has_ic) {
        // Clamped to the ranges in mck_trackpad_set_ic().
        mck_trackpad_set_and_save_ic(&(struct mck_trackpad_ic){
            .stationary_threshold = (uint8_t)MIN(req->ic.stationary_threshold, 255),
            .bottom_beta = (uint8_t)MIN(req->ic.bottom_beta, 255),
            .lower_speed = (uint8_t)MIN(req->ic.lower_speed, 255),
            .idle_touch_ms = (uint16_t)MIN(req->ic.idle_touch_ms, 65535),
            .tap_time_ms = (uint16_t)MIN(req->ic.tap_time_ms, 65535),
        });
    }
    fill_ic(resp);
}
#endif

static void handle_set_motion(const mck_trackpad_SetMotionRequest *req,
                              mck_trackpad_Response *resp) {
    if (req->has_motion) {
        // Clamped to the ranges in mck_trackpad_set_motion().
        mck_trackpad_set_and_save_motion(&(struct mck_trackpad_motion){
            .correction = (uint8_t)MIN(req->motion.correction, 255),
            .accel = (uint8_t)MIN(req->motion.accel, 255),
            .inertia_cursor = (uint8_t)MIN(req->motion.inertia_cursor, 255),
            .inertia_scroll = (uint8_t)MIN(req->motion.inertia_scroll, 255),
            .scroll_only = req->motion.scroll_only,
        });
    }
    fill_motion(resp);
}

static void set_error(mck_trackpad_Response *resp, const char *message) {
    resp->which_response_type = mck_trackpad_Response_error_tag;
    resp->response_type.error = (mck_trackpad_ErrorResponse)mck_trackpad_ErrorResponse_init_zero;
    strncpy(resp->response_type.error.message, message,
            sizeof(resp->response_type.error.message) - 1);
}

static bool trackpad_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                        pb_callback_t *encode_response) {
    // Static buffer: the response is encoded after this function returns.
    mck_trackpad_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(mck__trackpad, encode_response);

    mck_trackpad_Request req = mck_trackpad_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, mck_trackpad_Request_fields, &req)) {
        LOG_WRN("Failed to decode mck__trackpad request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case mck_trackpad_Request_get_info_tag:
        handle_get_info(resp);
        break;
    case mck_trackpad_Request_get_motion_tag:
        fill_motion(resp);
        break;
    case mck_trackpad_Request_set_motion_tag:
        handle_set_motion(&req.request_type.set_motion, resp);
        break;
    case mck_trackpad_Request_reset_motion_tag:
        mck_trackpad_reset_motion();
        fill_motion(resp);
        break;
#if IS_ENABLED(CONFIG_MCK_IQS5XX_IC_TUNING)
    case mck_trackpad_Request_get_ic_tag:
        fill_ic(resp);
        break;
    case mck_trackpad_Request_set_ic_tag:
        handle_set_ic(&req.request_type.set_ic, resp);
        break;
    case mck_trackpad_Request_reset_ic_tag:
        mck_trackpad_reset_ic();
        fill_ic(resp);
        break;
#endif
    default:
        LOG_WRN("Unsupported mck__trackpad request type: %d", req.which_request_type);
        set_error(resp, "Unsupported request");
        break;
    }
    return true;
}
