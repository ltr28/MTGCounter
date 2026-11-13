#ifndef APP_UI_H
#define APP_UI_H

#include <stdint.h>

// Builds the first screen (player count picker) and wires up the rotary
// knob. Call once after lcd_lvgl_Init().
void app_ui_init(void);

// Must be called regularly from loop() (a few ms is fine). Drains rotation
// events queued by the knob driver and applies them to whichever screen is
// currently active.
void app_ui_tick(void);

// ---- BLE sync surface ------------------------------------------------------
//
// A small extern surface so ble_sync.cpp - a separate translation unit, kept
// out of app_ui.cpp's anonymous namespace so it doesn't need to reach into
// app_ui.cpp's internals directly - can read and mutate the player-profile
// roster and live game state. See ble_sync.h for the actual GATT service
// these back.
//
// Thread safety: NimBLE callbacks run on their own FreeRTOS task, not the
// Arduino loop() task, so every one of these takes lvgl_port_lock()
// internally (the same primitive app_ui_tick() already uses for the knob) -
// callers don't need their own locking.

#pragma pack(push, 1)

// One roster entry as sent over BLE: this header immediately followed by
// `name_len` bytes of (non-null-terminated) name. app_ui_serialize_roster()
// packs these back to back for every in-use profile.
struct BleProfileRecordHeader {
  uint8_t id;
  uint8_t flags;  // reserved, always 0 for now
  uint32_t games_played;
  uint32_t wins;
  int32_t total_life_delta;
  uint8_t name_len;
};

// A live snapshot of the current game, for a phone-side "watch on phone"
// view. profile_id[i] is 0 (the wire "no profile" sentinel) for a Guest
// slot, matching BleProfileRecordHeader.id's numbering.
struct BleGameState {
  uint8_t player_count;
  uint8_t active_player;
  int32_t life[4];
  uint8_t profile_id[4];
};

// One entry from the in-game history ring buffer (see app_ui.cpp). `player`
// is a slot index (0-3), not a profile id - cross-reference against the
// most recently read BleGameState.profile_id[] to get a name, the same way
// the on-device History screen does. Life changes only: counter changes
// are left out, and commander damage appears as the life it cost.
struct BleHistoryEntryWire {
  uint8_t player;
  int16_t delta;
  int16_t life_after;
};

// Extra counters (poison, commander damage, mana, ...) for the current
// game, from the Counters characteristic - see ble_sync.cpp. Separate from
// BleGameState so that struct's existing layout (and older phone apps that
// parse it) are unaffected. `version` is 1 for this layout; a phone should
// ignore a snapshot with a version it doesn't know.
//
// Counter kinds (shown_kinds[] values; never renumbered, only appended):
//    0 none (unused slot)   5 Monarch      10 Black mana
//    1 Poison               6 Rad          11 Red mana
//    2 Energy               7 Tally        12 Green mana
//    3 Experience           8 White mana   13 Colorless mana
//    4 +1/+1                9 Blue mana    40 Commander damage
//
// values[player][slot] holds every player's value for every kind, shown or
// not (hidden counters keep their values on the board): slots 1-13 are the
// kinds above (slot 5, Monarch, is 1 for the monarch, else 0), and slots
// 14-17 are commander damage that player has taken *from* player slot 0-3.
// Slot 0 is unused. Only the first player_count rows are meaningful.
constexpr int BLE_COUNTER_VALUE_SLOTS = 18;
constexpr uint8_t BLE_COUNTER_STATE_VERSION = 1;

struct BleCounterState {
  uint8_t version;         // BLE_COUNTER_STATE_VERSION
  uint8_t player_count;
  uint8_t shown_count;     // how many of shown_kinds[] are in use
  uint8_t shown_kinds[4];  // counter kinds shown on the board, in chip order
  uint8_t view;            // what the board's big numbers show: 0xFF = life, else index into shown_kinds
  uint8_t cmdr_source;     // while viewing commander damage: the attacking player's slot
  uint8_t monarch;         // player slot, or 0xFF for none
  int16_t values[4][BLE_COUNTER_VALUE_SLOTS];
};

#pragma pack(pop)

// Serializes every in-use roster profile into `out` (BleProfileRecordHeader
// + name bytes, back to back) up to `out_cap` bytes; a profile that wouldn't
// fully fit is dropped rather than split, so partial buffers are always
// still valid to parse. Returns the number of bytes written.
int app_ui_serialize_roster(uint8_t *out, int out_cap);

// Applies a phone-originated profile write: `data` is
// {uint8_t id, uint8_t flags, uint8_t name_len, char name[name_len]}, where
// id 0 means "create a new profile" and flags bit 0 means "delete the
// profile named by id" (name ignored). Returns false if `data`/`len` was
// malformed or referenced an id that no longer exists.
bool app_ui_apply_profile_write(const uint8_t *data, int len);

// Fills `out` with the current game's live state.
void app_ui_get_game_state(BleGameState *out);

// Fills `out` with the current game's extra counters.
void app_ui_get_counter_state(BleCounterState *out);

// Serializes the in-game history ring buffer, newest first, into `out` up
// to `out_cap` bytes (same drop-rather-than-split truncation as
// app_ui_serialize_roster()). Returns the number of bytes written.
int app_ui_serialize_history(uint8_t *out, int out_cap);

#endif
