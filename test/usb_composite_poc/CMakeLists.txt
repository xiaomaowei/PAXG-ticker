cmake_minimum_required(VERSION 3.16)

# Reuse cmake_utilities and tinyusb from isolated IDF PoC
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../usb_audio_poc_idf/components/cmake_utilities"
    "${CMAKE_CURRENT_LIST_DIR}/../usb_audio_poc_idf/components/tinyusb"
    "${CMAKE_CURRENT_LIST_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_composite_poc)
