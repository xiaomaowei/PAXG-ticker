# Arduino-ESP32 Component Provenance & Modification Record

## 1. Upstream Metadata
- **Upstream Repository**: https://github.com/espressif/arduino-esp32
- **Exact Upstream Tag**: `3.0.7` (Release v3.0.7, targeted for ESP-IDF v5.1.x)
- **Primary License**: GNU Lesser General Public License v2.1 (LGPL-2.1)
  - Core hal, peripheral drivers, and libraries: LGPL-2.1 / Apache 2.0 / MIT.
  - Full license text included in [LICENSE.txt](./LICENSE.txt).

## 2. In-Tree Vendoring Rationale & Fresh Checkout Reproducibility
- In PlatformIO with pure `framework = espidf`, pulling `arduino-esp32` via standard `idf_component.yml` invokes the ESP Component Registry, which frequently pulls unpinned sub-dependencies (e.g. `espressif/lan867x ^2.0.0`) that fail on ESP32-S3.
- To guarantee **100% reproducible offline builds in a fresh git checkout**, the component is vendored in-tree within `test/arduino_idf_integration_poc/components/arduino/` at ~1.87 MB (347 files), completely eliminating runtime network and registry dependencies.

## 3. Applied Modifications & Patches

### Patch 1: Library Pruning (`CMakeLists.txt`)
- **Reason**: Arduino 3.x ships 35+ bundled libraries. Many require complex external IDF components (e.g. `RainMaker`, `Zigbee`, `Matter`, `Insights`, `ESP_SR`), or cause Windows filesystem case-insensitivity collisions (`PPP/src/ppp.c` vs `PPP/src/PPP.cpp`).
- **Modification**: `ARDUINO_ALL_LIBRARIES` in `CMakeLists.txt` is restricted to libraries required for current and projected production features:
  `FS`, `HTTPClient`, `Network`, `NetworkClientSecure`, `Preferences`, `SPI`, `SPIFFS`, `Ticker`, `Update`, `WiFi`, `Wire`.
- 29 unused library directories were pruned from `libraries/`.

### Patch 2: Network Provisioning Component Mapping (`CMakeLists.txt`)
- **Reason**: Upstream packaged manifests reference `espressif__network_provisioning`. In native ESP-IDF 5.1.2, this is the in-tree `wifi_provisioning` component.
- **Modification**: Replaced `espressif__network_provisioning` with `wifi_provisioning` in component requirements.

### Patch 3: Provisioning Wrapper Headers (`cores/esp32/network_provisioning/`)
- **Reason**: Arduino 3.0.7 `NetworkEvents.h` includes `<network_provisioning/network_config.h>` and `<network_provisioning/manager.h>`, whereas ESP-IDF 5.1 exposes `<wifi_provisioning/wifi_config.h>` and `<wifi_provisioning/manager.h>`.
- **Modification**: Added wrapper headers in `cores/esp32/network_provisioning/` mapping types and functions to the in-tree IDF 5.1 `wifi_provisioning` API.

### Patch 4: GCC 12 Switch Warning (`cores/esp32/chip-debug-report.cpp`)
- **Reason**: `ESP_PARTITION_SUBTYPE_DATA_LITTLEFS` (0x83) is not a named enumerator in ESP-IDF 5.1's `esp_partition_subtype_t` enum, triggering `-Werror=switch`.
- **Modification**: Handled value 0x83 in the `default:` branch with `(int)partition->subtype == 0x83`.

### Patch 5: FTM API Version Compatibility (`libraries/WiFi/src/WiFiGeneric.cpp`)
- **Reason**: `wifi_ftm_initiator_cfg_t.use_get_report_api` exists in ESP-IDF 5.1.4+, but does not exist in ESP-IDF 5.1.2.
- **Modification**: Removed `.use_get_report_api = true` from the `ftmi_cfg` structure initialization.

### Patch 6: Variant Pruning (`variants/`)
- **Reason**: Reduce repository weight and prevent unused target confusion.
- **Modification**: Retained only `variants/esp32s3/`, pruned `esp32`, `esp32c2`, `esp32c3`, `esp32c6`, `esp32h2`, `esp32s2`.
