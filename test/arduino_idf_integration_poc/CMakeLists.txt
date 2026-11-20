cmake_minimum_required(VERSION 3.16)

# Provide components:
# 1. cmake_utilities from test/usb_audio_poc_idf
# 2. tinyusb from test/usb_audio_poc_idf
# 3. usb_device_composite from test/usb_composite_poc
# 4. components (contains arduino and TFT_eSPI adapter)
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../usb_audio_poc_idf/components/cmake_utilities"
    "${CMAKE_CURRENT_LIST_DIR}/../usb_audio_poc_idf/components/tinyusb"
    "${CMAKE_CURRENT_LIST_DIR}/../usb_composite_poc/components"
    "${CMAKE_CURRENT_LIST_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_arduino_idf_poc)
