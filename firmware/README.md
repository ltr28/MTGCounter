# MTGCounter firmware

A life-total counter for the **Waveshare ESP32-S3 1.8" Knob Display**
(`ESP32-S3-Knob-Touch-LCD-1.8`, 360x360 round touchscreen + rotary knob).
Boots straight into a setup flow, then shows every player's life total at
once and lets you change them with the knob and/or a tap.

## What it does

1. **Players** - rotate the knob to pick 1-4 players, tap the screen to continue.
2. **Starting life** - rotate the knob to pick any total from 1-100, tap to
   continue. Defaults to 20 for 1-2 players and 40 for 3-4 players (rotating
   the knob overrides the default for that game).
3. **Game** - one shape per player: a full circle for 1 player, top/bottom
   halves for 2, three pie wedges for 3, or four quadrants for 4. Each
   player's total is rotated to read right-side up from where they'd be
   sitting around the device.
   - **Tap a player's shape** to make it "active" (highlighted) - this does
     *not* change its total.
   - **Rotate the knob** to adjust the active player's total up/down. This is
     the only way to change a total.
   - The small circular button in the center opens a "New game?" confirmation
     that takes you back to step 1 (life totals are not saved across a reset).

## Project layout

```
firmware/
  MTGCounter/          <- Arduino sketch, open MTGCounter.ino in the Arduino IDE
    MTGCounter.ino      entry point (setup/loop)
    app_ui.cpp/.h        the actual life-counter UI and state machine (see below)
    lcd_bsp.cpp/.h        brings up the QSPI panel, LVGL and the backlight
    lcd_config.h          all board pin numbers in one place
    esp_lcd_sh8601.c/.h    vendor QSPI panel driver (from Waveshare's demo, unmodified)
    cst816.cpp/.h          CST816 capacitive touch driver over I2C (from Waveshare's demo)
    lcd_bl_pwm_bsp.c/.h    backlight PWM driver (from Waveshare's demo)
    bidi_switch_knob.c/.h  quadrature rotary-encoder driver (from Waveshare's demo)
  lv_conf/lv_conf.h    <- LVGL config, needs to be copied into your Arduino libraries folder (see below)
```

`app_ui.cpp` is the file to read/edit if you want to change how the counter
behaves - the rest is board bring-up copied from Waveshare's official demo
(`ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip`, `Arduino/examples/08_LVGL_Test` and
`04_Encoder_Test`) so the low-level QSPI/touch/encoder timing is exactly what
the vendor validated on this hardware.

## Flashing the firmware

You only need to do **one** of the two options below.

### Option A: Arduino IDE

1. **Install the ESP32 board package**: File > Preferences > "Additional
   boards manager URLs" ->
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`,
   then Tools > Board > Boards Manager, install **esp32 by Espressif Systems**
   (2.0.14 or newer, 3.x is fine too).
2. **Install LVGL**: Sketch > Include Library > Manage Libraries, install
   **lvgl by kisvegabor, version 8.4.0** (this project uses the v8 API -
   do not install v9).
3. **Install the LVGL config**: copy `firmware/lv_conf/lv_conf.h` into your
   Arduino *libraries* folder as a sibling of the `lvgl` folder you just
   installed, i.e. `<sketchbook>/libraries/lv_conf.h` (typically
   `~/Arduino/libraries/lv_conf.h`, next to `~/Arduino/libraries/lvgl/`).
   This is what turns on the larger fonts, 16-bit swapped color format and
   transparent-layer rendering the UI needs; without it LVGL falls back to
   its template config and either won't compile or will silently fail to
   draw rotated text (see "gotcha" below).
4. Open `firmware/MTGCounter/MTGCounter.ino`, set the board settings below
   under the Tools menu, pick the board's port under Tools > Port, and click
   Upload. If the port doesn't show up, hold the BOOT button while plugging
   the board in to force download mode.

### Option B: arduino-cli (faster for repeated flashes)

One-time setup:

```bash
# 1. Install arduino-cli (see https://arduino.github.io/arduino-cli/latest/installation/)
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=./bin sh

# 2. Point it at the ESP32 core and install it
./bin/arduino-cli config init --overwrite
./bin/arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
./bin/arduino-cli core update-index
./bin/arduino-cli core install esp32:esp32

# 3. Install LVGL 8.4.0 (not 9.x - this project uses the v8 API)
./bin/arduino-cli lib install "lvgl@8.4.0"

# 4. Install the LVGL config next to the lvgl library (default sketchbook is
#    ~/Arduino unless you've changed it - check with `arduino-cli config dump`)
cp firmware/lv_conf/lv_conf.h ~/Arduino/libraries/lv_conf.h
```

Every time you flash:

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi"

./bin/arduino-cli compile --fqbn "$FQBN" firmware/MTGCounter

# Board shows up as /dev/ttyACM0 on Linux (or /dev/cu.usbmodem* on macOS).
# See "Troubleshooting" below if this fails with a permissions error.
./bin/arduino-cli upload -p /dev/ttyACM0 --fqbn "$FQBN" firmware/MTGCounter
```

⚠️ **Gotcha**: `firmware/lv_conf/lv_conf.h` in this repo is just the source
copy. If you edit it, you must re-copy it to wherever you installed it in
step 4 (`~/Arduino/libraries/lv_conf.h`) before recompiling - the build reads
from the installed copy, not from this repo directly, and arduino-cli's
build cache won't reliably notice the difference on its own. When in doubt,
re-copy the file and delete the build cache
(`rm -rf ~/.cache/arduino/sketches`) before compiling.

### Board settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Enabled |
| Flash Mode | DIO |
| Flash Size | 16MB |
| Partition Scheme | any 16MB scheme (e.g. "16M Flash (3MB APP/9.9MB FATFS)") |
| PSRAM | OPI PSRAM |
| Upload Speed | 921600 |

(This is exactly what the `$FQBN` string above encodes, for the CLI path.)

### Troubleshooting

- **`Permission denied` / `port is busy or doesn't exist` on Linux**: your
  user doesn't have access to the serial port yet. Either fix it for this
  session with `sudo chmod 666 /dev/ttyACM0` (resets on unplug/replug, so
  you'll need to repeat it after a reboot or reconnect), or fix it
  permanently with `sudo usermod -aG dialout $USER` followed by a
  logout/login (or reboot).
- **Port doesn't appear at all**: hold the BOOT button on the board while
  plugging in the USB-C cable to force download mode, then try again.
- **Compiles fine but the screen shows nothing / renders wrong**: almost
  always the `lv_conf.h` gotcha above - the installed copy is stale or
  missing. Re-copy it and do a clean rebuild.

## Hardware reference

Pulled from Waveshare's official demo package for this exact board
(`ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip`), also in `lcd_config.h`:

| Function | Pin(s) |
|---|---|
| LCD QSPI | CS=14, CLK=13, D0=15, D1=16, D2=17, D3=18, RST=21 |
| LCD backlight (PWM) | 47 |
| Touch (CST816, I2C) | SDA=11, SCL=12, addr 0x15 |
| Rotary knob (quadrature) | A=8, B=7 |

The panel is 360x360, RGB565. The knob has no push-button - only rotation -
which is why every "confirm" action in this app is a screen tap instead.

## Customizing

- **Encoder feels backwards**: swap `EXAMPLE_PIN_NUM_ENCODER_A`/`_B` in
  `lcd_config.h`, or swap the two `fetch_add`/`fetch_sub` calls in
  `knob_left_cb`/`knob_right_cb` in `app_ui.cpp`.
- **Step size**: each detent changes life by 1 (`apply_knob_step` in
  `app_ui.cpp`); change the `+= dir` arithmetic there for bigger steps.
- **Starting life range/defaults**: `MIN_LIFE`/`MAX_LIFE` and
  `default_life_for_players()` in `app_ui.cpp`.
- **Panel colors**: `PLAYER_COLOR[]` in `app_ui.cpp`.
- **Layout/rotation**: every player count is drawn as N equal pie wedges
  (drawn with `lv_arc`) via `wedge_geometry()` / `wedge_content_size()` /
  `WEDGE_RADIUS` in `app_ui.cpp`, used from `show_game_screen()`.
  `player_at_point()` has the matching tap hit-test math (which wedge a touch
  point falls in) - keep it in sync if you change the layout.
