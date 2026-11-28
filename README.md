# zmk-module-mck

[![Test](https://github.com/mare-cycles/zmk-module-mck/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/mare-cycles/zmk-module-mck/actions/workflows/zmk-module.yml)

ZMK module for the MCK keyboards (MCK-01, MCK-02) and their configurator app, MCK Aquarium.
MCK キーボード用の ZMK の部品です（単 4 電池の残量と、設定アプリ向けの機体の情報）。

It works with any ZMK keyboard, and provides:

- **`mck,aaa-battery`** — battery level of AAA NiMH cells (e.g. eneloop) measured through a
  voltage divider. It plugs into ZMK's normal battery reporting, so the percentage shows up in
  the OS's Bluetooth battery indicator and is relayed from split peripherals as usual.
- **`mck__device`** — a custom ZMK Studio RPC subsystem that answers the model name, board
  revision, connection (USB / Bluetooth profile) and battery levels, and sends a notification
  when they change. With `CONFIG_MCK_SETTINGS` it also reads / changes the battery type and the
  idle / sleep times (shared with split peripherals).
- **`mck,iqs5xx`** — input driver for Azoteq IQS5xx trackpads (GR-Trackpad65, TPS65, TPS43).
  Cursor, IC taps (left / right click) and two finger scroll. It keeps the trackpad module's own
  tuning (auto re-ATI, palm rejection, filters) and only touches the bits it needs.

> Requires ZMK with the **unofficial custom Studio RPC protocol**
> ([cormoran/zmk](https://github.com/cormoran/zmk), branches `main+custom-studio-protocol` or
> `v0.3-branch+custom-studio-protocol`). The battery driver alone also works on plain ZMK.

## Usage

### 1. `config/west.yml`

```yaml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
    - name: mare-cycles
      url-base: https://github.com/mare-cycles
  projects:
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol # or a commit hash
      import: app/west.yml
    - name: zmk-module-mck
      remote: mare-cycles
      revision: main # or a commit hash
  self:
    path: config
```

### 2. Battery: `mck,aaa-battery`

Same wiring and properties as ZMK's `zmk,battery-voltage-divider`; only the
voltage-to-percent curve differs. Example for one AAA cell + boost converter, battery voltage
on XIAO D0 (AIN0) through a 1 MΩ / 470 kΩ divider:

```dts
/ {
    chosen {
        zmk,battery = &vbatt;
    };
};

// Boards that already have a `vbatt` node (e.g. xiao_ble): override it.
&vbatt {
    compatible = "mck,aaa-battery";
    io-channels = <&adc 0>;
    output-ohms = <470000>;
    full-ohms = <(1000000 + 470000)>;
    cells = <1>; // cells in series (default 1)
};
```

```conf
CONFIG_ZMK_BATTERY_REPORTING=y
CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE=y

# Split central: also fetch the peripherals' battery levels and expose them to the host
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y
```

The NiMH curve (per cell, linear between the points) is provisional:

| Voltage per cell | ≥ 1.30 V | 1.20 V | 1.10 V | ≤ 1.00 V |
| ---------------- | -------- | ------ | ------ | -------- |
| Percent          | 100 %    | 50 %   | 10 %   | 0 %      |

### 3. Device info: `mck__device`

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_MCK_DEVICE=y
CONFIG_MCK_DEVICE_MODEL="MCK-02"        # empty: CONFIG_ZMK_KEYBOARD_NAME
CONFIG_MCK_DEVICE_BOARD_REVISION="r1"
```

Enable it on the half that talks to ZMK Studio (the central of a split keyboard).
`CONFIG_MCK_DEVICE_STUDIO_RPC` defaults to `y` when `CONFIG_ZMK_STUDIO=y`.

| Request      | Response                                                                                                                                  |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `get_info`   | model, board revision, module version, number of split peripherals, number of Bluetooth profiles                                         |
| `get_status` | transport (none / USB / BLE), active BLE profile and whether it is connected, USB power, battery of this half (% and mV), peripherals (% and connected) |
| `get_settings` / `set_settings` | battery type (NiMH / alkaline) and idle / sleep times, when `CONFIG_MCK_SETTINGS` is on (see 4.) |

A `Notification { status_changed }` is sent (after `CONFIG_MCK_DEVICE_NOTIFY_DELAY_MS`, default
250 ms, merging bursts) when the battery level, the endpoint, the BLE profile or the USB state
changes. The protocol is in [`proto/mck/device/device.proto`](proto/mck/device/device.proto).

A peripheral counts as connected while its battery report is above 0 % (ZMK sets it to 0 when the
peripheral disconnects).

### 4. Settings: `CONFIG_MCK_SETTINGS`

```conf
# every half
CONFIG_MCK_SETTINGS=y
# idle / sleep times: needs cormoran/zmk main+dya (zmk_activity_set_*)
CONFIG_MCK_SETTINGS_POWER=y
CONFIG_ZMK_SLEEP=y
# split: the central sends changes to the peripherals
CONFIG_ZMK_SPLIT_RELAY_EVENT=y
```

- Battery type: switches the `mck,aaa-battery` curve between NiMH and alkaline (provisional
  points in [`include/mck/aaa_battery_curve.h`](include/mck/aaa_battery_curve.h)); saved as the
  Zephyr setting `mck/bat`.
- Idle / sleep: ZMK's activity timeouts (saved by ZMK).
- Split: each half keeps its own copy. The central sends a change as a split relay event
  (`mks`) and sends it again when a peripheral reconnects.

### 5. Trackpad: `mck,iqs5xx`

```dts
&xiao_i2c {
    status = "okay";

    trackpad: iqs5xx@74 {
        compatible = "mck,iqs5xx";
        reg = <0x74>;
        rdy-gpios = <&xiao_d 3 GPIO_ACTIVE_HIGH>;    // required: the driver reads on RDY
        reset-gpios = <&xiao_d 6 GPIO_ACTIVE_LOW>;
        one-finger-tap;     // BTN_0
        two-finger-tap;     // BTN_1
        scroll;             // two finger scroll -> REL_WHEEL / REL_HWHEEL
        switch-xy;          // GR-Trackpad65's own orientation
        // bottom-beta / stationary-threshold: omit to keep the module's values
    };
};

/ {
    trackpad_listener: trackpad_listener {
        compatible = "zmk,input-listener";
        device = <&trackpad>;
    };
};
```

Enabled automatically by the devicetree node (`CONFIG_MCK_IQS5XX`). `CONFIG_MCK_IQS5XX_BLOCK_READ`
(default y) reads the status registers in one I2C transaction.

**Gestures on key positions.** With `gesture-positions`, a gesture presses a key position
instead of reporting a button, so what it does follows the active layer and can be changed in
ZMK Studio. Add the positions to the matrix transform (rows the kscan never reports) and to the
physical layout, and give them bindings on every layer.

```dts
&trackpad {
    // 1 / 2 finger tap, then 3 / 4 finger tap, 3 finger swipe up / right / down / left,
    // 4 finger swipe up / right / down / left. Entries can be left out from the end.
    gesture-positions = <3 4 5 6  7 8 9 10  11 12 13 14>;
    gesture-release-ms = <100>;   // default
};
```

Three / four finger taps and swipes are detected by the driver (`CONFIG_MCK_IQS5XX_GESTURES`,
default y, active when `gesture-positions` reaches the three finger tap): a tap is a touch of at
most `CONFIG_MCK_IQS5XX_GESTURE_TAP_MS` (400) whose fingers move at most `..._TAP_MOVE` (80); a
swipe fires once when the fingers have moved `..._SWIPE_DIST` (200), from `..._SWIPE_SETTLE_MS`
(100) to `..._SWIPE_MAX_MS` (1000) after the first finger. While 3 or more fingers are down,
no cursor movement, scroll or IC tap is reported.

**Motion** (`CONFIG_MCK_IQS5XX_MOTION`, default y, everything off by default = the movement is
reported as before): `CONFIG_MCK_IQS5XX_CORRECTION` (average of the last N moves, 1-9),
`..._ACCEL` (0-9, the speed is measured per time), `..._INERTIA_CURSOR` / `..._INERTIA_SCROLL`
(0-9: after lifting a quick move, it goes on and slows down by 1 - 1 / (1 + length) each step;
touching stops it) and `..._SCROLL_ONLY` (one finger scrolls). `&tp_scroll` swaps scroll only
while held:

```dts
#include <behaviors/mck_trackpad.dtsi>   // in the keymap, then bind &tp_scroll
```

These are only the starting values: with ZMK Studio, the **`mck__trackpad`** custom RPC
subsystem (`CONFIG_MCK_TRACKPAD_STUDIO_RPC`, default y; `proto/mck/trackpad/trackpad.proto`)
reads and changes them at runtime. A change takes effect at once and is saved
`CONFIG_MCK_TRACKPAD_SAVE_DELAY_MS` (1000) later in the setting `mck/tp/motion`
(`CONFIG_MCK_TRACKPAD_SETTINGS`); `ResetMotion` goes back to the Kconfig values.

Some values of the **IC itself** can be changed the same way (`CONFIG_MCK_IQS5XX_IC_TUNING`,
default y; `GetIc` / `SetIc` / `ResetIc`, saved in `mck/tp/ic`): the stationary touch
threshold (0x0672), the dynamic XY filter's bottom beta and lower speed (0x0637 / 0x0638), the
idle touch rate (0x057C) and the tap time (0x06B9). Slow, fine moves come in steps when a touch
inside the stationary threshold drops the IC to the idle touch rate and the filter damps hard
below the lower speed. The IC only listens in its communication window, so new values are
written on the next touch. Until changed, the IC keeps its own values (read at setup and
returned as `defaults`); `ResetIc` goes back to them. A board can start from other values with
the devicetree properties `bottom-beta`, `stationary-threshold`, `idle-touch-rate-ms` and
`tap-time-ms` (written at setup, so they are then the `defaults`).

**Tap and drag** (`CONFIG_MCK_IQS5XX_TAP_DRAG`, default y; on at first with
`CONFIG_MCK_IQS5XX_TAP_DRAG_ON`, then the movement setting `tap_drag`): a one finger touch that
starts within 150 ms of a one finger tap and stays still for 300 ms (net movement below 8) keeps
the one finger tap's key position pressed until the finger lifts. A touch that moves first is an
ordinary move (tapping and moving on at once must not drag), and one that lifts at once is still
a tap, so a double tap stays a double click.

`CONFIG_MCK_IQS5XX_TOUCH_LOG` (for a USB logging build) logs every touch: the finger count as
it changes, and when all fingers are up, the most fingers, the duration, how far their center
moved and the gestures the IC reported. `CONFIG_MCK_IQS5XX_MOTION_LOG` logs every move report
and inertia step.

## Development

Tests run on GitHub Actions (`.github/workflows/zmk-module.yml`): build tests in
`tests/zmk-config/` (checked by `test.py`) and the native_sim test in `tests/studio/`.

Local setup (Linux, isolated layout; dependencies go to `./dependencies`):

```bash
bash scripts/setup_workspace.sh
python3 -m unittest            # build tests + native_sim test
west zmk-build tests/zmk-config
west zmk-test tests -m .
```

## License

MIT — see [LICENSE](LICENSE).

- Created from [cormoran/zmk-module-template-with-custom-studio-rpc](https://github.com/cormoran/zmk-module-template-with-custom-studio-rpc) (MIT). <!-- zmk-module-template:keep -->
- `src/battery/aaa_battery.c` is based on ZMK's `battery_voltage_divider.c` (MIT, © The ZMK Contributors).
- `src/trackpad/iqs5xx.c` / `iqs5xx.h` are based on [AYM1607/zmk-driver-azoteq-iqs5xx](https://github.com/AYM1607/zmk-driver-azoteq-iqs5xx) 27321f0 (MIT, © 2025 Mariano Uvalle; full text in [`src/trackpad/LICENSE.iqs5xx`](src/trackpad/LICENSE.iqs5xx)). Changes are marked `GRT65W:` / `MCK:`.
