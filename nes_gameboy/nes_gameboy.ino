#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX(void) {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.pin_sclk    = 12;
      cfg.pin_mosi    = 11;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 10;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs       = 13;
      cfg.pin_rst      = 9;
      cfg.pin_busy     = -1;
      cfg.panel_width  = 240;
      cfg.panel_height = 320;
      cfg.readable     = false;
      cfg.invert       = true;
      cfg.rgb_order    = false;
      cfg.bus_shared   = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 14;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("NES GameBoy");
  tft.setCursor(10, 40);
  tft.println("Display OK");

  Serial.println("Display initialized");
}

void loop() {
  tft.fillScreen(TFT_RED);   delay(300);
  tft.fillScreen(TFT_GREEN); delay(300);
  tft.fillScreen(TFT_BLUE);  delay(300);
  tft.fillScreen(TFT_BLACK); delay(300);
}
