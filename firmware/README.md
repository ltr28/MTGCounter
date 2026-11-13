# MTGCounter firmware

A life-total counter for the **Waveshare ESP32-S3 1.8" Knob Display**
(`ESP32-S3-Knob-Touch-LCD-1.8`, 360x360 round touchscreen + rotary knob).
Boots straight into a setup flow, then shows every player's life total at
once and lets you change them with the knob and/or a tap. A game in progress
survives power cycles - unplugging the board (or a firmware update) and
plugging it back in resumes exactly where you left off instead of restarting
setup, unless you explicitly start a New Game.

Players can be anonymous, or assigned from a persistent named roster that
tracks games played/wins/net life swing across games - see "Player
profiles" below. The board also runs a Bluetooth Low Energy GATT server so
the [MTGCounter Android app](../android/) can pair with it, manage that
roster (real names and deletions are easiest from the phone - the board
itself has no keyboard), and watch a game live - see "Bluetooth sync"
below.

## What it does

1. **Players** - rotate the knob to pick 1-4 players (a small map shows the
   table layout), tap the screen to continue, then assign each seat a
   player (see "Player profiles" below). The seat being assigned lights up
   on the map, so "Player 2" is always a visible seat.
2. **Starting life** - rotate the knob to pick any total from 1-100, tap to
   continue. Defaults to 20 for 1-2 players and 40 for 3-4 players (rotating
   the knob overrides the default for that game; spin it fast to move in
   5s). Both setup steps have a **Back** button.
3. **Game** - one shape per player: a full circle for 1 player, top/bottom
   halves for 2, three pie wedges for 3, or four quadrants for 4. Each
   player's total is rotated to read right-side up from where they'd be
   sitting around the device.
   - **Tap a player's shape** to make them the *active* player
     (highlighted) - this does *not* change their total.
   - **Tap the active player's shape again** to change their total: the
     right half (as read from that player's seat) increases it, the left
     half decreases it. Small `+`/`-` badges on the active player mark the
     two halves, and flash briefly when tapped.
   - **Rotate the knob** to adjust the active player too. Turning slowly
     moves 1 at a time; spinning quickly moves life in steps of 5.
   - **Running change** - while a player's total is changing, their name is
     replaced by the running change (e.g. `-7` in red, `+3` in green) until
     10 seconds pass without another change.
   - **Knocked out** - at 0 life, 10 poison, or 21 damage from one
     commander, the player's shape turns grey and shows a small skull.
   - **Who goes first** - whenever a game starts (New Game or Restart, 2+
     players), the highlight spins round the table, slows down and stops on
     a random player, whose name reads **Goes first!** for a few seconds.
     Tap the screen to skip straight to the result.
   - **Low battery** - at 15% the center button gets a thick red rim, and
     the center-button list shows the percentage at the top. It clears once
     the battery is back above 20%. (With no battery fitted - USB power
     only - it stays off.)
   - **Extra counters** - choose them from **Counters...** in the
     center-button list (or long-press anywhere on the table). Once any are
     shown, each player gets a row of small chips below their life total
     (from their seat): a heart showing their life, then the shown
     counters.
     - Counters are shared by every player (show Poison and everyone gets
       one). Up to 4 can be shown with 1-2 players, or 3 (with bigger
       chips) with 3-4 players. Available: Poison, Commander damage, Energy,
       Experience, +1/+1, Monarch, Rad, a generic Tally, and the six mana
       types (White, Blue, Black, Red, Green, Colorless) for floating mana.
     - **Tap a counter chip** to switch the whole table to that counter:
       every player's big number shows their value of it, with the
       counter's name in place of theirs. Adjust it exactly like life.
       Showing a new counter switches to it automatically.
     - **Switching counters (easiest)**: the big center button opens a
       list of large buttons - Life, each shown counter, and one
       "<name>'s cmdr" button per attacker for commander damage - then
       **Counters...** (show/hide counters) and **Menu**. With no counters
       shown it's just Counters... and Menu. Once any counter
       is shown, the button itself shows what the table is currently
       displaying (heart = life).
     - **Back to life**: Life in the center-button list, any heart chip, or
       the outlined chip again. The table also returns to life by itself
       after 15 seconds idle.
     - **Commander damage** is one counter. Each player's chip shows the
       most damage they've taken from any single commander. Tap a player's
       commander chip to say *their* commander is attacking: everyone else
       then shows (and adjusts) damage taken from it, captioned
       `From <name>`, and the attacker shows `Attacking`. Commander damage
       also comes off life automatically.
     - **Monarch** isn't a number: tap a player's crown chip to make them
       the monarch (it lights up gold - only one player can be), or tap it
       again to clear it.
     - Every player keeps a value for every counter for the whole game -
       hiding a counter in the picker only hides it. Counters never go
       below 0. Restart Game zeroes them; New Game hides and zeroes them.
       Counters are saved with the game, so they survive a power cycle.
   - **Screen dimming** - after 90 seconds without a touch or knob turn the
     backlight dims. The touch or turn that wakes it does nothing else, so
     waking it can't change a total.
   - The **menu** is the last button in the center-button list (scroll it
     with a swipe or the knob; tap outside it to close):
     - **History / Undo** - a log of every change this game (life and
       counters: who, what, by how much, and the result), newest first.
       Rapid changes to the same player and counter are folded into one
       entry; a run splits after 10 seconds without a change, or when you
       switch players. **Undo** at the top reverts the newest entry - the
       whole run, e.g. `-7` - including the life a commander-damage entry
       cost.
     - **Players** - the roster with wins/games. Tap a player for their
       stats, and a Delete button (which asks first).
     - **Dice & Coin** - roll a d6 or d20, or flip a coin (turn the knob to
       roll the same again), or re-run **Who goes first?**.
     - **Exit** - closes the menu.
     - **Restart Game** - asks first, then resets everyone to the starting
       life with the same players.
     - **New Game** - asks first, then goes back to step 1. This and
       Restart Game are the only things that clear history and the saved
       game. If named players are assigned, both first ask who won
       (players still standing are listed first).
     - A battery indicator (icon + `%`) sits above the menu options, read
       from the board's onboard battery-sense pin and refreshed every few
       seconds while the menu is open.
   - Confirmations are consistent everywhere: the destructive answer is
     red, the safe one green.

## Bluetooth sync

The board advertises as `MTGCounter` with one custom GATT service,
`8f6a2c00-2b1a-4f6e-9b0a-6f1d2a3c4e00` (NimBLE, Just Works bonding). All
values are little-endian packed structs; the structs themselves are
declared and documented in `MTGCounter/app_ui.h`.

| Characteristic | UUID | Access | Contents |
|---|---|---|---|
| Profile Roster | `8f6a2c01-...` | read, notify | every roster profile: `BleProfileRecordHeader` + name bytes, back to back |
| Profile Write | `8f6a2c02-...` | write | `{id, flags, name_len, name}` - id 0 creates, flags bit 0 deletes, otherwise renames |
| Game State | `8f6a2c03-...` | read, notify | `BleGameState`: player count, active player, life totals, profile ids |
| History | `8f6a2c04-...` | read | `BleHistoryEntryWire` per entry, newest first - life changes only (commander damage appears as the life it cost) |
| Counters | `8f6a2c05-...` | read, notify | `BleCounterState` (154 bytes, `version` 1): shown counter kinds, current view, commander attacker, monarch, and every player's value for every counter kind |

(The full UUIDs all end `-2b1a-4f6e-9b0a-6f1d2a3c4e00`.) Game State and
Counters are refreshed and notified together, debounced to about 150ms, on
any game change. A notification only carries as much of the value as the
connection's MTU allows - a phone should request an MTU of at least 157
(the Counters snapshot plus ATT overhead), or simply re-read the
characteristic when notified. Counters is new: the Android app needs
updating to read it; older app versions ignore it and keep working.

## Project layout

```
firmware/
  MTGCounter/          <- Arduino sketch, open MTGCounter.ino in the Arduino IDE
    MTGCounter.ino      entry point (setup/loop)
    app_ui.cpp/.h        the actual life-counter UI and state machine (see below)
    counter_icons.c/.h    GENERATED counter symbols (see tools/gen_counter_icons.py)
    lcd_bsp.cpp/.h        brings up the QSPI panel, LVGL and the backlight
    lcd_config.h          all board pin numbers in one place
    esp_lcd_sh8601.c/.h    vendor QSPI panel driver (from Waveshare's demo, unmodified)
    cst816.cpp/.h          CST816 capacitive touch driver over I2C (from Waveshare's demo)
    lcd_bl_pwm_bsp.c/.h    backlight PWM driver (from Waveshare's demo)
    bidi_switch_knob.c/.h  quadrature rotary-encoder driver (from Waveshare's demo)
  tools/gen_counter_icons.py  <- regenerates counter_icons.c/.h (needs Pillow)
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
| Battery sense (ADC1_CH0) | GPIO1, via an onboard 1:2 divider |

The panel is 360x360, RGB565. The knob has no push-button - only rotation -
which is why every "confirm" action in this app is a screen tap instead.

## Customizing

- **Encoder feels backwards**: swap `EXAMPLE_PIN_NUM_ENCODER_A`/`_B` in
  `lcd_config.h`, or swap the two `fetch_add`/`fetch_sub` calls in
  `knob_left_cb`/`knob_right_cb` in `app_ui.cpp`.
- **Step size**: each detent changes life by 1 (`apply_knob_step` in
  `app_ui.cpp`); change the `+= dir` arithmetic there for bigger steps.
- **Knob sensitivity**: a single stray pulse (e.g. brushing the knob) is
  ignored - `KNOB_ENGAGE_PULSES` in `app_ui.cpp` sets how many consecutive
  pulses in the same direction are required before they start changing a
  value. Raise it for a bigger dead zone, lower it (to 1) to make every pulse
  count immediately.
- **Starting life range/defaults**: `MIN_LIFE`/`MAX_LIFE` and
  `default_life_for_players()` in `app_ui.cpp`.
- **Life total font size**: `value_font_for_count()` in `app_ui.cpp` (limited
  to the Montserrat sizes enabled in `lv_conf.h` - 14/20/24/32/40/48 by
  default; enabling a different size there requires re-copying
  `lv_conf.h` per the gotcha above).
- **History length**: `HISTORY_CAPACITY` in `app_ui.cpp` - how many recent
  (possibly merged) history entries the menu's History screen remembers
  (oldest entries drop off the ring buffer once it's full).
- **History merge window**: `HISTORY_MERGE_WINDOW_MS` in `app_ui.cpp` (default
  10000ms) - how long a run of same-player changes can pause before the next
  one starts a new history entry instead of folding into the last one.
- **Saved-game persistence**: `save_game()`/`load_saved_game()`/
  `clear_saved_game()` in `app_ui.cpp` mirror the in-progress game (player
  count, starting life, each player's life and counters, shown counters, active player) into NVS flash via
  `Preferences`, under the `mtgcounter` namespace / `game` key, so
  `app_ui_init()` can auto-resume on boot. It's written after every life
  change, so a stray power loss loses at most nothing (NVS write is roughly
  synchronous). If you ever want a truly fresh board, erase NVS
  (`arduino-cli` has no built-in command for this - easiest is
  `esptool.py erase_flash` or a one-off sketch calling `Preferences::clear()`).
- **Battery percentage**: `read_battery_percent()` in `app_ui.cpp` assumes a
  single-cell LiPo and maps `BATTERY_EMPTY_V`/`BATTERY_FULL_V` (3.3-4.2V)
  linearly onto 0-100% - it's a rough approximation, not a fuel-gauge-grade
  reading. If your board doesn't have a battery connected, this will just
  read a low/noisy percentage from the floating pin.
- **Critical-battery shutdown**: `critical_battery_timer_cb` in `app_ui.cpp`
  polls the battery every `BATTERY_CRITICAL_CHECK_MS` (10s), regardless of
  which screen is up. Once the voltage reads below `BATTERY_CRITICAL_V`
  (3.1V) for `BATTERY_CRITICAL_CONSECUTIVE` checks in a row (debounced so one
  noisy ADC sample can't trigger it), `show_low_battery_and_shutdown()` saves
  the game, shows a "BATTERY CRITICAL" screen, and a couple of seconds later
  turns the backlight off and calls `esp_deep_sleep_start()`. This board has
  no PMU/load switch to actually cut power to the rest of the circuit, so
  deep sleep - with no wake source armed - is the closest thing to "off" the
  firmware can do: it just sits at the ESP32's deep-sleep current floor until
  you reset or power-cycle the board (which is also when you'd plug it in to
  recharge). The game resumes as normal on that next boot, same as any other
  power loss.
- **Panel colors**: `PLAYER_COLOR[]` in `app_ui.cpp`.
- **Extra counters**: the kinds, names, tint colors and "lethal" thresholds
  live in `CounterKind` / `COUNTER_KINDS[]` in `app_ui.cpp` (append new
  kinds only - the numeric values are stored in the saved game).
  `max_shown_counters()` sets how many can be shown per player count, and
  `chip_geometry()` the chip size/placement, sized so a full row clears the
  round bezel and stays inside its wedge. `VIEW_IDLE_RETURN_MS` sets the
  idle return-to-life delay (0 disables it). The symbols are drawn in
  `firmware/tools/gen_counter_icons.py` as simple vector shapes and baked
  into `counter_icons.c` as 8-bit alpha masks (tinted at runtime), so to
  add or tweak one, edit the script and run
  `python3 firmware/tools/gen_counter_icons.py`. Counters aren't part of the
  BLE game-state snapshot (whose layout is unchanged) but have their own
  Counters characteristic - see "Bluetooth sync".
- **Knob acceleration**: `KNOB_FAST_GAP_MS` / `KNOB_FAST_STREAK` /
  `KNOB_FAST_STEP` in `app_ui.cpp` - how quick and how sustained a spin has
  to be before life moves in bigger steps, and how big. Counters never
  accelerate.
- **Running change display**: `DELTA_DISPLAY_MS` (default 10000ms).
- **Who goes first**: `FIRST_PICK_MIN_STEPS` / `FIRST_PICK_START_MS` (how
  long the spin runs) and `FIRST_BANNER_MS` (how long "Goes first!" shows).
- **Low battery warning**: `BATTERY_LOW_PCT` / `BATTERY_LOW_CLEAR_PCT`
  (15% / 20%) and `BATTERY_ABSENT_V` in `app_ui.cpp`.
- **Screen dimming**: `DIM_AFTER_MS` (90s), `BACKLIGHT_DIM` and
  `BACKLIGHT_FULL` (PWM duty, 0-255) in `app_ui.cpp`.
- **Layout/rotation**: every player count is drawn as N equal pie wedges
  (drawn with `lv_arc`) via `wedge_geometry()` / `wedge_content_size()` /
  `WEDGE_RADIUS` in `app_ui.cpp`, used from `show_game_screen()`.
  `player_at_point()` has the matching tap hit-test math (which wedge a touch
  point falls in), and `point_is_on_right_half()` decides which side of that
  wedge counts as "increase" - keep both in sync if you
  change the layout. `BADGE_SIZE` controls the size of the `+`/`-` tap
  indicators drawn by `make_pm_badge()`.
