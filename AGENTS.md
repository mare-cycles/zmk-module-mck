This repository is a ZMK module (zmk-module-mck) using the **unofficial** custom
ZMK Studio RPC protocol (cormoran/zmk). See README.md for what it provides.

## Dev Rules

- One code base builds against both cormoran/zmk `main+custom-studio-protocol`
  (Zephyr 4.x) and `v0.3-branch+custom-studio-protocol` (Zephyr 3.5). Where the
  ZMK API differs, switch at compile time (e.g. `ZMK_ENDPOINT_NONE_COUNT` tells
  the endpoint API apart).
- proto: give every `string` / `repeated` field a size in the `.options` file;
  set `has_<field> = true` for sub-messages; never use 64-bit field types.
- RPC responses are encoded after the handler returns: keep response data in
  static storage (`ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER`).
- Keep the build tests (`tests/zmk-config/build.yaml`) and `test.py` in sync:
  artifact names and expected CONFIG lines.
- Keep the license notices of the files this module is based on.

## Commands

```
# unit test + build test (Linux, after scripts/setup_workspace.sh)
python3 -m unittest
# build test directly
west zmk-build tests/zmk-config
# native_sim test directly
west zmk-test tests -m .
# leftover template placeholders (also runs in pre-commit)
python3 scripts/init_module.py --verify-only
```
