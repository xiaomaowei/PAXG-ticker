idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "esp32s3")
    set(tusb_mcu "OPT_MCU_ESP32S3")
elseif(${target} STREQUAL "esp32s2")
    set(tusb_mcu "OPT_MCU_ESP32S2")
elseif(${target} STREQUAL "esp32p4")
    set(tusb_mcu "OPT_MCU_ESP32P4")
elseif(${target} STREQUAL "esp32h4")
    set(tusb_mcu "OPT_MCU_ESP32H4")
elseif(${target} STREQUAL "esp32s31")
    set(tusb_mcu "OPT_MCU_ESP32S31")
endif()

set(compile_options
    "-DCFG_TUSB_MCU=${tusb_mcu}"
    )

# If optional networking implementation files are missing from the vendor tree,
# inject a compile-time guard header ensuring CFG_TUD_ECM_RNDIS=1 or CFG_TUD_NCM=1 fails explicitly
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/networking/rndis_reports.c")
    list(APPEND compile_options "-include" "${CMAKE_CURRENT_LIST_DIR}/src/tusb_net_guard.h")
endif()

set(includes_private
    "src/"
    "src/device"
    )

set(includes_public
    "src/"
    )

set(srcs
    "src/class/cdc/cdc_device.c"
    "src/class/hid/hid_device.c"
    "src/class/midi/midi_device.c"
    "src/class/msc/msc_device.c"
    "src/class/vendor/vendor_device.c"
    "src/class/audio/audio_device.c"
    "src/class/video/video_device.c"
    "src/class/bth/bth_device.c"
    "src/class/usbtmc/usbtmc_device.c"
    # DFU
    "src/class/dfu/dfu_device.c"
    "src/class/dfu/dfu_rt_device.c"
    # Common, device-mode related
    "src/portable/synopsys/dwc2/dcd_dwc2.c"
    "src/portable/synopsys/dwc2/dwc2_common.c"
    "src/common/tusb_fifo.c"
    "src/device/usbd_control.c"
    "src/device/usbd.c"
    "src/tusb.c"
    )

set(requirements_private)

# Optional networking class support (ECM / RNDIS / NCM)
# In isolated audio/HID configurations, CFG_TUD_ECM_RNDIS and CFG_TUD_NCM are disabled (0)
# and the vendor tree does not ship lib/networking (rndis_reports.c).
# If networking support is enabled via Kconfig or CMake, ensure required sources exist or fail explicitly.
if(CONFIG_TINYUSB_NET_MODE OR CONFIG_USB_DEVICE_NET OR TINYUSB_NET_ENABLED)
    set(net_rndis_src "${CMAKE_CURRENT_LIST_DIR}/lib/networking/rndis_reports.c")
    if(NOT EXISTS "${net_rndis_src}")
        message(FATAL_ERROR "TinyUSB network driver enabled (CONFIG_TINYUSB_NET_MODE / CONFIG_USB_DEVICE_NET / TINYUSB_NET_ENABLED), but required networking source '${net_rndis_src}' is missing from the vendor tree.")
    endif()

    list(APPEND includes_private "lib/networking")
    list(APPEND srcs
        "src/class/net/ecm_rndis_device.c"
        "lib/networking/rndis_reports.c"
        "src/class/net/ncm_device.c"
    )
    list(APPEND requirements_private esp_netif)
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/networking/rndis_reports.c")
    # If the vendor tree provides lib/networking, include it conditionally for builds with networking support
    list(APPEND includes_private "lib/networking")
    list(APPEND srcs
        "src/class/net/ecm_rndis_device.c"
        "lib/networking/rndis_reports.c"
        "src/class/net/ncm_device.c"
    )
    list(APPEND requirements_private esp_netif)
endif()

if(${target} STREQUAL "esp32p4")
    list(APPEND requirements_private
                esp_mm # required by dcd_dwc2.c: #include "esp_cache.h"
        )
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes_public}
                       PRIV_INCLUDE_DIRS ${includes_private}
                       PRIV_REQUIRES  ${requirements_private}
                       REQUIRES freertos
                       )

target_compile_options(${COMPONENT_LIB} PUBLIC ${compile_options})

# when no builtin class driver is enabled, an uint8_t data compared with `BUILTIN_DRIVER_COUNT` will always be false
set_source_files_properties("src/device/usbd.c" PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
