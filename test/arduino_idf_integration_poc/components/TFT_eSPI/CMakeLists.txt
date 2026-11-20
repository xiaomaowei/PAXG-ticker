set(TFT_UPSTREAM_DIR "${CMAKE_CURRENT_LIST_DIR}/upstream")
set(TFT_ROOT_FALLBACK "${CMAKE_CURRENT_LIST_DIR}/../../../../.pio/libdeps/lilygo-t-display-s3/TFT_eSPI")

if(EXISTS "${TFT_UPSTREAM_DIR}/TFT_eSPI.cpp")
    set(TFT_ROOT "${TFT_UPSTREAM_DIR}")
elseif(EXISTS "${TFT_ROOT_FALLBACK}/TFT_eSPI.cpp")
    set(TFT_ROOT "${TFT_ROOT_FALLBACK}")
else()
    message(STATUS "TFT_eSPI not found locally. Running bootstrap_tft.py...")
    execute_process(
        COMMAND python "${CMAKE_CURRENT_LIST_DIR}/bootstrap_tft.py"
        RESULT_VARIABLE BOOTSTRAP_RES
    )
    if(NOT BOOTSTRAP_RES EQUAL 0 OR NOT EXISTS "${TFT_UPSTREAM_DIR}/TFT_eSPI.cpp")
        message(FATAL_ERROR "Failed to acquire pinned TFT_eSPI v2.5.43 dependency!")
    endif()
    set(TFT_ROOT "${TFT_UPSTREAM_DIR}")
endif()

message(STATUS "TFT_eSPI root resolved to: ${TFT_ROOT}")

idf_component_register(
    SRCS "${TFT_ROOT}/TFT_eSPI.cpp"
    INCLUDE_DIRS "include" "${TFT_ROOT}"
    PRIV_REQUIRES arduino
)
