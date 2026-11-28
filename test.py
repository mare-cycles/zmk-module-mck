from __future__ import annotations

import platform
import shutil
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

THIS_DIR = Path(__file__).parent.resolve()
TEST_BUILD_DIR_NAME = "tests-zmk-module-mck"


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        cls.BUILD_DIR = cls.WEST_TOPDIR / "build"

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        test_build_dir = self.BUILD_DIR / TEST_BUILD_DIR_NAME
        shutil.rmtree(test_build_dir, ignore_errors=True)

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(test_build_dir)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertNotIn("FAILED: ", result.stdout, result.stdout + result.stderr)

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "device_board_feature_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        "CONFIG_ZMK_USB=y",
                        "CONFIG_ZMK_BLE=y",
                        "# CONFIG_MCK_DEVICE is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                    ],
                ),
                "device_board_with_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_MCK_DEVICE=y",
                        "CONFIG_MCK_DEVICE_STUDIO_RPC=y",
                    ],
                    device=[],
                ),
                "device_board_without_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_DEVICE=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_MCK_DEVICE_STUDIO_RPC"),
                    ],
                    device=[],
                ),
                "mck_aaa_battery": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_MCK_DEVICE=y",
                        "CONFIG_MCK_DEVICE_STUDIO_RPC=y",
                        'CONFIG_MCK_DEVICE_MODEL="MCK-TEST"',
                        "CONFIG_MCK_AAA_BATTERY=y",
                        "CONFIG_ZMK_BATTERY_REPORTING=y",
                        "CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE=y",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_aaa_battery",
                    ],
                ),
                "mck_settings": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_DEVICE=y",
                        "CONFIG_MCK_AAA_BATTERY=y",
                        "CONFIG_MCK_SETTINGS=y",
                        "CONFIG_MCK_SETTINGS_POWER=y",
                        "CONFIG_ZMK_SLEEP=y",
                        # not split: nothing to relay
                        NotFound("CONFIG_MCK_SETTINGS_SPLIT_RELAY=y"),
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_aaa_battery",
                    ],
                ),
                "mck_settings_battery_only": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_SETTINGS=y",
                        "# CONFIG_MCK_SETTINGS_POWER is not set",
                    ],
                    device=[],
                ),
                "custom_settings_board": ConfigAndDeviceTree(
                    config=[
                        # Verify that zmk-feature-custom-settings is present and enabled
                        "zmk-feature-custom-settings",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_MCK_DEVICE=y",
                        "CONFIG_MCK_DEVICE_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                    ],
                    device=[],
                ),
                "mck_iqs5xx": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_IQS5XX=y",
                        "CONFIG_MCK_IQS5XX_BLOCK_READ=y",
                        "CONFIG_ZMK_POINTING=y",
                        # On, with everything off by default (same movement as before)
                        "CONFIG_MCK_IQS5XX_MOTION=y",
                        "CONFIG_MCK_IQS5XX_CORRECTION=1",
                        "CONFIG_MCK_IQS5XX_ACCEL=0",
                        "# CONFIG_MCK_IQS5XX_SCROLL_ONLY is not set",
                        NotFound("CONFIG_MCK_BEHAVIOR_TP_SCROLL=y"),
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_iqs5xx",
                    ],
                ),
                "mck_iqs5xx_no_block_read": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_IQS5XX=y",
                        "# CONFIG_MCK_IQS5XX_BLOCK_READ is not set",
                        NotFound("CONFIG_MCK_IQS5XX_GESTURES=y"),
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_iqs5xx",
                    ],
                ),
                "mck_iqs5xx_positions": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_IQS5XX=y",
                        "CONFIG_MCK_IQS5XX_GESTURES=y",
                        "CONFIG_MCK_IQS5XX_FINGER_DATA=y",
                        "CONFIG_MCK_IQS5XX_GESTURE_SWIPE_DIST=200",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_iqs5xx",
                        "_P_gesture_positions_LEN 12",
                        "_P_gesture_release_ms 50",
                    ],
                ),
                "mck_iqs5xx_motion": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_IQS5XX_MOTION=y",
                        "CONFIG_MCK_IQS5XX_CORRECTION=2",
                        "CONFIG_MCK_IQS5XX_ACCEL=5",
                        "CONFIG_MCK_IQS5XX_INERTIA_CURSOR=5",
                        "CONFIG_MCK_IQS5XX_INERTIA_SCROLL=5",
                        "CONFIG_MCK_IQS5XX_SCROLL_ONLY=y",
                        "CONFIG_MCK_BEHAVIOR_TP_SCROLL=y",
                        "CONFIG_MCK_TRACKPAD_SETTINGS=y",
                        "CONFIG_MCK_TRACKPAD_STUDIO_RPC=y",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_iqs5xx",
                        "DT_COMPAT_HAS_OKAY_mck_behavior_tp_scroll",
                    ],
                ),
                "mck_iqs5xx_touch_log": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_MCK_IQS5XX_BLOCK_READ=y",
                        "CONFIG_MCK_IQS5XX_TOUCH_LOG=y",
                        "CONFIG_MCK_IQS5XX_MOTION_LOG=y",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_mck_iqs5xx",
                    ],
                ),
            }
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        for artifact in artifacts_and_expected_build_params.keys():
            shutil.rmtree(self.BUILD_DIR / artifact, ignore_errors=True)

        result = run_west(["zmk-build", "tests/zmk-config", "-q"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_build_params.items():
            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")


if __name__ == "__main__":
    unittest.main()
