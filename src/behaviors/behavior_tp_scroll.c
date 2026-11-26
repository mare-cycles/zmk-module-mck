/*
 * Copyright (c) 2026 mare-cycles
 * SPDX-License-Identifier: MIT
 *
 * &tp_scroll: while held, one finger on the mck,iqs5xx trackpad scrolls instead of moving the
 * cursor (scroll only is swapped). 2026-11-26 (D-136 T4).
 */

#define DT_DRV_COMPAT mck_behavior_tp_scroll

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <mck/trackpad.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_tp_scroll_binding_pressed(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    mck_trackpad_scroll_mode(true);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tp_scroll_binding_released(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    mck_trackpad_scroll_mode(false);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_tp_scroll_driver_api = {
    .binding_pressed = on_tp_scroll_binding_pressed,
    .binding_released = on_tp_scroll_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define TP_SCROLL_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_tp_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TP_SCROLL_INST)

#endif
