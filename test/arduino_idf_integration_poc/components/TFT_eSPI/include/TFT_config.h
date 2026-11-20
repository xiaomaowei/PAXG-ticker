#ifndef TFT_CONFIG_H
#define TFT_CONFIG_H

#include "sdkconfig.h"

// LilyGo T-Display-S3 ST7789 8-bit parallel bus configuration
// Resolves the Kconfig range limit (-1 to 31) where ESP32-S3 parallel pins 39-48 are used.
#define USER_SETUP_LOADED 1
#define ST7789_DRIVER 1
#define INIT_SEQUENCE_3 1
#define CGRAM_OFFSET 1
#define TFT_RGB_ORDER TFT_RGB
#define TFT_INVERSION_ON 1
#define TFT_PARALLEL_8_BIT 1
#define TFT_WIDTH 170
#define TFT_HEIGHT 320
#define TFT_CS 6
#define TFT_DC 7
#define TFT_RST 5
#define TFT_WR 8
#define TFT_RD 9
#define TFT_D0 39
#define TFT_D1 40
#define TFT_D2 41
#define TFT_D3 42
#define TFT_D4 45
#define TFT_D5 46
#define TFT_D6 47
#define TFT_D7 48
#define TFT_BL 38
#define TFT_BACKLIGHT_ON 1
#define LOAD_GLCD 1
#define LOAD_FONT2 1
#define LOAD_FONT4 1
#define LOAD_FONT6 1
#define LOAD_FONT7 1
#define LOAD_FONT8 1
#define LOAD_GFXFF 1
#define SMOOTH_FONT 1

#endif // TFT_CONFIG_H
