// TFT_eSPI config for FREENOVE ESP32-S3 WROOM + ILI9341 240x320 display
// Place this file in the sketch folder alongside the .ino

#define USER_SETUP_LOADED

#define ILI9341_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   13
#define TFT_DC   10
#define TFT_RST   9
#define TFT_BL   14

#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

#define SPI_FREQUENCY     40000000
#define SPI_READ_FREQUENCY 20000000
