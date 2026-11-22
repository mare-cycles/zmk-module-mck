/*
 * Based on ZMK's app/module/drivers/sensor/battery/battery_voltage_divider.c
 * Copyright (c) 2020 The ZMK Contributors
 *
 * Changes (NiMH / alkaline curves, `cells`, standalone channel_get):
 * Copyright (c) 2026 mare-cycles
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT mck_aaa_battery

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <mck/aaa_battery_curve.h>

#if IS_ENABLED(CONFIG_MCK_SETTINGS)
#include <mck/settings.h>
#endif

/* The percent curve: NiMH, or the type chosen in MCK Aquarium (mck/settings.h). */
static uint8_t aaa_mv_to_pct(uint16_t millivolts, uint8_t cells) {
#if IS_ENABLED(CONFIG_MCK_SETTINGS)
    if (mck_battery_type_get() == MCK_BATTERY_TYPE_ALKALINE) {
        return mck_alkaline_mv_to_pct(millivolts, cells);
    }
#endif
    return mck_nimh_mv_to_pct(millivolts, cells);
}

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "Only one mck,aaa-battery node is supported");

struct aaa_value {
    uint16_t adc_raw;
    uint16_t millivolts;
    uint8_t state_of_charge;
};

struct io_channel_config {
    uint8_t channel;
};

struct aaa_config {
    struct io_channel_config io_channel;
    struct gpio_dt_spec power;
    uint32_t output_ohm;
    uint32_t full_ohm;
    uint8_t cells;
};

struct aaa_data {
    const struct device *adc;
    struct adc_channel_cfg acc;
    struct adc_sequence as;
    struct aaa_value value;
};

static int aaa_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct aaa_data *drv_data = dev->data;
    const struct aaa_config *drv_cfg = dev->config;
    struct adc_sequence *as = &drv_data->as;

    // Make sure selected channel is supported
    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_ALL) {
        LOG_DBG("Selected channel is not supported: %d.", chan);
        return -ENOTSUP;
    }

    int rc = 0;

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    // Enable power before sampling
    rc = gpio_pin_set_dt(&drv_cfg->power, 1);

    if (rc != 0) {
        LOG_DBG("Failed to enable ADC power GPIO: %d", rc);
        return rc;
    }

    // wait for any capacitance to charge up
    k_sleep(K_MSEC(10));
#endif // DT_INST_NODE_HAS_PROP(0, power_gpios)

    // Read ADC
    rc = adc_read(drv_data->adc, as);
    as->calibrate = false;

    if (rc == 0) {
        int32_t val = drv_data->value.adc_raw;

        adc_raw_to_millivolts(adc_ref_internal(drv_data->adc), drv_data->acc.gain, as->resolution,
                              &val);

        uint16_t millivolts = val * (uint64_t)drv_cfg->full_ohm / drv_cfg->output_ohm;
        uint8_t percent = aaa_mv_to_pct(millivolts, drv_cfg->cells);
        LOG_DBG("ADC raw %d ~ %d mV => %d mV (%d cell) => %d%%", drv_data->value.adc_raw, val,
                millivolts, drv_cfg->cells, percent);

        drv_data->value.millivolts = millivolts;
        drv_data->value.state_of_charge = percent;
    } else {
        LOG_DBG("Failed to read ADC: %d", rc);
    }

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    // Disable power GPIO if present
    int rc2 = gpio_pin_set_dt(&drv_cfg->power, 0);

    if (rc2 != 0) {
        LOG_DBG("Failed to disable ADC power GPIO: %d", rc2);
        return rc2;
    }
#endif // DT_INST_NODE_HAS_PROP(0, power_gpios)

    return rc;
}

static int aaa_channel_get(const struct device *dev, enum sensor_channel chan,
                           struct sensor_value *val_out) {
    const struct aaa_data *drv_data = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        val_out->val1 = drv_data->value.millivolts / 1000;
        val_out->val2 = (drv_data->value.millivolts % 1000) * 1000U;
        break;

    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val_out->val1 = drv_data->value.state_of_charge;
        val_out->val2 = 0;
        break;

    default:
        return -ENOTSUP;
    }

    return 0;
}

static const struct sensor_driver_api aaa_api = {
    .sample_fetch = aaa_sample_fetch,
    .channel_get = aaa_channel_get,
};

static int aaa_init(const struct device *dev) {
    struct aaa_data *drv_data = dev->data;
    const struct aaa_config *drv_cfg = dev->config;

    if (drv_data->adc == NULL) {
        LOG_ERR("ADC failed to retrieve ADC driver");
        return -ENODEV;
    }

    int rc = 0;

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    if (!device_is_ready(drv_cfg->power.port)) {
        LOG_ERR("GPIO port for power control is not ready");
        return -ENODEV;
    }
    rc = gpio_pin_configure_dt(&drv_cfg->power, GPIO_OUTPUT_INACTIVE);
    if (rc != 0) {
        LOG_ERR("Failed to control feed %u: %d", drv_cfg->power.pin, rc);
        return rc;
    }
#endif // DT_INST_NODE_HAS_PROP(0, power_gpios)

    drv_data->as = (struct adc_sequence){
        .channels = BIT(0),
        .buffer = &drv_data->value.adc_raw,
        .buffer_size = sizeof(drv_data->value.adc_raw),
        .oversampling = 4,
        .calibrate = true,
    };

#ifdef CONFIG_ADC_NRFX_SAADC
    drv_data->acc = (struct adc_channel_cfg){
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
        .input_positive = SAADC_CH_PSELP_PSELP_AnalogInput0 + drv_cfg->io_channel.channel,
    };

    drv_data->as.resolution = 12;
#else
#error Unsupported ADC
#endif

    rc = adc_channel_setup(drv_data->adc, &drv_data->acc);
    LOG_DBG("AIN%u setup returned %d", drv_cfg->io_channel.channel, rc);

    return rc;
}

static struct aaa_data aaa_data = {.adc = DEVICE_DT_GET(DT_IO_CHANNELS_CTLR(DT_DRV_INST(0)))};

static const struct aaa_config aaa_cfg = {
    .io_channel =
        {
            DT_IO_CHANNELS_INPUT(DT_DRV_INST(0)),
        },
#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    .power = GPIO_DT_SPEC_INST_GET(0, power_gpios),
#endif
    .output_ohm = DT_INST_PROP(0, output_ohms),
    .full_ohm = DT_INST_PROP(0, full_ohms),
    .cells = DT_INST_PROP(0, cells),
};

DEVICE_DT_INST_DEFINE(0, &aaa_init, NULL, &aaa_data, &aaa_cfg, POST_KERNEL,
                      CONFIG_SENSOR_INIT_PRIORITY, &aaa_api);
