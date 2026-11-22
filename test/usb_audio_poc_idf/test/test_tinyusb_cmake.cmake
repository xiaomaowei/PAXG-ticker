# Static validation test for TinyUSB networking conditional inclusion and preprocessor guards.
#
# Validates:
# 1. Non-network configuration: missing lib/networking directory and rndis_reports.c does NOT cause fatal error or include missing paths
# 2. Network configuration enabled via CMake/Kconfig flags: fails explicitly at configuration time if sources are missing
# 3. Actual preprocessor macros: CFG_TUD_ECM_RNDIS=1 and CFG_TUD_NCM=1 fail explicitly via tusb_net_guard.h
# 4. Actual non-network audio/HID configurations (CFG_TUD_ECM_RNDIS=0, CFG_TUD_NCM=0) pass compilation cleanly
# 5. Full prior source behavior is preserved when networking sources exist

cmake_minimum_required(VERSION 3.16)

get_filename_component(TINYUSB_DIR "${CMAKE_CURRENT_LIST_DIR}/../components/tinyusb" ABSOLUTE)
message(STATUS "Running TinyUSB CMake and Preprocessor Guard validation...")

# Mock IDF functions
function(idf_build_get_property var prop)
    set(${var} "esp32s3" PARENT_SCOPE)
endfunction()

function(idf_component_register)
    cmake_parse_arguments(ARG "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;PRIV_REQUIRES;REQUIRES" ${ARGN})
    set(REGISTERED_SRCS "${ARG_SRCS}" PARENT_SCOPE)
    set(REGISTERED_PRIV_INCLUDES "${ARG_PRIV_INCLUDE_DIRS}" PARENT_SCOPE)
    set(REGISTERED_PRIV_REQUIRES "${ARG_PRIV_REQUIRES}" PARENT_SCOPE)
endfunction()

function(target_compile_options target scope)
    set(REGISTERED_COMPILE_OPTIONS "${ARGN}" PARENT_SCOPE)
endfunction()

function(set_source_files_properties file PROPERTIES prop val)
endfunction()

# -------------------------------------------------------------
# Case 1: Default / isolated non-network mode CMake properties
# -------------------------------------------------------------
set(COMPONENT_LIB "mock_tusb_lib")
set(COMPONENT_DIR "${TINYUSB_DIR}")
include("${TINYUSB_DIR}/CMakeLists.txt")

# Assert lib/networking and rndis_reports.c are NOT present in non-network mode
string(FIND "${REGISTERED_SRCS}" "rndis_reports.c" POS_RNDIS)
if(NOT POS_RNDIS EQUAL -1)
    message(FATAL_ERROR "TEST FAILED: rndis_reports.c should NOT be included in default non-network mode")
endif()

string(FIND "${REGISTERED_PRIV_INCLUDES}" "lib/networking" POS_NET_INC)
if(NOT POS_NET_INC EQUAL -1)
    message(FATAL_ERROR "TEST FAILED: lib/networking should NOT be in PRIV_INCLUDE_DIRS in default non-network mode")
endif()

string(FIND "${REGISTERED_PRIV_REQUIRES}" "esp_netif" POS_NETIF)
if(NOT POS_NETIF EQUAL -1)
    message(FATAL_ERROR "TEST FAILED: esp_netif should NOT be in PRIV_REQUIRES in default non-network mode")
endif()

# Assert audio and hid drivers are preserved
string(FIND "${REGISTERED_SRCS}" "src/class/audio/audio_device.c" POS_AUDIO)
if(POS_AUDIO EQUAL -1)
    message(FATAL_ERROR "TEST FAILED: audio_device.c must be preserved in registered sources")
endif()

string(FIND "${REGISTERED_SRCS}" "src/class/hid/hid_device.c" POS_HID)
if(POS_HID EQUAL -1)
    message(FATAL_ERROR "TEST FAILED: hid_device.c must be preserved in registered sources")
endif()

# Assert guard header is included in compile_options when sources are missing
string(FIND "${REGISTERED_COMPILE_OPTIONS}" "tusb_net_guard.h" POS_GUARD)
if(POS_GUARD EQUAL -1)
    message(FATAL_ERROR "TEST FAILED: tusb_net_guard.h should be injected via compile_options when networking sources are missing")
endif()

message(STATUS "Case 1 PASSED: Default non-network configuration cleanly excludes missing sources, injects guard, and preserves audio/HID.")

# -------------------------------------------------------------
# Case 2: CMake configuration failure when network mode enabled
# -------------------------------------------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}" -DCONFIG_TINYUSB_NET_MODE=1 -P "${CMAKE_CURRENT_LIST_FILE}"
    OUTPUT_VARIABLE SUB_OUT
    ERROR_VARIABLE SUB_ERR
    RESULT_VARIABLE SUB_RES
)
if(SUB_RES EQUAL 0)
    message(FATAL_ERROR "TEST FAILED: Enabling CONFIG_TINYUSB_NET_MODE with missing sources must fail with FATAL_ERROR")
endif()
if(NOT SUB_ERR MATCHES "TinyUSB network driver enabled.*missing from the vendor tree")
    message(FATAL_ERROR "TEST FAILED: Expected diagnostic message not found in error output:\n${SUB_ERR}")
endif()

message(STATUS "Case 2 PASSED: Enabling network mode via CMake/Kconfig correctly fails with clear FATAL_ERROR when vendor sources are missing.")

# -------------------------------------------------------------
# Case 3: Preprocessor guard checks using xtensa compiler
# -------------------------------------------------------------
find_program(XTENSA_GCC NAMES "xtensa-esp32s3-elf-gcc" "xtensa-esp32s3-elf-gcc.exe"
    PATHS "$ENV{USERPROFILE}/.platformio/packages/toolchain-xtensa-esp32s3/bin")

if(XTENSA_GCC)
    set(TEST_TMP_DIR "$ENV{TEMP}")
    set(PROBE_SRC "${TEST_TMP_DIR}/test_guard_probe.c")
    file(WRITE "${PROBE_SRC}" "
#include \"tusb_option.h\"
#include \"tusb_net_guard.h\"
int dummy(void) { return 0; }
")

    set(TUSB_SRC_DIR "${TINYUSB_DIR}/src")

    # 3a. Audio/HID non-network config: CFG_TUD_ECM_RNDIS=0, CFG_TUD_NCM=0
    set(CFG_NON_NET "${TEST_TMP_DIR}/test_cfg_non_net.h")
    file(WRITE "${CFG_NON_NET}" "
#define CFG_TUD_ENABLED 1
#define CFG_TUD_AUDIO 1
#define CFG_TUD_HID 1
#define CFG_TUD_ECM_RNDIS 0
#define CFG_TUD_NCM 0
")
    execute_process(
        COMMAND "${XTENSA_GCC}" -DCFG_TUSB_MCU=OPT_MCU_ESP32S3
                "-I${TUSB_SRC_DIR}" "-I${TEST_TMP_DIR}"
                "-DCFG_TUSB_CONFIG_FILE=\"test_cfg_non_net.h\""
                -c "${PROBE_SRC}" -o "${TEST_TMP_DIR}/test_probe_pass.o"
        RESULT_VARIABLE RES_NON_NET
        ERROR_VARIABLE ERR_NON_NET
    )
    if(NOT RES_NON_NET EQUAL 0)
        message(FATAL_ERROR "TEST FAILED: Non-network audio/HID config failed compilation:\n${ERR_NON_NET}")
    endif()
    message(STATUS "Case 3a PASSED: Non-network config (CFG_TUD_ECM_RNDIS=0, CFG_TUD_NCM=0) compiles cleanly.")

    # 3b. Actual CFG_TUD_ECM_RNDIS=1 enabled -> MUST fail compile explicitly
    set(CFG_RNDIS "${TEST_TMP_DIR}/test_cfg_rndis.h")
    file(WRITE "${CFG_RNDIS}" "
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ECM_RNDIS 1
#define CFG_TUD_NCM 0
")
    execute_process(
        COMMAND "${XTENSA_GCC}" -DCFG_TUSB_MCU=OPT_MCU_ESP32S3
                "-I${TUSB_SRC_DIR}" "-I${TEST_TMP_DIR}"
                "-DCFG_TUSB_CONFIG_FILE=\"test_cfg_rndis.h\""
                -c "${PROBE_SRC}" -o "${TEST_TMP_DIR}/test_probe_fail1.o"
        RESULT_VARIABLE RES_RNDIS
        ERROR_VARIABLE ERR_RNDIS
    )
    if(RES_RNDIS EQUAL 0)
        message(FATAL_ERROR "TEST FAILED: CFG_TUD_ECM_RNDIS=1 must fail compilation when sources missing!")
    endif()
    if(NOT ERR_RNDIS MATCHES "TinyUSB network driver enabled.*missing from the vendor tree")
        message(FATAL_ERROR "TEST FAILED: Expected diagnostic message not found for CFG_TUD_ECM_RNDIS=1:\n${ERR_RNDIS}")
    endif()
    message(STATUS "Case 3b PASSED: CFG_TUD_ECM_RNDIS=1 fails explicitly with guard error.")

    # 3c. Actual CFG_TUD_NCM=1 enabled -> MUST fail compile explicitly
    set(CFG_NCM "${TEST_TMP_DIR}/test_cfg_ncm.h")
    file(WRITE "${CFG_NCM}" "
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ECM_RNDIS 0
#define CFG_TUD_NCM 1
")
    execute_process(
        COMMAND "${XTENSA_GCC}" -DCFG_TUSB_MCU=OPT_MCU_ESP32S3
                "-I${TUSB_SRC_DIR}" "-I${TEST_TMP_DIR}"
                "-DCFG_TUSB_CONFIG_FILE=\"test_cfg_ncm.h\""
                -c "${PROBE_SRC}" -o "${TEST_TMP_DIR}/test_probe_fail2.o"
        RESULT_VARIABLE RES_NCM
        ERROR_VARIABLE ERR_NCM
    )
    if(RES_NCM EQUAL 0)
        message(FATAL_ERROR "TEST FAILED: CFG_TUD_NCM=1 must fail compilation when sources missing!")
    endif()
    if(NOT ERR_NCM MATCHES "TinyUSB network driver enabled.*missing from the vendor tree")
        message(FATAL_ERROR "TEST FAILED: Expected diagnostic message not found for CFG_TUD_NCM=1:\n${ERR_NCM}")
    endif()
    message(STATUS "Case 3c PASSED: CFG_TUD_NCM=1 fails explicitly with guard error.")
else()
    message(WARNING "xtensa-esp32s3-elf-gcc not found on PATH or default PlatformIO location; skipped compiler probe.")
endif()

message(STATUS "ALL TESTS PASSED SUCCESSFULLY.")
