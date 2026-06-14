// Arduino-ESP32 compiles at -Os by default. The emulator core is a hot interpreter
// loop, so force -O3 for everything defined in this translation unit (peanut_gb.h
// is #included below, so gb_run_frame is optimised at O3 too).
#pragma GCC optimize("O3")

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>

// ── Sound: Peanut-GB APU via minigb_apu ───────────────────────────────────────
#include "minigb_apu.h"
// Peanut-GB calls these bare hooks when ENABLE_SOUND is set. We declare them
// before peanut_gb.h is included and define them in the Audio section below,
// bridging to the context-based minigb_apu API.
uint8_t audio_read(const uint16_t addr);
void    audio_write(const uint16_t addr, const uint8_t val);

#define ENABLE_SOUND 1
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
#define DISP_H 240          // physical panel height
#define FB_H   216          // rendered framebuffer height (1.5× of GB's 144)
#define FB_Y   12           // top border; sprite pushed here, borders stay black
// Horizontal: 2× (160→320, fills screen width)
// Vertical:   1.5× (144→216, centred with a static 12px black border top/bottom)
// The framebuffer holds only the 216 active rows, so the per-frame SPI push
// sends 216 rows (not 240) — ~10% less data — and the borders are never redrawn.
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
    fb.setBuffer(render_buf[i], DISP_W, FB_H, 16);
    fb.pushSprite(0, FB_Y);
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

// Game picker: list of .gb/.gbc files found in the SD root.
#define MAX_GAMES 64
static char game_list[MAX_GAMES][80];
static int  game_count = 0;

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
  int base_y = (int)(line / 2) * 3;       // 0-based in the 216-row framebuffer
  int y_off  = (line & 1) ? 2 : 0;
  int rows   = (line & 1) ? 1 : 2;
  for (int r = 0; r < rows; r++) {
    int y = base_y + y_off + r;
    if ((unsigned)y < (unsigned)FB_H)
      memcpy(buf + y * (DISP_W * 2), sram_line, DISP_W * 2);
  }
}

// ── Sound output (LEDC PWM → NPN transistor → speaker on GPIO 1) ──────────────
// minigb_apu synthesises 4-channel stereo at 32768 Hz. Each emulated frame we
// mix it to mono, convert every sample to an 8-bit PWM duty, and queue it. A
// 32768 Hz timer ISR pops one duty per tick and writes it to an LEDC PWM
// channel. The transistor switches the speaker at the PWM carrier; the speaker
// coil + 10uF cap low-pass the carrier back into the audio waveform.

#define AUDIO_PIN      1
#define AUDIO_CARRIER  100000   // PWM carrier; below LEDC ceiling, above audible
#define AUDIO_RES_BITS 8
#define AUDIO_GAIN     256      // 256 = unity; raise for louder, lower if harsh
#define AUDIO_OUT_RATE 30000    // ISR consumer rate; lower it toward fps*548 if audio stutters
#define AUDIO_TMR_BASE 8000000  // 8 MHz timer base (clean divider of 80 MHz source)

static struct minigb_apu_ctx apu;
static audio_sample_t audio_stream[AUDIO_SAMPLES_TOTAL];

// Single-producer (emulator core) / single-consumer (timer ISR) ring of duties.
#define ABUF_SIZE 2048          // power of two; holds ~3.7 frames of audio
static volatile uint8_t  abuf[ABUF_SIZE];
static volatile uint32_t ahead = 0, atail = 0;
static hw_timer_t *audio_timer = nullptr;

// Bridge Peanut-GB's sound hooks to the context-based minigb_apu API.
uint8_t audio_read(const uint16_t addr) { return minigb_apu_audio_read(&apu, addr); }
void    audio_write(const uint16_t addr, const uint8_t val) { minigb_apu_audio_write(&apu, addr, val); }

// Timer ISR @ 32768 Hz: emit the next sample's duty to the PWM channel.
static void ARDUINO_ISR_ATTR audio_isr() {
  if (atail != ahead) {
    uint8_t v = abuf[atail];
    atail = (atail + 1) & (ABUF_SIZE - 1);
    ledcWrite(AUDIO_PIN, v);
  }
  // else: ring empty — hold the last duty (brief silence), no underrun crash.
}

// Run once per emulated frame: synthesise audio and queue mono duty values.
static void audioProduce() {
  minigb_apu_audio_callback(&apu, audio_stream);
  for (unsigned i = 0; i < AUDIO_SAMPLES; i++) {
    int32_t m = ((int32_t)audio_stream[2 * i] + audio_stream[2 * i + 1]) >> 1; // L+R → mono
    m = (m * AUDIO_GAIN) >> 8;
    int32_t d = 128 + (m >> 8);          // int16 → 0..255 centred on mid-rail
    if (d < 0) d = 0; else if (d > 255) d = 255;
    uint32_t next = (ahead + 1) & (ABUF_SIZE - 1);
    if (next != atail) { abuf[ahead] = (uint8_t)d; ahead = next; } // drop if full
  }
}

static void audioInit() {
  minigb_apu_audio_init(&apu);
  if (!ledcAttach(AUDIO_PIN, AUDIO_CARRIER, AUDIO_RES_BITS))
    Serial.println("ledcAttach FAILED - lower AUDIO_CARRIER");
  ledcWrite(AUDIO_PIN, 128);             // mid-rail = silence
  audio_timer = timerBegin(AUDIO_TMR_BASE);
  timerAttachInterrupt(audio_timer, &audio_isr);
  timerAlarm(audio_timer, AUDIO_TMR_BASE / AUDIO_OUT_RATE, true, 0);  // ≈32768 Hz
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
  // Prefer fast internal RAM — gb_rom_read() hits the ROM on every opcode fetch,
  // so internal SRAM is far quicker than PSRAM. Fall back to PSRAM for big ROMs
  // that don't fit internally.
  rom_data = (uint8_t *)heap_caps_malloc(rom_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  bool internal = (rom_data != nullptr);
  if (!rom_data) rom_data = (uint8_t *)ps_malloc(rom_size);
  if (!rom_data) { f.close(); return false; }
  f.read(rom_data, rom_size);
  f.close();
  Serial.printf("ROM: %s (%u bytes) in %s\n", path, rom_size, internal ? "internal RAM" : "PSRAM");
  Serial.printf("Free internal heap: %u\n", ESP.getFreeHeap());

  // Derive save path: replace the ROM extension with ".sav".
  strncpy(save_path, path, sizeof(save_path) - 1);
  save_path[sizeof(save_path) - 1] = 0;
  char *dot = strrchr(save_path, '.');
  if (dot && (dot - save_path) < (int)(sizeof(save_path) - 5)) strcpy(dot, ".sav");
  else strncat(save_path, ".sav", sizeof(save_path) - strlen(save_path) - 1);
  return true;
}

// ── Battery saves ─────────────────────────────────────────────────────────────

// Load the .sav file into cart RAM, if one exists and its size matches.
static void loadSave(size_t ram_sz) {
  if (!cart_ram || ram_sz == 0) return;
  File f = SD_MMC.open(save_path, FILE_READ);
  if (!f) { Serial.printf("No save file (%s)\n", save_path); return; }
  if (f.size() == ram_sz) {
    f.read(cart_ram, ram_sz);
    Serial.printf("Loaded save: %s (%u bytes)\n", save_path, (unsigned)ram_sz);
  } else {
    Serial.printf("Save size mismatch (%u vs %u) - ignoring\n",
                  (unsigned)f.size(), (unsigned)ram_sz);
  }
  f.close();
}

// Overwrite the .sav file with current cart RAM. FILE_WRITE ("w") truncates.
static void writeSave() {
  size_t ram_sz = gb_get_save_size(&gb);
  if (!cart_ram || ram_sz == 0) return;
  File f = SD_MMC.open(save_path, FILE_WRITE);
  if (!f) { Serial.printf("Save open failed (%s)\n", save_path); return; }
  size_t w = f.write(cart_ram, ram_sz);
  f.close();
  Serial.printf("Saved: %s (%u bytes)\n", save_path, (unsigned)w);
}

// ── Game picker menu ──────────────────────────────────────────────────────────

static void scanGames() {
  game_count = 0;
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) return;
  File entry;
  while ((entry = root.openNextFile()) && game_count < MAX_GAMES) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".gb") || lower.endsWith(".gbc")) {
        snprintf(game_list[game_count], sizeof(game_list[0]), "/%s", name.c_str());
        Serial.printf("Found: %s\n", game_list[game_count]);
        game_count++;
      }
    }
    entry.close();
  }
}

// True while the button with the given JOYPAD_* mask is held (active-low).
static bool btnDown(uint8_t mask) {
  for (int i = 0; i < NUM_BUTTONS; i++)
    if (buttons[i].mask == mask) return digitalRead(buttons[i].pin) == LOW;
  return false;
}

static void drawMenu(int sel, int top, int visible) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 8);
  tft.printf("Select a game (%d)", game_count);
  int y = 40;
  for (int i = top; i < top + visible && i < game_count; i++) {
    const char *name = game_list[i] + 1;   // skip leading '/'
    if (i == sel) tft.setTextColor(TFT_BLACK, TFT_GREEN);
    else          tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, y);
    tft.printf("%c %s ", (i == sel) ? '>' : ' ', name);
    y += 18;
  }
}

// Blocking menu loop. Returns the selected game index. Runs during setup() while
// disp_task is parked, so the TFT is ours to draw on directly.
static int gameMenu() {
  const int VISIBLE = 11;
  int sel = 0, top = 0;
  bool pUp = false, pDn = false, pOk = false;
  drawMenu(sel, top, VISIBLE);
  for (;;) {
    bool up = btnDown(JOYPAD_UP);
    bool dn = btnDown(JOYPAD_DOWN);
    bool ok = btnDown(JOYPAD_A) || btnDown(JOYPAD_START);
    bool changed = false;
    if (up && !pUp) { sel = (sel - 1 + game_count) % game_count; changed = true; }
    if (dn && !pDn) { sel = (sel + 1) % game_count;             changed = true; }
    if (ok && !pOk) { return sel; }
    pUp = up; pDn = dn; pOk = ok;
    if (changed) {
      if (sel < top)            top = sel;
      if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
      drawMenu(sel, top, VISIBLE);
    }
    delay(30);   // debounce + yield
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  Serial.printf("CPU: %u MHz\n", getCpuFrequencyMhz());

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
  const size_t fb_bytes = DISP_W * FB_H * sizeof(uint16_t);
  for (int i = 0; i < 2; i++) {
    // Put buffer 0 in fast internal DMA RAM (skips the PSRAM write stall in
    // lcd_draw_line on alternate frames); buffer 1 stays in PSRAM. Both don't
    // fit internally alongside the ROM, so this is the best split available.
    render_buf[i] = (i == 0) ? (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA)
                             : nullptr;
    bool got_internal = (render_buf[i] != nullptr);
    if (!render_buf[i]) render_buf[i] = (uint16_t *)ps_malloc(fb_bytes);
    if (!render_buf[i]) {
      Serial.printf("render_buf[%d] ALLOC FAILED\n", i);
      tft.println("FB ALLOC FAIL");
      while (1) delay(1000);
    }
    Serial.printf("render_buf[%d] in %s\n", i, got_internal ? "internal" : "PSRAM");
    memset(render_buf[i], 0, fb_bytes);
    sem_free[i]  = xSemaphoreCreateBinary();
    sem_ready[i] = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_free[i]);
  }
  // Initialise sprite geometry so pushSprite knows the dimensions.
  // setBuffer points it at render_buf[0] for now; disp_task swaps it each frame.
  fb.setColorDepth(16);
  fb.setBuffer(render_buf[0], DISP_W, FB_H, 16);

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

  buttons_init();   // needed for menu navigation
  scanGames();
  if (game_count == 0) { msg("No .gb files on SD!"); while (1) delay(1000); }
  int sel = gameMenu();
  if (!loadROM(game_list[sel])) { msg("ROM load failed!"); while (1) delay(1000); }

  msg("Init emulator...");
  enum gb_init_error_e err = gb_init(&gb, gb_rom_read, gb_cart_ram_read,
                                      gb_cart_ram_write, gb_error, nullptr);
  if (err != GB_INIT_NO_ERROR) { tft.printf("GB init err: %d", err); while (1) delay(1000); }

  size_t ram_sz = gb_get_save_size(&gb);
  if (ram_sz > 0) {
    cart_ram = (uint8_t *)ps_malloc(ram_sz);
    memset(cart_ram, 0xFF, ram_sz);
    loadSave(ram_sz);   // restore previous progress if a .sav exists
  }

  gb_init_lcd(&gb, lcd_draw_line);
  audioInit();
  Serial.println("Running");
}

// ── Loop (Core 1) ─────────────────────────────────────────────────────────────

void loop() {
  static uint32_t last_us  = 0;
  static uint32_t frame_n  = 0;
  static uint32_t fps_time = 0;

  uint32_t now = micros();
  if (now - last_us < 16742) return;   // cap at the Game Boy's native 59.7 fps
  last_us = now;

  buttons_read();
  xSemaphoreTake(sem_free[emu_buf], portMAX_DELAY);
  gb_run_frame(&gb);
  audioProduce();   // synthesise this frame's audio while regs are fresh
  xSemaphoreGive(sem_ready[emu_buf]);
  emu_buf ^= 1;

  // Flush cart RAM to SD once writes have settled (~1.5s idle). Batches the
  // burst of writes a game makes during a save into a single SD write.
  if (cart_ram_dirty && (millis() - last_ram_write_ms) > 1500) {
    cart_ram_dirty = false;
    writeSave();
  }

  frame_n++;
  if (millis() - fps_time >= 5000) {
    Serial.printf("FPS: %.1f\n", frame_n / 5.0f);
    frame_n = 0;
    fps_time = millis();
  }
}
