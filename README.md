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
