# ESP32-S3 Game Boy Emulator

A fully playable Nintendo Game Boy (DMG) emulator running on a **FREENOVE ESP32-S3 WROOM**
with a 240×320 ST7789 SPI display, SD-card ROM storage, and physical buttons. Built on the
header-only [Peanut-GB](https://github.com/deltabeard/Peanut-GB) emulator core and the
[LovyanGFX](https://github.com/lovyan03/LovyanGFX) display library.

**~56–57 fps**, locking to the native 59.7 fps cap on lighter scenes (Game Boy native is
59.7 fps), color-accurate 4-shade DMG output, mono sound, on-device game picker, and
battery saves to the SD card.

---

## Status

| Feature | State |
|---------|-------|
| CPU / PPU emulation (Peanut-GB) | ✅ Working — `cpu_instrs.gb` passes all tests |
| ST7789 display @ ~56–57 fps (caps at 59.7) | ✅ Working |
| Dual-core double buffering | ✅ Working |
| 8-button input (D-pad + A/B/Start/Select) | ✅ Working |
| Battery saves (`.sav` on SD) | ✅ Working (for cart-RAM games) |
| On-device game picker menu | ✅ Working |
| Sound (mono, PWM → transistor → speaker) | ✅ Working |
| Color (Game Boy Color) | ❌ Not supported (Peanut-GB is DMG-only) |

Tested games: Tetris, Super Mario Land (both DMG, 4-shade green).

---

## Hardware

**Board:** FREENOVE ESP32-S3 WROOM (selected as *ESP32S3 Dev Module* in Arduino IDE)
- 8 MB PSRAM (OPI), 8 MB Flash
- Dual-core Xtensa LX7 @ 240 MHz

### Display — 240×320 ST7789 (SPI)

| Signal | GPIO |
|--------|------|
| RST  | 9  |
| DC   | 10 |
| MOSI | 11 |
| SCLK | 12 |
| CS   | 13 |
| BL (backlight) | 14 |

SPI2_HOST, 80 MHz write clock, DMA. Panel configured with `invert=true`, `rgb_order=false`,
landscape orientation `rotation=3` (180°-flipped so the image is upright for the chosen layout).

### SD card — SD_MMC, 1-bit mode

| Signal | GPIO |
|--------|------|
| CLK   | 39 |
| CMD   | 38 |
| DATA0 | 40 |

`SD_MMC.setPins(39, 38, 40)` then `SD_MMC.begin("/sdcard", true)` (1-bit mode).

### Buttons — 8× momentary, active-low

Each button connects its GPIO directly to **GND**. The ESP32 internal pull-ups are enabled
(`INPUT_PULLUP`), so an unpressed pin reads HIGH and a pressed pin reads LOW. No external
resistors needed.

| Button | GPIO | Joypad bit |
|--------|------|------------|
| D-pad Up    | 4  | `JOYPAD_UP` |
| D-pad Down  | 5  | `JOYPAD_DOWN` |
| D-pad Left  | 6  | `JOYPAD_LEFT` |
| D-pad Right | 7  | `JOYPAD_RIGHT` |
| A      | 15 | `JOYPAD_A` |
| B      | 16 | `JOYPAD_B` |
| Start  | 17 | `JOYPAD_START` |
| Select | 18 | `JOYPAD_SELECT` |

> The "+" cross on a real Game Boy is one physical rocker but **four electrical switches** —
> one per direction. Wire it as four buttons (GPIO 4/5/6/7).

These GPIOs were chosen to avoid the display, SD, OPI-PSRAM/flash (26–37), USB-JTAG (19/20),
UART0 bridge (43/44), and strapping pins (0/3/45/46).

### Audio — GPIO 1 → transistor → speaker

The ESP32-S3 has no DAC, so audio is generated as a high-frequency PWM (1-bit
"class-D" style) on **GPIO 1** and driven into a 2 W 8 Ω speaker through a single
NPN transistor. The speaker coil + 10 µF cap low-pass the PWM carrier back into sound.

```
GPIO 1 ──[1kΩ]── base
                 NPN (e.g. 2N2222 / S8050 / BC547)
        +5V ──┬── speaker ── collector
              │              emitter ── GND
              └── diode (cathode → +5V) across speaker   (flyback protection)
                  10 µF AC-couples the speaker
```

| Part | Purpose |
|------|---------|
| NPN transistor | switches the speaker current the GPIO can't source |
| 1 kΩ resistor  | limits base current from GPIO 1 |
| diode          | catches the speaker coil's inductive kickback |
| 10 µF cap      | AC-couples the audio / blocks DC |

---

## Software setup (Arduino IDE)

### Libraries
- **LovyanGFX** (Library Manager)
- **Peanut-GB** — `peanut_gb.h` is included in the sketch folder (header-only)
- **minigb_apu** — `minigb_apu.c` / `minigb_apu.h` in the sketch folder (APU sound synthesis)

### Board settings (critical — these are known-good)
| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| Flash Size | 8MB (64Mb) |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | 8M with spiffs (3MB APP/1.5MB SPIFFS) |
| **USB CDC On Boot** | **Disabled** |
| USB Mode | Hardware CDC and JTAG |
| Upload Speed | 921600 |

> **Why USB CDC must be Disabled:** this board has a hardware UART-to-USB bridge (CH340/CH343)
> on UART0 (GPIO43/44). ROM messages and `Serial` both go through it. Enabling native USB CDC
> routes `Serial` to a different COM port and the serial monitor shows nothing.
>
> A double boot message on serial connect is normal — the bridge toggles RTS, resetting the board.

---

## Usage

1. Format an SD card (FAT32) and copy one or more `.gb` / `.gbc` ROMs to the **root**.
2. Insert the card, power on the board.
3. The **game picker** appears. Navigate with **Up/Down**, select with **A** or **Start**.
4. The game boots. Play with the D-pad and A/B/Start/Select.
5. Saves are written automatically to a `.sav` file next to the ROM (see below).

Serial monitor (115200 baud) logs ROM info, save activity, and an FPS counter every 5 s.

> **Use legal homebrew ROMs.** Free, legitimate Game Boy games are available from itch.io,
> GB Studio, and Homebrew Hub. Commercial ROMs (Tetris, Mario, Pokémon, etc.) remain under
> copyright.

---

## How it works

### Display pipeline & performance

The native Game Boy frame is 160×144. It's scaled to fill the 320×240 panel:
- **Horizontal: 2×** (160 → 320) — each pixel written twice via `c | (c << 16)`.
- **Vertical: 1.5×** (144 → 216) — every 2 GB scanlines map to 3 display rows (even line
  doubled, odd line single). The framebuffer holds **only the 216 active rows** and is
  pushed at screen y=12, so the 12 px top/bottom black borders are drawn once and never
  re-sent (trims ~10% off every SPI push).

**Color byte order:** the palette values are pre-swapped with `__builtin_bswap16()`. The
display expects big-endian RGB565 over SPI, but `pushSprite` DMA sends bytes in memory
(little-endian) order — pre-swapping the palette makes the bytes land correctly. (Skipping
this was the cause of the early blue/pink tint bugs.)

**Dual-core double buffering** keeps the framerate up:
- **Core 1** (Arduino `loop`): runs the emulator, renders each scanline into one of two
  PSRAM pixel buffers.
- **Core 0** (`disp_task`): pushes the completed buffer to the display via DMA.
- Two binary-semaphore pairs (`sem_free` / `sem_ready`) hand buffers back and forth.

While Core 0 is busy DMA-ing frame *N*, Core 1 is already emulating frame *N+1*.

**Memory trick:** LovyanGFX's `createSprite()` allocates from internal DMA RAM, which can't
hold two framebuffers. Instead we allocate two raw `uint16_t*` buffers ourselves and use a
single sprite with `setBuffer()` to point at whichever buffer is ready.

**Performance work** (took it from 21 → ~57 fps):
1. *Dual-core double buffering* — emulate and push in parallel. 21 → 35 → ~52 fps.
2. *ROM in internal RAM* — `gb_rom_read()` hits the ROM on every opcode fetch; internal SRAM
   is far faster than PSRAM. `heap_caps_malloc(MALLOC_CAP_INTERNAL)` with a PSRAM fallback for
   ROMs too big to fit. **~42 → ~52 fps** (biggest single win).
3. *Asymmetric framebuffers* — `render_buf[0]` in internal DMA RAM, `render_buf[1]` in PSRAM.
   Both can't fit internally alongside a 64 KB ROM, so this halves the PSRAM-write stall.
4. *`-O3`* — Arduino defaults to `-Os`; a `#pragma GCC optimize("O3")` over the emulator and
   APU translation units sped up the hot interpreter loop. ~1.5 ms/frame.
5. *Push only 216 active rows* — see above; trims the SPI transfer and frees ~15 KB internal.

Result: emulation-bound at ~14–16 ms/frame, locking to the 59.7 fps cap on lighter scenes.
The remaining gap on heavy scenes is the one PSRAM framebuffer — fitting *both* in internal
RAM (only possible without a large ROM) would lock 59.7 everywhere.

**Watchdog:** `disp_task` occupies Core 0 continuously, starving the idle task, and
LovyanGFX calls `esp_task_wdt_reset()` internally. The Task Watchdog Timer is fully disabled
with `esp_task_wdt_deinit()` — correct for a console that intentionally pins a core.

### Input

`buttons_read()` runs once per frame: it reads all 8 pins and builds the active-low
`gb.direct.joypad` byte (bit clear = pressed). Because both the wiring and the joypad
register are active-low, a pressed pin simply clears its `JOYPAD_*` mask — no inversion.

### Sound

`minigb_apu` synthesizes the Game Boy's 4 audio channels as 32768 Hz stereo. The flow:
- Peanut-GB's `audio_read` / `audio_write` hooks bridge into the context-based
  `minigb_apu` API (the two are separate versions, so thin wrappers connect them).
- Each emulated frame, `audioProduce()` calls the APU callback (~548 samples/frame),
  **mixes stereo → mono**, converts each sample to an 8-bit PWM duty, and pushes it into
  a single-producer / single-consumer ring buffer.
- A hardware timer ISR at `AUDIO_OUT_RATE` pops one duty per tick and writes it to an
  LEDC PWM channel on GPIO 1. The transistor + speaker reconstruct the waveform.

**Tunables** (top of the sound section in the sketch):
- `AUDIO_CARRIER` — PWM carrier frequency. Must stay below the LEDC ceiling
  (clock ÷ 2^bits); 100 kHz is safe. (312.5 kHz hit the ceiling and failed to attach.)
- `AUDIO_OUT_RATE` — ISR consumer rate. Should match the audio *production* rate,
  which is `fps × 548`. At ~56 fps that's **30000**. If audio is raspy/buzzy, the consumer
  is outrunning the producer — lower this.
- `AUDIO_GAIN` — 256 = unity; raise for louder, lower if harsh.

**Pitch note:** because production is tied to frame rate and the emulator runs ~56 fps
(not 59.7), audio plays a touch low-pitched. A locked 59.7 fps would fix the pitch and let
`AUDIO_OUT_RATE` return to 32768. Mono only (single speaker).

### Battery saves

For cartridges with battery-backed RAM, cart RAM is mirrored to `<romname>.sav` on the SD card:
- **On boot:** if a matching-size `.sav` exists, it's loaded into cart RAM.
- **During play:** `gb_cart_ram_write` marks RAM dirty and timestamps the write.
- **Flush:** ~1.5 s after writes settle, `loop()` writes the `.sav` (batches a save's burst
  of writes into one SD write; avoids thrashing the card every frame).
- The `.sav` is created automatically — no manual file setup.

> ROM-only carts (e.g. Tetris) have **no** save RAM — their high scores live in the Game Boy's
> work RAM and are lost on power-off, exactly like the original hardware. The save system stays
> dormant for them.
>
> **Power-off caveat:** wait ~1.5 s after an in-game save before cutting power, so the flush
> completes. A hardware power-loss detector would remove this caveat (future work).

### Game picker

During `setup()`, `disp_task` is parked waiting on `sem_ready` (never signaled until `loop()`
runs), so the TFT is free for direct text drawing. `scanGames()` lists all `.gb`/`.gbc` files
in the SD root; `gameMenu()` draws a scrollable list and reads the D-pad live via `btnDown()`
(edge-detected, one step per press), returning the chosen index.

---

## File layout

```
gameboy/
├─ README.md                 ← this file
└─ nes_gameboy/
   ├─ nes_gameboy.ino        ← main sketch (display, emulator, input, saves, menu, sound)
   ├─ peanut_gb.h            ← Peanut-GB emulator core (header-only)
   ├─ minigb_apu.h           ← APU sound synthesis (header)
   ├─ minigb_apu.c           ← APU sound synthesis (implementation)
   └─ User_Setup.h           ← legacy TFT_eSPI config (not used by LovyanGFX build)
```

---

## Roadmap

- [ ] Selectable / per-game palettes (grayscale, GBC-style DMG tints)
- [ ] "Exit to menu" hotkey (return to picker without rebooting)
- [ ] Volume control
- [ ] Hardware power-loss detection for guaranteed saves
- [ ] Enclosure + battery (true handheld)
- [ ] (Stretch) lock 59.7 fps everywhere — needs both framebuffers in internal RAM

## Known limitations

- **No color.** Peanut-GB is DMG-only; even `.gbc` games render in 4-shade green. True color
  would require a CGB-capable emulator core.
- **Audio is mono and slightly low-pitched** because the synthesis rate is tied to the
  emulator's ~56 fps (vs 59.7 native). See the Sound section.
- **Heavy scenes dip to ~54 fps** (light scenes lock to the 59.7 cap). See Performance work.
- Single ROM directory (SD root); subfolders are not scanned.
- Supported mappers: ROM-only, MBC1/2/3/5 (per Peanut-GB). MBC3 RTC games untested.

## Credits

- [Peanut-GB](https://github.com/deltabeard/Peanut-GB) — emulator core
- [minigb_apu](https://github.com/deltabeard/Peanut-GB) (by Mahyar Koshkouei / Alex Baines) — APU sound
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) — display driver
