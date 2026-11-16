# The ESP32-C6 firmware

The installer carries two ESP-Hosted co-processor images, each turned into a header by
`make_c6_header.py` (`python3 make_c6_header.py image.bin output.h [PREFIX]`).

## `c6_firmware.h` (prefix `C6`): ESP-Hosted 2.12.13, the default

- **esp-hosted-mcu v2.12.13** (github.com/espressif/esp-hosted-mcu), its `slave/` project,
  `idf.py set-target esp32c6`, default configuration (SDIO transport, Wi-Fi and Bluetooth),
  built with **ESP-IDF v5.5.5**.
- 1,227,008 bytes, SHA-256 `16b8fff0de8712b896d450da19ce8b027da7494c56b7fa891a8075d061fa51d7`.

That is the ESP-Hosted version the Arduino ESP32 core 3.3.12 builds its host side against
(and the IDF version the core itself was built with). It matches, in version, IDF and
size, the `esp32c6-v2.12.13.bin` Espressif publishes for the core's own online updater;
the two differ only in build path and date.

## `c6_firmware_3x.h` (prefix `C6X`): ESP-Hosted 3.1.0, under Advanced

- **esp-hosted-mcu v3.1.0** (with its git submodules), the `examples/wifi/sta/cp` project
  copied out to its own folder, with its `idf_component.yml` `override_path` pointed at the
  checkout. (ESP-IDF names a locally overridden component after its folder, so the
  checkout must be in a folder named `esp_hosted`.) In `CMakeLists.txt`,
  `set(PROJECT_VER "v3.1.0")` before `project.cmake`: outside the git tree the version
  would otherwise read "1".
- Bluetooth added with an overlay, `sdkconfig.defaults.bt` (from the bluetooth examples'
  controller-only co-processors):

  ```
  CONFIG_BT_ENABLED=y
  CONFIG_BT_CONTROLLER_ONLY=y
  CONFIG_ESP_HOSTED_CP_FEAT_BT=y
  CONFIG_ESP_HOSTED_CP_BT_ENABLED=y
  CONFIG_ESP_HOSTED_CP_FEAT_BT_HCI_VHCI=y
  ```

  built with `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bt"`
  (`set-target esp32c6`, then `build`), **ESP-IDF v5.5.5**. SDIO transport, Wi-Fi and
  Bluetooth.
- 1,361,104 bytes, SHA-256 `41bf7e9a4abc6fce651749c24cb081064e90ea2f466102fede46fabccb7286cc`.

## Going between them

Measured on a JC8012P4A1C, 2026-11-15, with the installer itself (an Arduino sketch, so an
ESP-Hosted 2.12.13 host): 2.12.13 → 3.1.0 → 2.12.13, each step verified by the version the
C6 reported after its restart. A 2.x host can still read a 3.x C6's version and update it.
During an update from 3.x the host logs hundreds of harmless "Dropping packet(s)" errors
(the 3.x C6 sends traffic it doesn't expect); the installer mutes them.

Going back from any *other* 3.x version, or from anything newer than the image being
installed, is untested, and the installer has no copy of that version to put back - which
is why it asks the user to accept the risk.

When a newer Arduino core moves to a newer ESP-Hosted, rebuild the default the same way at
that version.

Apache License 2.0; see `../../LICENSE-Apache-2.0.txt`.
