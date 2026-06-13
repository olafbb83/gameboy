#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

#define ENABLE_SOUND 0
#include "peanut_gb.h"

// ── Display ───────────────────────────────────────────────────────────────────

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host=SPI2_HOST; cfg.spi_mode=0; cfg.freq_write=80000000;
      cfg.freq_read=16000000; cfg.pin_sclk=12; cfg.pin_mosi=11;
      cfg.pin_miso=-1; cfg.pin_dc=10; cfg.dma_channel=SPI_DMA_CH_AUTO;
      _bus.config(cfg); _panel.setBus(&_bus); }
    { auto cfg = _panel.config();
      cfg.pin_cs=13; cfg.pin_rst=9; cfg.panel_width=240; cfg.panel_height=320;
      cfg.readable=false; cfg.invert=true; cfg.rgb_order=false; cfg.bus_shared=false;
      _panel.config(cfg); }
    { auto cfg = _light.config();
      cfg.pin_bl=14; cfg.invert=false; cfg.freq=44100; cfg.pwm_channel=7;
      _light.config(cfg); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

static LGFX tft;
// One sprite is kept solely so we can call pushSprite with a PSRAM-backed buffer.
// We allocate its pixel buffer ourselves from PSRAM; the sprite just wraps it.
static LGFX_Sprite fb(&tft);   // single sprite for display push

// ── Emulator layout ───────────────────────────────────────────────────────────

#define GB_W   LCD_WIDTH    // 160
#define GB_H   LCD_HEIGHT   // 144
#define DISP_W 320
#define DISP_H 240
// Horizontal: 2× (160→320, fills screen width)
// Vertical:   1.5× (144→216, fits in 240 with 12px border top/bottom)
// Pattern: every 2 GB lines → 3 display rows; even GB lines doubled, odd single

// ── Double buffering via two raw PSRAM pixel arrays ───────────────────────────
// Core 1 (loop): renders into render_buf[emu_buf]
// Core 0 (disp_task): pushes completed render_buf via the sprite

static uint16_t *render_buf[2];   // two PSRAM pixel arrays, 153 600 bytes each
static volatile int emu_buf = 0;

static SemaphoreHandle_t sem_free[2];   // buf[i] ready to be rendered into
static SemaphoreHandle_t sem_ready[2];  // buf[i] has a complete frame

static void disp_task(void *) {
  int i = 0;
  for (;;) {
    xSemaphoreTake(sem_ready[i], portMAX_DELAY);
    fb.setBuffer(render_buf[i], DISP_W, DISP_H, 16);
    fb.pushSprite(0, 0);
    xSemaphoreGive(sem_free[i]);
    i ^= 1;
  }
}

// ── Emulator state ────────────────────────────────────────────────────────────

static uint16_t dmg_palette[4];
static uint8_t *rom_data  = nullptr;
static size_t   rom_size  = 0;
static uint8_t *cart_ram  = nullptr;
static struct gb_s gb;

// Battery-save state: cart RAM is mirrored to a .sav file on the SD card.
static char     save_path[80]      = {0};
static volatile bool     cart_ram_dirty    = false;
static volatile uint32_t last_ram_write_ms = 0;

// ── Peanut-GB callbacks ───────────────────────────────────────────────────────

uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) { return rom_data[addr]; }
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) { return cart_ram ? cart_ram[addr] : 0xFF; }
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
  if (cart_ram) {
    cart_ram[addr] = val;
    cart_ram_dirty = true;
    last_ram_write_ms = millis();
  }
}
void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t val) { Serial.printf("GB error %d val %04X\n", err, val); }

void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[GB_W], const uint_fast8_t line) {
  // Build 2x-wide scanline in fast SRAM (160 uint32_t = 320 uint16_t pixels).
  static uint32_t sram_line[GB_W];
  for (int x = 0; x < GB_W; x++) {
    uint32_t c = dmg_palette[pixels[x] & 3];
    sram_line[x] = c | (c << 16);
  }
  // 1.5x vertical: pairs of GB lines map to 3 display rows.
  // Even GB line → 2 display rows (base+0, base+1)
  // Odd  GB line → 1 display row  (base+2)
  // Result: 216 display rows, Y offset 12 (centered in 240).
  uint8_t *buf = (uint8_t *)render_buf[emu_buf];
  int base_y = 12 + (int)(line / 2) * 3;
  int y_off  = (line & 1) ? 2 : 0;
  int rows   = (line & 1) ? 1 : 2;
  for (int r = 0; r < rows; r++) {
    int y = base_y + y_off + r;
    if ((unsigned)y < (unsigned)DISP_H)
      memcpy(buf + y * (DISP_W * 2), sram_line, DISP_W * 2);
  }
}

// ── Buttons ───────────────────────────────────────────────────────────────────
// Each button is wired GPIO → GND with the ESP32 internal pull-up enabled, so a
// pressed button reads LOW. gb.direct.joypad is also active-low (bit clear =
// pressed, 0xFF = nothing pressed), so a pressed pin simply clears its mask.

struct btn_map { uint8_t pin; uint8_t mask; };
static const btn_map buttons[] = {
  {  4, JOYPAD_UP     },
  {  5, JOYPAD_DOWN   },
  {  6, JOYPAD_LEFT   },
  {  7, JOYPAD_RIGHT  },
  { 15, JOYPAD_A      },
  { 16, JOYPAD_B      },
  { 17, JOYPAD_START  },
  { 18, JOYPAD_SELECT },
};
static const int NUM_BUTTONS = sizeof(buttons) / sizeof(buttons[0]);

static void buttons_init() {
  for (int i = 0; i < NUM_BUTTONS; i++) pinMode(buttons[i].pin, INPUT_PULLUP);
}

static void buttons_read() {
  uint8_t jp = 0xFF;
  for (int i = 0; i < NUM_BUTTONS; i++)
    if (digitalRead(buttons[i].pin) == LOW) jp &= ~buttons[i].mask;
  gb.direct.joypad = jp;
}

// ── ROM loading ───────────────────────────────────────────────────────────────

static bool loadROM(const char *path) {
  File f = SD_MMC.open(path);
  if (!f) return false;
  rom_size = f.size();
  rom_data = (uint8_t *)ps_malloc(rom_size);
  if (!rom_data) { f.close(); return false; }
  f.read(rom_data, rom_size);
  f.close();
  Serial.printf("ROM: %s (%u bytes)\n", path, rom_size);
  return true;
}

static bool findAndLoadROM() {
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) return false;
  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) {
      String path = "/" + String(entry.name());
      entry.close();
      if (path.endsWith(".gb") || path.endsWith(".GB") ||
          path.endsWith(".gbc") || path.endsWith(".GBC")) {
        Serial.printf("Found: %s\n", path.c_str());
        return loadROM(path.c_str());
      }
    } else { entry.close(); }
  }
  return false;
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  // Classic DMG green palette.
  // Pre-bswapped so pushSprite DMA (which sends bytes in memory order) delivers
  // the correct big-endian byte sequence that ST7789 expects over SPI.
  dmg_palette[0] = __builtin_bswap16(tft.color565(0xE0, 0xF8, 0xD0));
  dmg_palette[1] = __builtin_bswap16(tft.color565(0x88, 0xC0, 0x70));
  dmg_palette[2] = __builtin_bswap16(tft.color565(0x34, 0x68, 0x56));
  dmg_palette[3] = __builtin_bswap16(tft.color565(0x08, 0x18, 0x20));

  // Allocate two pixel buffers from PSRAM (no LovyanGFX sprite allocation limit)
  Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
  for (int i = 0; i < 2; i++) {
    render_buf[i] = (uint16_t *)ps_malloc(DISP_W * DISP_H * sizeof(uint16_t));
    if (!render_buf[i]) {
      Serial.printf("render_buf[%d] ALLOC FAILED\n", i);
      tft.println("PSRAM ALLOC FAIL");
      while (1) delay(1000);
    }
    memset(render_buf[i], 0, DISP_W * DISP_H * 2);
    sem_free[i]  = xSemaphoreCreateBinary();
    sem_ready[i] = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_free[i]);
  }
  // Initialise sprite geometry so pushSprite knows the dimensions.
  // setBuffer points it at render_buf[0] for now; disp_task swaps it each frame.
  fb.setColorDepth(16);
  fb.setBuffer(render_buf[0], DISP_W, DISP_H, 16);

  // Fully deinit the task WDT. disp_task runs continuously on Core 0 and
  // starves IDLE0; LovyanGFX also calls esp_task_wdt_reset() internally.
  // Neither plays well with the TWDT, and we don't need it for a game console.
  esp_task_wdt_deinit();
  xTaskCreatePinnedToCore(disp_task, "disp", 4096, NULL, 2, NULL, 0);

  auto msg = [](const char *s) {
    static int y = 10;
    tft.setCursor(10, y); tft.println(s); y += 20;
  };

  msg("Init SD...");
  SD_MMC.setPins(39, 38, 40);
  if (!SD_MMC.begin("/sdcard", true)) { msg("SD FAILED"); while (1) delay(1000); }

  msg("Loading ROM...");
  if (!findAndLoadROM()) { msg("No .gb file on SD!"); while (1) delay(1000); }

  msg("Init emulator...");
  enum gb_init_error_e err = gb_init(&gb, gb_rom_read, gb_cart_ram_read,
                                      gb_cart_ram_write, gb_error, nullptr);
  if (err != GB_INIT_NO_ERROR) { tft.printf("GB init err: %d", err); while (1) delay(1000); }

  size_t ram_sz = gb_get_save_size(&gb);
  if (ram_sz > 0) {
    cart_ram = (uint8_t *)ps_malloc(ram_sz);
    memset(cart_ram, 0xFF, ram_sz);
  }

  gb_init_lcd(&gb, lcd_draw_line);
  buttons_init();
  Serial.println("Running");
}

// ── Loop (Core 1) ─────────────────────────────────────────────────────────────

void loop() {
  static uint32_t last_us  = 0;
  static uint32_t frame_n  = 0;
  static uint32_t fps_time = 0;

  uint32_t now = micros();
  if (now - last_us < 16742) return;
  last_us = now;

  buttons_read();
  xSemaphoreTake(sem_free[emu_buf], portMAX_DELAY);
  gb_run_frame(&gb);
  xSemaphoreGive(sem_ready[emu_buf]);
  emu_buf ^= 1;

  frame_n++;
  if (millis() - fps_time >= 5000) {
    Serial.printf("FPS: %.1f\n", frame_n / 5.0f);
    frame_n = 0;
    fps_time = millis();
  }
}
