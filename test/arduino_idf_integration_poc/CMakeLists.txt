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

# ---------------------------------------------------------------------------
# OTA version string: "ota1-<full-git-hash>[-dirty]"
# Incremental-safe: re-evaluated every configure; no token material in defines.
# ---------------------------------------------------------------------------
find_package(Git QUIET)
if(Git_FOUND OR GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE _OTA_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE _GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_GIT_DIRTY)
        set(_OTA_GIT_SUFFIX "-dirty")
    else()
        set(_OTA_GIT_SUFFIX "")
    endif()
else()
    set(_OTA_GIT_COMMIT "unknown")
    set(_OTA_GIT_SUFFIX "")
endif()

if(NOT _OTA_GIT_COMMIT)
    set(_OTA_GIT_COMMIT "unknown")
endif()

# PROJECT_VER: "ota1-" (5) + 12 hex chars + optional "-dirty" (6) = 23 max (< 32 byte IDF limit).
# OTA_GIT_COMMIT: full 40-char hash only (no dirty suffix) for /api/version git_commit field.
string(SUBSTRING "${_OTA_GIT_COMMIT}" 0 12 _SHORT_COMMIT)
set(PROJECT_VER "ota1-${_SHORT_COMMIT}${_OTA_GIT_SUFFIX}")
add_compile_definitions(OTA_GIT_COMMIT="${_OTA_GIT_COMMIT}")

project(esp32s3_arduino_idf_poc)
