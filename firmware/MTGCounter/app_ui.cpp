// Life-counter application built on top of the board bring-up in lcd_bsp.cpp.
//
// Flow:
//   1. STATE_SELECT_PLAYERS  - rotate the knob to pick 1-4 players, tap to confirm.
//   2. STATE_ASSIGN_PROFILES - per seat (shown on a little table map), pick
//      Guest / a roster profile / "+ New Player".
//   3. STATE_SELECT_LIFE     - rotate the knob to pick a starting total (1-100,
//      defaulted per player count), tap to confirm. Steps 2-3 have Back.
//   4. STATE_GAME            - one pie wedge per player, each with its life
//      total rotated to face outward from the center. Tapping a wedge makes
//      it the active player (which the knob adjusts, faster when spun
//      quickly); tapping the active wedge again adjusts it (right half up,
//      left half down). Extra counters (poison, commander damage, mana, ...)
//      show as tappable chips - see "extra counters" below. A center button
//      opens a menu (counters / history+undo / players / restart / new game).
//
// Rotating a shape's text is done by putting its labels in a small child
// container and setting that container's `transform_angle` style - LVGL 8
// renders any object with a non-zero transform_angle (and its children) to an
// offscreen layer first and rotates the whole layer as a bitmap when
// compositing, so this rotates real text, not just an image.
//
// The 3-player wedges are drawn with lv_arc: a full-size arc whose indicator
// spans the wedge's angle range with flat (non-rounded) caps and a width
// equal to the arc's radius is exactly a filled pie slice, and LVGL's arc
// renderer is well-tested compared to hand-rolling one with draw masks.
#include <atomic>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "lcd_bsp.h"
#include "lcd_config.h"
#include "lcd_bl_pwm_bsp.h"
#include "bidi_switch_knob.h"
#include "app_ui.h"
#include "ble_sync.h"
#include "counter_icons.h"

namespace {

enum AppState {
  STATE_SELECT_PLAYERS,
  STATE_ASSIGN_PROFILES,
  STATE_SELECT_LIFE,
  STATE_GAME,
};

constexpr int MAX_PLAYERS = 4;
constexpr int MIN_LIFE = 1;
constexpr int MAX_LIFE = 100;
constexpr float RAD2DEG = 57.29577951308232f;
constexpr uint32_t PLAYER_COLOR[MAX_PLAYERS] = {0xE9503F, 0x4F8FE9, 0x53D07A, 0xE9C23F};

// ---- extra counters ---------------------------------------------------------
//
// Besides life, the table can show a few extra counter kinds (poison,
// commander damage, energy, mana, ...) - at most max_shown_counters() at
// once. The set of shown kinds is shared by every player, and each player
// keeps a value for *every* kind for the whole game (kind_values below), so
// hiding a kind in the counter picker and showing it again later loses
// nothing. Shown kinds appear as a row of small icon + number "chips" just
// outside each player's life total, led by a heart chip (their life).
// Counters are shown/hidden from the center button's "Counters..." entry.
//
// Tapping a counter chip switches the whole table into that counter's
// *view*: every player's big number shows their value of it, adjusted
// exactly like life. Two kinds behave differently:
//   - Commander damage is one counter, but the 21-damage rule is per
//     commander, so each player stores one tally per *source* player.
//     Tapping player X's commander chip means "X's commander is attacking":
//     everyone else's big number then shows the damage they've taken from
//     X. The chip itself shows the most damage that player has taken from
//     any one commander.
//   - Monarch isn't a number: tapping a player's crown chip makes them the
//     monarch (only one player can be) or, if they already are, clears it.
//
// CounterKind values are persisted in the saved game, so never renumber
// them - only append.
enum CounterKind : uint8_t {
  CK_NONE = 0,
  CK_POISON,
  CK_ENERGY,
  CK_EXPERIENCE,
  CK_PLUS_ONE,
  CK_MONARCH,
  CK_RAD,
  CK_TALLY,
  CK_MANA_W,
  CK_MANA_U,
  CK_MANA_B,
  CK_MANA_R,
  CK_MANA_G,
  CK_MANA_C,
  CK_BASIC_COUNT,
  // Commander damage from one specific source player (32 + source slot).
  // Older firmware showed these as separate counters (migrated to
  // CK_COMMANDER on load); now they only tag commander-damage history
  // entries with their source.
  CK_COMMANDER_BASE = 32,
  // The single "Commander damage" counter - see the comment above.
  CK_COMMANDER = 40,
};

constexpr int MAX_COUNTERS = 4;  // storage cap; see max_shown_counters()
// One stored value per basic kind plus one commander-damage tally per
// source player - see value_slot().
constexpr int NUM_VALUE_SLOTS = CK_BASIC_COUNT + MAX_PLAYERS;
constexpr int COUNTER_MAX_VALUE = 999;
constexpr int COMMANDER_LETHAL = 21;
constexpr int POISON_LETHAL = 10;
constexpr uint32_t COMMANDER_COLOR = 0xD8D8D8;
constexpr uint32_t OUT_COLOR = 0x4A4A4A;  // wedge color for a knocked-out player

struct CounterKindInfo {
  const char *name;
  const lv_img_dsc_t *icon_small;
  const lv_img_dsc_t *icon_large;
  uint32_t color;
  int lethal;  // value at which the chip's number turns red, or 0 for never
};

// Indexed by CounterKind (basic kinds only - the commander kinds are handled
// in the counter_*() helpers below).
const CounterKindInfo COUNTER_KINDS[CK_BASIC_COUNT] = {
    {"", nullptr, nullptr, 0, 0},  // CK_NONE
    {"Poison", &icon_poison_s, &icon_poison_l, 0x8BD45A, POISON_LETHAL},
    {"Energy", &icon_energy_s, &icon_energy_l, 0xF2D04C, 0},
    {"Exp", &icon_experience_s, &icon_experience_l, 0xC39BFF, 0},
    {"+1/+1", &icon_plus_one_s, &icon_plus_one_l, 0xE0E0E0, 0},
    {"Monarch", &icon_monarch_s, &icon_monarch_l, 0xFFC93C, 0},
    {"Rad", &icon_rad_s, &icon_rad_l, 0xB5F23A, 0},
    {"Tally", &icon_tally_s, &icon_tally_l, 0xBBBBBB, 0},
    {"White", &icon_mana_w_s, &icon_mana_w_l, 0xF8F0C8, 0},
    {"Blue", &icon_mana_u_s, &icon_mana_u_l, 0x5AA9F0, 0},
    {"Black", &icon_mana_b_s, &icon_mana_b_l, 0xB49CB0, 0},
    {"Red", &icon_mana_r_s, &icon_mana_r_l, 0xF0603C, 0},
    {"Green", &icon_mana_g_s, &icon_mana_g_l, 0x4FC46A, 0},
    {"Colorless", &icon_mana_c_s, &icon_mana_c_l, 0xC8C8D2, 0},
};

constexpr int CHIP_SLOTS = MAX_COUNTERS + 1;  // + the life chip

struct PlayerShape {
  lv_obj_t *shape = nullptr;  // lv_arc wedge
  lv_obj_t *value_label = nullptr;
  lv_obj_t *name_label = nullptr;   // caption: name, recent change, or counter name
  lv_obj_t *out_icon = nullptr;     // skull, shown while knocked out
  lv_obj_t *minus_badge = nullptr;  // tap target / feedback for decrementing
  lv_obj_t *plus_badge = nullptr;   // tap target / feedback for incrementing
  int life = 0;

  // Running life change shown in place of the name for DELTA_DISPLAY_MS
  // after the last change (e.g. "-7"), so nobody has to count clicks.
  int recent_delta = 0;
  unsigned long recent_delta_ms = 0;
  bool recent_delta_shown = false;

  // This player's value for every counter kind, indexed by value_slot() -
  // kept for the whole game whether or not that kind is currently shown.
  int16_t kind_values[NUM_VALUE_SLOTS] = {};

  // Rotated holder for this player's chips, plus where it sits in screen
  // coordinates (center + rotation) so taps can be hit-tested against the
  // chips without relying on LVGL's transformed hit testing. chip_kind[]
  // maps each chip position, left to right, to a shown-kind index or one of
  // the CHIP_LIFE marker; counter_chip[]/counter_label[] are
  // indexed by shown-kind index.
  lv_obj_t *counter_row = nullptr;
  lv_obj_t *counter_chip[MAX_COUNTERS] = {};
  lv_obj_t *counter_icon_img[MAX_COUNTERS] = {};
  lv_obj_t *counter_label[MAX_COUNTERS] = {};
  lv_obj_t *life_chip = nullptr;
  lv_obj_t *life_chip_label = nullptr;
  int chip_kind[CHIP_SLOTS] = {};
  int chip_count = 0;
  lv_coord_t counter_cx = 0;
  lv_coord_t counter_cy = 0;
  int counter_rotation_deg = 0;
};

AppState s_state = STATE_SELECT_PLAYERS;
std::atomic<int> s_knob_delta{0};

// Set once the critical-battery shutdown sequence has started, so a stray
// knob pulse in the last couple of seconds before deep sleep can't still
// poke a life total on a screen that's no longer even visible.
bool s_shutting_down = false;

int s_player_count = 2;
int s_starting_life = 20;
int s_active_player = 0;
PlayerShape s_players[MAX_PLAYERS];

// The table's shown counter kinds (CounterKind values), in chip order.
uint8_t s_counter_kinds[MAX_COUNTERS];
int s_counter_kind_count = 0;

// Which shown counter (index into s_counter_kinds) the table is currently
// showing and adjusting in place of life, or -1 for life. Not persisted -
// a resumed game starts on life.
int s_view_counter = -1;
// While viewing commander damage: whose commander is dealing it.
int s_cmdr_source = 0;

// While viewing a counter, the table drops back to life after this long
// with no taps/knob turns (0 disables it), so a quick "add a poison" never
// leaves the table stuck on the poison view. See ui_tick_timer_cb().
constexpr unsigned long VIEW_IDLE_RETURN_MS = 15000;
unsigned long s_view_last_activity_ms = 0;

// How long a player's running life change ("-7") replaces their name.
constexpr unsigned long DELTA_DISPLAY_MS = 10000;

void note_view_activity() {
  s_view_last_activity_ms = millis();
}

// 3-4 player wedges are narrow, so they show fewer (but bigger) chips.
int max_shown_counters() {
  return s_player_count >= 3 ? 3 : 4;
}

bool is_commander_source_kind(uint8_t kind) {
  return kind >= CK_COMMANDER_BASE && kind < CK_COMMANDER_BASE + MAX_PLAYERS;
}

int commander_source(uint8_t kind) {
  return kind - CK_COMMANDER_BASE;
}

int cmdr_slot(int source) {
  return CK_BASIC_COUNT + source;
}

// Can `kind` be shown in a game of `player_count` players?
bool counter_kind_valid(uint8_t kind, int player_count) {
  if (kind > CK_NONE && kind < CK_BASIC_COUNT) return true;
  return kind == CK_COMMANDER && player_count > 1;
}

// Index into PlayerShape::kind_values for a basic kind or a per-source
// commander kind (CK_COMMANDER itself has no single slot).
int value_slot(uint8_t kind) {
  return is_commander_source_kind(kind) ? cmdr_slot(commander_source(kind)) : kind;
}

const lv_img_dsc_t *counter_icon(uint8_t kind, bool large) {
  if (kind == CK_COMMANDER || is_commander_source_kind(kind)) return large ? &icon_commander_l : &icon_commander_s;
  return large ? COUNTER_KINDS[kind].icon_large : COUNTER_KINDS[kind].icon_small;
}

uint32_t counter_color(uint8_t kind) {
  if (kind == CK_COMMANDER) return COMMANDER_COLOR;
  if (is_commander_source_kind(kind)) return PLAYER_COLOR[commander_source(kind)];
  return COUNTER_KINDS[kind].color;
}

int counter_lethal(uint8_t kind) {
  if (kind == CK_COMMANDER || is_commander_source_kind(kind)) return COMMANDER_LETHAL;
  return COUNTER_KINDS[kind].lethal;
}

// The most commander damage `player` has taken from any one commander -
// what the 21-damage rule cares about, so it's what their chip shows.
int max_commander_damage(int player) {
  int best = 0;
  for (int src = 0; src < s_player_count; src++) {
    if (src != player && s_players[player].kind_values[cmdr_slot(src)] > best) {
      best = s_players[player].kind_values[cmdr_slot(src)];
    }
  }
  return best;
}

// What `player`'s chip for shown counter k displays.
int chip_value(int player, int k) {
  uint8_t kind = s_counter_kinds[k];
  if (kind == CK_COMMANDER) return max_commander_damage(player);
  return s_players[player].kind_values[kind];
}

// The value `player`'s big number shows (and the knob/taps adjust) while
// the table views shown counter k, or nullptr if there isn't one - the
// attacking player in the commander view.
int16_t *view_value_ptr(int player, int k) {
  uint8_t kind = s_counter_kinds[k];
  if (kind == CK_COMMANDER) {
    return player == s_cmdr_source ? nullptr : &s_players[player].kind_values[cmdr_slot(s_cmdr_source)];
  }
  if (kind == CK_MONARCH) return nullptr;
  return &s_players[player].kind_values[kind];
}

// Knocked out: 0 life, 10 poison, or 21 damage from one commander.
bool player_is_out(int player) {
  const PlayerShape &ps = s_players[player];
  return ps.life <= 0 || ps.kind_values[CK_POISON] >= POISON_LETHAL || max_commander_damage(player) >= COMMANDER_LETHAL;
}

int monarch_player() {
  for (int i = 0; i < s_player_count; i++) {
    if (s_players[i].kind_values[CK_MONARCH] > 0) return i;
  }
  return -1;
}

int find_counter_kind(uint8_t kind) {
  for (int k = 0; k < s_counter_kind_count; k++) {
    if (s_counter_kinds[k] == kind) return k;
  }
  return -1;
}

void zero_all_counter_values() {
  for (int i = 0; i < MAX_PLAYERS; i++) memset(s_players[i].kind_values, 0, sizeof(s_players[i].kind_values));
}

void clear_all_counters() {
  s_counter_kind_count = 0;
  s_view_counter = -1;
  s_cmdr_source = 0;
  zero_all_counter_values();
}

// Which roster PlayerProfile (see "player profiles" section below) occupies
// each slot for the current game, or -1 for an anonymous "Guest" slot.
int s_player_profile_id[MAX_PLAYERS] = {-1, -1, -1, -1};

lv_obj_t *s_scr_players = nullptr;
lv_obj_t *s_players_value_label = nullptr;
lv_obj_t *s_players_map = nullptr;

lv_obj_t *s_scr_life = nullptr;
lv_obj_t *s_life_value_label = nullptr;

lv_obj_t *s_scr_game = nullptr;
lv_obj_t *s_reset_overlay = nullptr;  // "New game?" / "Restart game?" confirm
lv_obj_t *s_menu_overlay = nullptr;
lv_obj_t *s_history_overlay = nullptr;
lv_obj_t *s_players_list_overlay = nullptr;
lv_obj_t *s_profile_detail_overlay = nullptr;
lv_obj_t *s_profile_delete_confirm_overlay = nullptr;
lv_obj_t *s_winner_overlay = nullptr;
lv_obj_t *s_counter_picker_overlay = nullptr;
lv_obj_t *s_view_switcher_overlay = nullptr;
// The center button's contents (it opens the view switcher - see
// show_view_switcher()): a menu symbol while no counters are shown,
// otherwise the icon of what the table is currently showing.
lv_obj_t *s_center_btn = nullptr;
lv_obj_t *s_center_label = nullptr;
lv_obj_t *s_center_icon = nullptr;

// Set while the battery is low (see battery_low_timer_cb()): the center
// button gets a red rim and the switcher shows the percentage.
bool s_battery_low = false;
int s_battery_low_pct = 0;

// The random "who goes first" pick (start_first_player_pick()): a
// transparent overlay eats input while the highlight spins (tapping it
// skips to the result), then the winner's caption reads "Goes first!" for
// FIRST_BANNER_MS.
lv_obj_t *s_first_pick_overlay = nullptr;
int s_first_banner_player = -1;
unsigned long s_first_banner_ms = 0;
constexpr unsigned long FIRST_BANNER_MS = 5000;
constexpr uint32_t FIRST_BANNER_COLOR = 0xFFD54A;

lv_obj_t *s_dice_overlay = nullptr;
lv_obj_t *s_battery_fill = nullptr;
lv_obj_t *s_battery_label = nullptr;
lv_timer_t *s_battery_timer = nullptr;

// The scrollable list inside whichever overlay is open (menu, history,
// players, picker, winner prompt), if any - the knob scrolls it.
lv_obj_t *s_knob_scroll_target = nullptr;

// Ring buffer of recent changes (life totals and counters, from the knob or
// taps), shown - and undoable - from the menu's History screen. Cleared by
// New Game / Restart Game, but not on its own - it's "this game so far".
struct HistoryEntry {
  int8_t player;
  uint8_t kind;          // CK_NONE = life; a basic kind; CK_COMMANDER_BASE + source for commander damage
  int16_t delta;         // change to life (CK_NONE) or to the counter
  int16_t value_after;   // life or counter value after the change
  int16_t life_after;    // life after (commander damage costs life too)
};
constexpr int HISTORY_CAPACITY = 40;
HistoryEntry s_history[HISTORY_CAPACITY];
int s_history_count = 0;  // number of valid entries, saturates at HISTORY_CAPACITY
int s_history_next = 0;   // ring buffer write cursor

// A run of same-player, same-kind changes within HISTORY_MERGE_WINDOW_MS of
// each other is folded into one entry (net delta, latest value) instead of
// logging every single +1/-1 separately. The window slides - it resets on
// every merged change - so a burst only closes out after real inactivity;
// ends immediately if the active player switches, even inside the window.
constexpr unsigned long HISTORY_MERGE_WINDOW_MS = 10000;
int s_history_open_slot = -1;  // ring index of the entry still open for merging, or -1
unsigned long s_history_last_edit_ms = 0;

void reset_history() {
  s_history_count = 0;
  s_history_next = 0;
  s_history_open_slot = -1;
}

int newest_history_index() {
  return (s_history_next - 1 + HISTORY_CAPACITY) % HISTORY_CAPACITY;
}

// ---- persistence ------------------------------------------------------
//
// The current game (player count, starting life, each player's life and
// counters, which counters are shown, and which player is active) is
// mirrored into NVS flash after every change, so a power cycle resumes
// exactly where the game left off instead of restarting the setup flow.
// Cleared when the player deliberately starts a New Game. History isn't
// persisted - it's framed as "this game so far" and isn't worth the extra
// flash traffic.
Preferences s_prefs;
// Bumped from the pre-profiles 0x4D544743 ("MTGC") whenever SavedGame's
// layout changes, so a firmware update cleanly discards an old-shaped saved
// game instead of misinterpreting its bytes - see load_saved_game().
// Older layouts are still accepted, since each one only appended to or
// replaced the tail of the previous one:
//   "MTGD" - pre-counters: a valid prefix with no counters.
//   "MTGF" - counter values stored per shown counter (int16[4][4]) rather
//            than per kind; converted on load.
// (Shown kinds saved as per-source commander kinds, CK_COMMANDER_BASE + n,
// are folded into CK_COMMANDER by app_ui_init().)
constexpr uint32_t SAVE_MAGIC_V1 = 0x4D544744;  // "MTGD"
constexpr uint32_t SAVE_MAGIC_V2 = 0x4D544746;  // "MTGF"
constexpr uint32_t SAVE_MAGIC = 0x4D544747;     // "MTGG"

struct SavedGame {
  uint32_t magic;
  int32_t player_count;
  int32_t starting_life;
  int32_t active_player;
  int32_t life[MAX_PLAYERS];
  int32_t profile_id[MAX_PLAYERS];  // -1 = Guest, else a PlayerProfile.id
  // ---- end of the "MTGD" layout ----
  uint8_t counter_kinds[MAX_COUNTERS];  // shown kinds (CounterKind), CK_NONE = unused
  int16_t kind_values[MAX_PLAYERS][NUM_VALUE_SLOTS];  // by value_slot()
};
constexpr size_t SAVED_GAME_V1_SIZE = offsetof(SavedGame, counter_kinds);
constexpr size_t SAVED_GAME_V2_SIZE = offsetof(SavedGame, kind_values) + sizeof(int16_t) * MAX_PLAYERS * MAX_COUNTERS;

void save_game() {
  SavedGame g;
  memset(&g, 0, sizeof(g));
  g.magic = SAVE_MAGIC;
  g.player_count = s_player_count;
  g.starting_life = s_starting_life;
  g.active_player = s_active_player;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    g.life[i] = (i < s_player_count) ? s_players[i].life : 0;
    g.profile_id[i] = (i < s_player_count) ? s_player_profile_id[i] : -1;
  }
  for (int k = 0; k < s_counter_kind_count; k++) g.counter_kinds[k] = s_counter_kinds[k];
  for (int i = 0; i < s_player_count; i++) memcpy(g.kind_values[i], s_players[i].kind_values, sizeof(g.kind_values[i]));
  s_prefs.begin("mtgcounter", false);
  s_prefs.putBytes("game", &g, sizeof(g));
  s_prefs.end();
  // Every saved change is worth telling a connected phone about (this is
  // debounced) - counter-only changes don't otherwise notify.
  ble_sync_notify_game_state_changed();
}

void clear_saved_game() {
  s_prefs.begin("mtgcounter", false);
  s_prefs.remove("game");
  s_prefs.end();
}

bool load_saved_game(SavedGame *out) {
  memset(out, 0, sizeof(*out));  // an "MTGD" blob leaves counters all CK_NONE
  s_prefs.begin("mtgcounter", true);  // read-only
  size_t len = s_prefs.getBytesLength("game");
  bool read_ok = (len == sizeof(SavedGame) || len == SAVED_GAME_V2_SIZE || len == SAVED_GAME_V1_SIZE) &&
                 s_prefs.getBytes("game", out, len) == len;
  s_prefs.end();
  if (!read_ok) return false;
  bool magic_ok = (len == sizeof(SavedGame) && out->magic == SAVE_MAGIC) ||
                  (len == SAVED_GAME_V2_SIZE && out->magic == SAVE_MAGIC_V2) ||
                  (len == SAVED_GAME_V1_SIZE && out->magic == SAVE_MAGIC_V1);
  if (!magic_ok || out->player_count < 1 || out->player_count > MAX_PLAYERS) return false;

  if (out->magic == SAVE_MAGIC_V2) {
    // "MTGF" stored int16[player][shown counter] where kind_values now
    // starts - re-file each value under its kind's value slot.
    int16_t old_values[MAX_PLAYERS][MAX_COUNTERS];
    memcpy(old_values, out->kind_values, sizeof(old_values));
    memset(out->kind_values, 0, sizeof(out->kind_values));
    for (int k = 0; k < MAX_COUNTERS; k++) {
      uint8_t kind = out->counter_kinds[k];
      bool basic = kind > CK_NONE && kind < CK_BASIC_COUNT;
      bool source = is_commander_source_kind(kind) && commander_source(kind) < out->player_count;
      if (!basic && !source) continue;
      for (int i = 0; i < MAX_PLAYERS; i++) out->kind_values[i][value_slot(kind)] = old_values[i][k];
    }
  }
  return true;
}

// ---- player profiles ----------------------------------------------------
//
// A persistent roster of named players, separate from any one game, so
// stats (games played, wins, net life swing) accumulate across games
// instead of resetting every time. Assigned to slots at setup time (see
// STATE_ASSIGN_PROFILES below) instead of the old anonymous "P1".."P4".
//
// Naming is phone-first: this board has a knob and a touchscreen but no
// keyboard, so on-device you can only pick an existing profile or spawn a
// placeholder ("Player <id>") - real names get set from the Android app and
// synced down over BLE (see ble_sync.h). id 0 is reserved as the wire-format
// "no profile" sentinel (mirrored on-device by -1 in s_player_profile_id),
// so real ids start at 1.
constexpr int MAX_PROFILES = 16;
constexpr int PROFILE_NAME_MAX = 15;

struct PlayerProfile {
  uint8_t id;      // stable once assigned; 0 means this slot is unused
  bool in_use;
  char name[PROFILE_NAME_MAX + 1];
  uint32_t games_played;
  uint32_t wins;
  int32_t total_life_delta;
};

struct ProfileRoster {
  uint32_t magic;
  uint8_t next_id;
  PlayerProfile profiles[MAX_PROFILES];
};

constexpr uint32_t ROSTER_MAGIC = 0x4D545052;  // "MTPR"
ProfileRoster s_roster;

void save_roster() {
  s_prefs.begin("mtgcounter", false);
  s_prefs.putBytes("profiles", &s_roster, sizeof(s_roster));
  s_prefs.end();
}

// Called once at boot. Starts an empty roster if NVS has nothing (first
// boot) or something that doesn't match today's layout (rejected the same
// way load_saved_game() rejects a foreign/stale SavedGame).
void load_roster() {
  s_prefs.begin("mtgcounter", true);
  bool ok = s_prefs.getBytesLength("profiles") == sizeof(ProfileRoster) &&
            s_prefs.getBytes("profiles", &s_roster, sizeof(ProfileRoster)) == sizeof(ProfileRoster);
  s_prefs.end();
  if (!ok || s_roster.magic != ROSTER_MAGIC) {
    memset(&s_roster, 0, sizeof(s_roster));
    s_roster.magic = ROSTER_MAGIC;
    s_roster.next_id = 1;
  }
}

PlayerProfile *find_profile(uint8_t id) {
  if (id == 0) return nullptr;
  for (int i = 0; i < MAX_PROFILES; i++) {
    if (s_roster.profiles[i].in_use && s_roster.profiles[i].id == id) return &s_roster.profiles[i];
  }
  return nullptr;
}

// s_player_profile_id[slot] is -1 for Guest - never pass it to find_profile()
// directly, since casting -1 to uint8_t wraps to 255, not the 0 sentinel.
PlayerProfile *profile_for_slot(int slot) {
  int id = s_player_profile_id[slot];
  return id > 0 ? find_profile((uint8_t)id) : nullptr;
}

int count_in_use_profiles() {
  int n = 0;
  for (int i = 0; i < MAX_PROFILES; i++) {
    if (s_roster.profiles[i].in_use) n++;
  }
  return n;
}

// Creates a roster entry, named `name` if given (used by the BLE write
// path - see ble_sync.h) or "Player <id>" as an on-device placeholder
// (renamed later from the phone app). Returns the new id, or 0 if the
// roster is already full.
uint8_t create_profile(const char *name) {
  int slot = -1;
  for (int i = 0; i < MAX_PROFILES; i++) {
    if (!s_roster.profiles[i].in_use) { slot = i; break; }
  }
  if (slot < 0) return 0;

  uint8_t id = s_roster.next_id++;
  if (s_roster.next_id == 0) s_roster.next_id = 1;  // wrap past the 0 sentinel

  PlayerProfile &p = s_roster.profiles[slot];
  p.id = id;
  p.in_use = true;
  p.games_played = 0;
  p.wins = 0;
  p.total_life_delta = 0;
  if (name && name[0]) {
    strncpy(p.name, name, PROFILE_NAME_MAX);
    p.name[PROFILE_NAME_MAX] = '\0';
  } else {
    snprintf(p.name, sizeof(p.name), "Player %u", (unsigned)id);
  }
  save_roster();
  ble_sync_notify_profiles_changed();
  return id;
}

// Deletes a profile and un-assigns it from any slot in the game currently
// being set up (a slot pointing at a since-deleted profile would otherwise
// dangle).
void delete_profile(uint8_t id) {
  PlayerProfile *p = find_profile(id);
  if (!p) return;
  p->in_use = false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (s_player_profile_id[i] == (int)id) s_player_profile_id[i] = -1;
  }
  save_roster();
  ble_sync_notify_profiles_changed();
}

void show_players_screen();
void show_assign_profiles_screen(bool resume_last_slot = false);
void show_life_screen();
void show_game_screen(bool reset_life = true);
void show_menu();
void show_history();
void show_players_list();
void show_end_game_confirm(bool is_new_game);
void show_winner_prompt(bool is_new_game);
void show_counter_picker();
lv_obj_t *make_confirm_button(lv_obj_t *parent, const char *text, uint32_t color, lv_coord_t x_off);

// ---- shared helpers ------------------------------------------------------

lv_obj_t *make_screen() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
  return lbl;
}

int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Every lv_obj is clickable by default in LVGL 8 - anything that's purely
// decorative (backgrounds, rotated text containers) must have that cleared,
// otherwise it swallows taps that should fall through to the screen's
// tap-to-select handler below it.
lv_obj_t *make_inert(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

// ---- change-only setters ----------------------------------------------------
//
// LVGL invalidates (i.e. schedules a redraw of) an object on *every* style
// or text set, even when the value doesn't change. That matters here: each
// player wedge is a full-screen 360x360 arc, so re-setting its color redraws
// the whole screen (~125ms measured), and each player's text sits in a
// rotated layer that costs ~30ms to re-render. The refresh_*() functions
// recompute everything from state, so they go through these setters, which
// only touch LVGL when a value actually changes.

void show_hide(lv_obj_t *obj, bool show) {
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != show) return;  // already as wanted
  if (show) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

void set_text(lv_obj_t *label, const char *text) {
  const char *cur = lv_label_get_text(label);
  if (cur && strcmp(cur, text) == 0) return;
  lv_label_set_text(label, text);
}

void set_text_int(lv_obj_t *label, const char *fmt, int value) {
  char buf[16];
  snprintf(buf, sizeof(buf), fmt, value);
  set_text(label, buf);
}

void set_text_color(lv_obj_t *obj, uint32_t hex) {
  lv_color_t c = lv_color_hex(hex);
  if (lv_obj_get_style_text_color(obj, LV_PART_MAIN).full == c.full) return;
  lv_obj_set_style_text_color(obj, c, 0);
}

void set_border(lv_obj_t *obj, lv_coord_t width, uint32_t hex) {
  lv_color_t c = lv_color_hex(hex);
  if (lv_obj_get_style_border_width(obj, LV_PART_MAIN) != width) lv_obj_set_style_border_width(obj, width, 0);
  if (lv_obj_get_style_border_color(obj, LV_PART_MAIN).full != c.full) lv_obj_set_style_border_color(obj, c, 0);
}

void set_bg_opa(lv_obj_t *obj, lv_opa_t opa) {
  if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(obj, opa, 0);
}

void set_img_color(lv_obj_t *img, uint32_t hex) {
  lv_color_t c = lv_color_hex(hex);
  if (lv_obj_get_style_img_recolor(img, LV_PART_MAIN).full != c.full) lv_obj_set_style_img_recolor(img, c, 0);
}

void set_arc_look(lv_obj_t *arc, uint32_t hex, lv_opa_t opa) {
  lv_color_t c = lv_color_hex(hex);
  if (lv_obj_get_style_arc_color(arc, LV_PART_INDICATOR).full != c.full) {
    lv_obj_set_style_arc_color(arc, c, LV_PART_INDICATOR);
  }
  if (lv_obj_get_style_arc_opa(arc, LV_PART_INDICATOR) != opa) lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
}

// A full-screen dimmed backdrop for an overlay. Left clickable (the lv_obj
// default) so it eats taps instead of them falling through to the game
// screen underneath.
lv_obj_t *make_overlay(lv_opa_t opa) {
  lv_obj_t *overlay = lv_obj_create(s_scr_game);
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, 360, 360);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, opa, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  return overlay;
}

// Closes an overlay when its backdrop is tapped outside `inner` (its
// list/content area). Taps on buttons and lists never get here - they're
// clickable children, and LVGL events don't bubble by default - so this
// only sees the backdrop; the `inner` check is a safety margin so that
// just missing a button near the list doesn't close the overlay.
void overlay_outside_tap_cb(lv_event_t *e) {
  lv_obj_t *overlay = lv_event_get_current_target(e);
  if (lv_event_get_target(e) != overlay) return;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  lv_obj_t *inner = (lv_obj_t *)lv_obj_get_user_data(overlay);
  if (inner) {
    lv_area_t a;
    lv_obj_get_coords(inner, &a);
    if (_lv_area_is_point_on(&a, &p, 0)) return;
  }
  void (*close_fn)() = reinterpret_cast<void (*)()>(lv_event_get_user_data(e));
  close_fn();
}

void close_on_outside_tap(lv_obj_t *overlay, lv_obj_t *inner, void (*close_fn)()) {
  lv_obj_set_user_data(overlay, inner);
  lv_obj_add_event_cb(overlay, overlay_outside_tap_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(close_fn));
}

// "Alice", or "P2" for a Guest slot.
const char *slot_display_name(int slot, char *buf, size_t buf_len) {
  PlayerProfile *p = profile_for_slot(slot);
  if (p) return p->name;
  snprintf(buf, buf_len, "P%d", slot + 1);
  return buf;
}

// "Poison", "Commander", or "Cmdr Alice" for damage from one commander.
const char *counter_kind_name(uint8_t kind, char *buf, size_t buf_len) {
  if (kind == CK_COMMANDER) return "Commander";
  if (!is_commander_source_kind(kind)) return COUNTER_KINDS[kind].name;
  char name_buf[8];
  snprintf(buf, buf_len, "Cmdr %s", slot_display_name(commander_source(kind), name_buf, sizeof(name_buf)));
  return buf;
}

// ---- screen dimming ------------------------------------------------------
//
// The backlight drops to BACKLIGHT_DIM after DIM_AFTER_MS without a touch or
// knob turn (LVGL tracks touch activity itself; knob pulses report in via
// lv_disp_trig_activity() in app_ui_tick()). The touch or turn that wakes
// it is swallowed, so waking the screen can never change a life total.
constexpr uint32_t DIM_AFTER_MS = 90000;
constexpr uint16_t BACKLIGHT_FULL = 255;
constexpr uint16_t BACKLIGHT_DIM = 20;
constexpr unsigned long WAKE_SWALLOW_MS = 600;

bool s_dimmed = false;
unsigned long s_woke_ms = 0;

void wake_display() {
  if (!s_dimmed) return;
  s_dimmed = false;
  s_woke_ms = millis();
  setUpdutySubdivide(BACKLIGHT_FULL);
}

void dim_timer_cb(lv_timer_t *timer) {
  if (s_shutting_down) return;
  uint32_t inactive = lv_disp_get_inactive_time(NULL);
  if (!s_dimmed && inactive > DIM_AFTER_MS) {
    s_dimmed = true;
    setUpdutySubdivide(BACKLIGHT_DIM);
  } else if (s_dimmed && inactive < 300) {
    wake_display();
  }
}

// Call at the top of a tap handler: true if this tap is the one waking the
// screen (or lands right after it), and so should be ignored.
bool swallow_wake_tap() {
  if (s_dimmed) {
    wake_display();
    return true;
  }
  return millis() - s_woke_ms < WAKE_SWALLOW_MS;
}

// ---- table geometry ---------------------------------------------------------
//
// Every player count is drawn as N equal pie wedges meeting at the screen's
// center - for 1 player that's the whole circle, for 2 it's two 180-degree
// half-discs, for 3 it's three 120-degree slices, etc. Slot 0 always points
// straight down (unrotated, facing the "default" viewer) and the rest are
// spaced clockwise from there - e.g. for 3 players that's one wedge down and
// two pointing up-left/up-right, like the bottom half of a peace sign.
//
// Each wedge's content (name + value labels) lives in its own small
// container, rotated so its text reads right side up from that position
// (i.e. rotated 180 degrees from the direction pointing away from center),
// and positioned along the wedge's bisector at a fixed radius from center.

constexpr float DEG2RAD = 0.017453293f;
constexpr int WEDGE_RADIUS = 90;
constexpr lv_coord_t BADGE_SIZE = 22;  // diameter of the +/- tap-feedback badges

// LVGL arc angle (0 = 3 o'clock, clockwise) for the start of wedge `idx` out
// of `n`, the content rotation to apply (degrees clockwise), and the
// content's center offset from screen center. `n` must not be 0.
void wedge_geometry(int n, int idx, int *start_angle, int *rotation_deg, lv_coord_t *dx, lv_coord_t *dy) {
  if (n == 1) {
    *start_angle = 0;
    *rotation_deg = 0;
    *dx = 0;
    *dy = 0;
    return;
  }
  int step = 360 / n;
  int compass = (180 + idx * step) % 360;  // 0 = up, clockwise; direction this wedge points
  *start_angle = (((90 - step / 2 + idx * step) % 360) + 360) % 360;
  *rotation_deg = (idx * step) % 360;
  float rad = (float)compass * DEG2RAD;
  *dx = (lv_coord_t)lroundf(WEDGE_RADIUS * sinf(rad));
  *dy = (lv_coord_t)lroundf(-WEDGE_RADIUS * cosf(rad));
}

void wedge_content_size(int n, lv_coord_t *w, lv_coord_t *h) {
  if (n == 1) { *w = 220; *h = 140; }
  else if (n == 2) { *w = 150; *h = 80; }
  else if (n == 3) { *w = 120; *h = 74; }
  else { *w = 120; *h = 80; }  // 4
}

const lv_font_t *value_font_for_count(int n) {
  return n == 1 ? &lv_font_montserrat_48 : &lv_font_montserrat_40;
}

// One filled pie wedge as an lv_arc: a full-size arc whose indicator spans
// the wedge's angle range with flat caps and a width equal to its radius.
lv_obj_t *make_wedge_arc(lv_obj_t *parent, lv_coord_t size, int start_angle, int end_angle, uint32_t color,
                         lv_opa_t opa) {
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_remove_style_all(arc);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(arc, size, size);
  lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_bg_angles(arc, start_angle, end_angle);
  lv_arc_set_rotation(arc, 0);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 100);  // indicator = full bg range = the whole wedge
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, size / 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
  return arc;
}

// A small picture of the table layout for `n` players (same wedges and
// colors as the game screen), with seat `highlight` lit up - or every seat,
// for -1. Used by the setup screens so "player 2" means a visible seat.
lv_obj_t *make_seat_map(lv_obj_t *parent, int n, int highlight, lv_coord_t size) {
  lv_obj_t *holder = make_inert(parent);
  lv_obj_set_size(holder, size, size);
  for (int i = 0; i < n; i++) {
    int start, rotation;
    lv_coord_t dx, dy;
    wedge_geometry(n, i, &start, &rotation, &dx, &dy);
    int end = (n == 1) ? 360 : start + 360 / n;
    int gap = (n == 1) ? 0 : 3;
    bool lit = highlight < 0 || highlight == i;
    make_wedge_arc(holder, size, start + gap, end - gap, PLAYER_COLOR[i], lit ? LV_OPA_COVER : LV_OPA_20);
  }
  return holder;
}

// A small "< Back" pill along the bottom edge of a setup screen. It's a
// clickable child, so tapping it doesn't also trigger the screen's own
// tap-to-continue handler.
lv_obj_t *make_back_button(lv_obj_t *parent, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(btn, 100, 36);
  lv_obj_set_style_radius(btn, 18, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -24);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_14, 0xFFFFFF);
  lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
  lv_obj_center(lbl);
  return btn;
}

// ---- STATE_SELECT_PLAYERS -------------------------------------------------

void refresh_players_screen() {
  lv_label_set_text_fmt(s_players_value_label, "%d", s_player_count);
  if (s_players_map) lv_obj_del(s_players_map);
  s_players_map = make_seat_map(s_scr_players, s_player_count, -1, 64);
  lv_obj_align(s_players_map, LV_ALIGN_CENTER, 0, 58);
}

void players_screen_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_SELECT_PLAYERS || swallow_wake_tap()) return;
  show_assign_profiles_screen();
}

void show_players_screen() {
  s_state = STATE_SELECT_PLAYERS;
  lv_obj_t *scr = make_screen();

  lv_obj_t *title = make_label(scr, &lv_font_montserrat_20, 0xAAAAAA);
  lv_label_set_text(title, "PLAYERS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

  s_players_value_label = make_label(scr, &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_align(s_players_value_label, LV_ALIGN_CENTER, 0, -22);

  lv_obj_t *hint = make_label(scr, &lv_font_montserrat_14, 0x808080);
  lv_label_set_text(hint, "Rotate to choose - tap to continue");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -62);

  lv_obj_add_event_cb(scr, players_screen_clicked_cb, LV_EVENT_CLICKED, NULL);

  if (s_scr_players) lv_obj_del(s_scr_players);
  s_scr_players = scr;
  s_players_map = nullptr;  // went with the old screen
  refresh_players_screen();
  lv_scr_load(s_scr_players);
}

// ---- STATE_ASSIGN_PROFILES -------------------------------------------------
//
// One player slot at a time (title reads "PLAYER i OF n", with a seat map
// lighting up that seat): rotate the knob to cycle through Guest, each
// roster profile not already taken by a different slot this round, and
// "+ New Player"; tap confirms the current slot and advances to the next
// one, or into show_life_screen() after the last slot. Back steps to the
// previous slot (or the player count screen from the first).

int s_assign_slot = 0;
int s_assign_option = 0;
int8_t s_assign_ids[MAX_PROFILES];  // in-use, not-yet-taken-this-round profile ids
int s_assign_ids_count = 0;

lv_obj_t *s_scr_assign = nullptr;
lv_obj_t *s_assign_title_label = nullptr;
lv_obj_t *s_assign_value_label = nullptr;
lv_obj_t *s_assign_map = nullptr;

void recompute_assign_ids() {
  s_assign_ids_count = 0;
  for (int i = 0; i < MAX_PROFILES && s_assign_ids_count < MAX_PROFILES; i++) {
    if (!s_roster.profiles[i].in_use) continue;
    uint8_t id = s_roster.profiles[i].id;
    bool taken_elsewhere = false;
    for (int slot = 0; slot < s_player_count; slot++) {
      if (slot != s_assign_slot && s_player_profile_id[slot] == (int)id) {
        taken_elsewhere = true;
        break;
      }
    }
    if (!taken_elsewhere) s_assign_ids[s_assign_ids_count++] = (int8_t)id;
  }
}

// Total selectable options for the current slot: Guest + available profiles
// + "+ New Player".
int assign_option_total() {
  return s_assign_ids_count + 2;
}

void refresh_assign_screen() {
  recompute_assign_ids();
  s_assign_option = clampi(s_assign_option, 0, assign_option_total() - 1);

  lv_label_set_text_fmt(s_assign_title_label, "PLAYER %d OF %d", s_assign_slot + 1, s_player_count);
  lv_obj_set_style_text_color(s_assign_value_label, lv_color_hex(PLAYER_COLOR[s_assign_slot]), 0);

  if (s_assign_option == 0) {
    lv_label_set_text(s_assign_value_label, "Guest");
  } else if (s_assign_option == s_assign_ids_count + 1) {
    lv_label_set_text(s_assign_value_label, "+ New Player");
  } else {
    PlayerProfile *p = find_profile((uint8_t)s_assign_ids[s_assign_option - 1]);
    lv_label_set_text(s_assign_value_label, p ? p->name : "?");
  }

  if (s_assign_map) lv_obj_del(s_assign_map);
  s_assign_map = make_seat_map(s_scr_assign, s_player_count, s_assign_slot, 60);
  lv_obj_align(s_assign_map, LV_ALIGN_TOP_MID, 0, 14);
}

// Re-enters `slot` with its previous choice preselected (so Back doesn't
// make you re-scroll to the same profile).
void reopen_assign_slot(int slot) {
  s_assign_slot = slot;
  int prev = s_player_profile_id[slot];
  s_player_profile_id[slot] = -1;
  recompute_assign_ids();
  s_assign_option = 0;
  for (int i = 0; i < s_assign_ids_count; i++) {
    if (s_assign_ids[i] == prev) s_assign_option = i + 1;
  }
  refresh_assign_screen();
}

void assign_screen_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_ASSIGN_PROFILES || swallow_wake_tap()) return;

  if (s_assign_option == 0) {
    s_player_profile_id[s_assign_slot] = -1;
  } else if (s_assign_option == s_assign_ids_count + 1) {
    s_player_profile_id[s_assign_slot] = create_profile(nullptr);
  } else {
    s_player_profile_id[s_assign_slot] = s_assign_ids[s_assign_option - 1];
  }

  s_assign_slot++;
  if (s_assign_slot >= s_player_count) {
    show_life_screen();
    return;
  }
  s_assign_option = 0;
  refresh_assign_screen();
}

void assign_back_cb(lv_event_t *e) {
  if (s_state != STATE_ASSIGN_PROFILES) return;
  if (s_assign_slot == 0) {
    show_players_screen();
  } else {
    reopen_assign_slot(s_assign_slot - 1);
  }
}

// `resume_last_slot`: coming back from the starting-life screen, so keep
// every slot's choice and reopen the last one, instead of starting over.
void show_assign_profiles_screen(bool resume_last_slot) {
  s_state = STATE_ASSIGN_PROFILES;
  if (!resume_last_slot) {
    // Slots not yet visited this round must not carry over a profile id
    // from a previous game, or recompute_assign_ids() would wrongly treat
    // that profile as "already taken" by a slot not reached yet.
    for (int i = 0; i < MAX_PLAYERS; i++) s_player_profile_id[i] = -1;
  }
  lv_obj_t *scr = make_screen();

  s_assign_title_label = make_label(scr, &lv_font_montserrat_20, 0xAAAAAA);
  lv_obj_align(s_assign_title_label, LV_ALIGN_TOP_MID, 0, 84);

  s_assign_value_label = make_label(scr, &lv_font_montserrat_32, 0xFFFFFF);
  lv_obj_align(s_assign_value_label, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *hint = make_label(scr, &lv_font_montserrat_14, 0x808080);
  lv_label_set_text(hint, "Rotate to choose - tap to continue");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -72);

  make_back_button(scr, assign_back_cb);
  lv_obj_add_event_cb(scr, assign_screen_clicked_cb, LV_EVENT_CLICKED, NULL);

  if (s_scr_assign) lv_obj_del(s_scr_assign);
  s_scr_assign = scr;
  s_assign_map = nullptr;  // went with the old screen

  if (resume_last_slot) {
    reopen_assign_slot(s_player_count - 1);
  } else {
    s_assign_slot = 0;
    s_assign_option = 0;
    refresh_assign_screen();
  }
  lv_scr_load(s_scr_assign);
}

// ---- STATE_SELECT_LIFE ----------------------------------------------------

int default_life_for_players(int n) {
  if (n == 3 || n == 4) return 40;
  return 20;  // 1 and 2 players
}

void refresh_life_screen() {
  lv_label_set_text_fmt(s_life_value_label, "%d", s_starting_life);
}

void life_screen_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_SELECT_LIFE || swallow_wake_tap()) return;
  show_game_screen();
}

void life_back_cb(lv_event_t *e) {
  if (s_state != STATE_SELECT_LIFE) return;
  show_assign_profiles_screen(true);
}

void show_life_screen() {
  s_state = STATE_SELECT_LIFE;
  s_starting_life = default_life_for_players(s_player_count);
  lv_obj_t *scr = make_screen();

  lv_obj_t *title = make_label(scr, &lv_font_montserrat_20, 0xAAAAAA);
  lv_label_set_text(title, "STARTING LIFE");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 78);

  s_life_value_label = make_label(scr, &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_align(s_life_value_label, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *hint = make_label(scr, &lv_font_montserrat_14, 0x808080);
  lv_label_set_text(hint, "Rotate to choose - tap to continue");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -72);

  make_back_button(scr, life_back_cb);
  lv_obj_add_event_cb(scr, life_screen_clicked_cb, LV_EVENT_CLICKED, NULL);

  refresh_life_screen();

  if (s_scr_life) lv_obj_del(s_scr_life);
  s_scr_life = scr;
  lv_scr_load(s_scr_life);
}

// ---- STATE_GAME: counter chips ---------------------------------------------
//
// Each player's chips sit in a row (icon over number) in their own rotated
// holder further out along the wedge's bisector than the life total - i.e.
// just "below" it from that player's seat:
//   [heart: life] [shown counters...]
// The row only appears once at least one counter is shown. A separate holder (rather than growing
// the life content box) keeps each rotated layer small and lets the row be
// rebuilt without touching the life labels. Chip size/placement depends on
// the player count (chip_geometry()): sized so a full row clears the round
// bezel and stays inside its own wedge.
constexpr lv_coord_t COUNTER_ROW_ONE_PLAYER_DY = 100;  // 1 player: straight below center
constexpr int CHIP_LIFE = -1;  // chip_kind[] marker for the heart chip
constexpr uint32_t LIFE_CHIP_COLOR = 0xFF6B7A;
// Tap slop around each chip: a tap counts as hitting chip k if it's within
// half a chip pitch of it along the row (so the whole row has no dead gaps)
// and within row-height/2 + CHIP_HIT_EXTRA of the row's center line.
constexpr int CHIP_HIT_EXTRA = 3;

struct ChipGeometry {
  lv_coord_t w, h, gap;
  int row_radius;  // distance of the row's center from screen center (2+ players)
  int max_chips;
};

ChipGeometry chip_geometry() {
  if (s_player_count >= 3) return {32, 36, 3, 146, 1 + 3};  // life + 3
  return {28, 32, 3, 144, 1 + 4};                          // life + 4
}

// X offset of chip position `pos` (of `count`) from the row's center, in
// the row's own (unrotated) coordinates. Shared by layout and hit testing.
lv_coord_t chip_local_x(const ChipGeometry &g, int pos, int count) {
  return (lv_coord_t)((2 * pos - (count - 1)) * (g.w + g.gap) / 2);
}

void refresh_counter_chip(int player, int k) {
  PlayerShape &ps = s_players[player];
  if (!ps.counter_chip[k]) return;
  uint8_t kind = s_counter_kinds[k];
  bool viewing = k == s_view_counter;
  uint32_t icon_color = counter_color(kind);
  lv_obj_t *chip = ps.counter_chip[k];

  if (kind == CK_MONARCH) {
    // No number - the crown is lit (and outlined in gold) on the monarch.
    bool is_monarch = ps.kind_values[CK_MONARCH] > 0;
    if (!is_monarch) icon_color = 0x505050;
    set_border(chip, is_monarch ? 2 : 0, counter_color(kind));
    set_bg_opa(chip, is_monarch ? LV_OPA_90 : LV_OPA_50);
  } else {
    if (kind == CK_COMMANDER && viewing) icon_color = PLAYER_COLOR[s_cmdr_source];
    int value = chip_value(player, k);
    int lethal = counter_lethal(kind);
    set_text_int(ps.counter_label[k], "%d", value);
    set_text_color(ps.counter_label[k], lethal > 0 && value >= lethal ? 0xFF4040 : 0xFFFFFF);
    set_border(chip, viewing ? 2 : 0, 0xFFFFFF);
    set_bg_opa(chip, viewing ? LV_OPA_90 : LV_OPA_50);
  }
  set_img_color(ps.counter_icon_img[k], icon_color);
}

void refresh_life_chip(int player) {
  PlayerShape &ps = s_players[player];
  if (!ps.life_chip) return;
  set_text_int(ps.life_chip_label, "%d", ps.life);
  set_text_color(ps.life_chip_label, ps.life <= 0 ? 0xFF4040 : 0xFFFFFF);
  bool viewing = s_view_counter < 0;
  set_border(ps.life_chip, viewing ? 2 : 0, 0xFFFFFF);
  set_bg_opa(ps.life_chip, viewing ? LV_OPA_90 : LV_OPA_50);
}

lv_obj_t *make_chip_base(lv_obj_t *row, const ChipGeometry &g, int pos, int count) {
  lv_obj_t *chip = make_inert(row);
  lv_obj_set_size(chip, g.w, g.h);
  lv_obj_set_style_radius(chip, 8, 0);
  lv_obj_set_style_bg_color(chip, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_50, 0);
  lv_obj_set_style_border_color(chip, lv_color_white(), 0);
  lv_obj_align(chip, LV_ALIGN_CENTER, chip_local_x(g, pos, count), 0);
  return chip;
}

// An icon-over-number chip; returns the chip, its icon and its label.
lv_obj_t *make_chip(lv_obj_t *row, const ChipGeometry &g, int pos, int count, const lv_img_dsc_t *icon_src,
                    uint32_t color, bool icon_only, lv_obj_t **out_icon, lv_obj_t **out_label) {
  lv_obj_t *chip = make_chip_base(row, g, pos, count);

  lv_obj_t *icon = lv_img_create(chip);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_img_set_src(icon, icon_src);
  lv_obj_set_style_img_recolor(icon, lv_color_hex(color), 0);
  lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
  if (icon_only) {
    lv_obj_center(icon);
  } else {
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, g.h > 32 ? 3 : 1);
  }

  lv_obj_t *lbl = make_label(chip, &lv_font_montserrat_14, 0xFFFFFF);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, g.h > 32 ? 0 : 2);
  if (out_icon) *out_icon = icon;
  if (out_label) *out_label = lbl;
  return chip;
}

// (Re)creates the chips inside `player`'s counter row from the shared
// s_counter_kinds[] - called whenever kinds are shown/hidden. The holder
// itself is created once per game screen by make_counter_row(), so it
// keeps its place in the screen's z-order (under the menu button and any
// overlay).
void rebuild_counter_chips(int player) {
  PlayerShape &ps = s_players[player];
  if (!ps.counter_row) return;
  lv_obj_clean(ps.counter_row);
  ps.life_chip = nullptr;
  ps.life_chip_label = nullptr;
  for (int k = 0; k < MAX_COUNTERS; k++) {
    ps.counter_chip[k] = nullptr;
    ps.counter_icon_img[k] = nullptr;
    ps.counter_label[k] = nullptr;
  }

  int count = 0;
  if (s_counter_kind_count > 0) ps.chip_kind[count++] = CHIP_LIFE;
  for (int k = 0; k < s_counter_kind_count; k++) ps.chip_kind[count++] = k;
  ps.chip_count = count;
  show_hide(ps.counter_row, count > 0);

  ChipGeometry g = chip_geometry();
  for (int pos = 0; pos < count; pos++) {
    int k = ps.chip_kind[pos];
    if (k == CHIP_LIFE) {
      ps.life_chip = make_chip(ps.counter_row, g, pos, count, &icon_life_s, LIFE_CHIP_COLOR, false, nullptr,
                               &ps.life_chip_label);
      refresh_life_chip(player);
    } else {
      uint8_t kind = s_counter_kinds[k];
      ps.counter_chip[k] = make_chip(ps.counter_row, g, pos, count, counter_icon(kind, false), counter_color(kind),
                                     kind == CK_MONARCH, &ps.counter_icon_img[k], &ps.counter_label[k]);
      refresh_counter_chip(player, k);
    }
  }
}

void make_counter_row(lv_obj_t *parent, int idx) {
  PlayerShape &ps = s_players[idx];
  ChipGeometry g = chip_geometry();
  lv_coord_t dx = 0, dy = COUNTER_ROW_ONE_PLAYER_DY;
  int rotation = 0;
  if (s_player_count > 1) {
    int start_angle;
    lv_coord_t unused_x, unused_y;
    wedge_geometry(s_player_count, idx, &start_angle, &rotation, &unused_x, &unused_y);
    float rad = (float)((180 + idx * 360 / s_player_count) % 360) * DEG2RAD;
    dx = (lv_coord_t)lroundf(g.row_radius * sinf(rad));
    dy = (lv_coord_t)lroundf(-g.row_radius * cosf(rad));
  }

  lv_coord_t w = g.max_chips * g.w + (g.max_chips - 1) * g.gap;
  lv_obj_t *row = make_inert(parent);
  lv_obj_set_size(row, w, g.h);
  lv_obj_align(row, LV_ALIGN_CENTER, dx, dy);
  lv_obj_set_style_transform_pivot_x(row, w / 2, 0);
  lv_obj_set_style_transform_pivot_y(row, g.h / 2, 0);
  lv_obj_set_style_transform_angle(row, rotation * 10, 0);

  ps.counter_row = row;
  ps.counter_cx = 180 + dx;
  ps.counter_cy = 180 + dy;
  ps.counter_rotation_deg = rotation;
  rebuild_counter_chips(idx);
}

// Finds which chip (if any) screen point (x, y) hits, returning the owning
// player and the chip's chip_kind[] entry (a shown-kind index or
// CHIP_LIFE). Mirrors make_counter_row()'s layout: the tap is rotated back
// into each row's own coordinates (LVGL's transform_angle is clockwise,
// i.e. the usual rotation matrix in y-down screen coordinates), then
// matched against the chip slots along it.
bool counter_chip_at_point(lv_coord_t x, lv_coord_t y, int *out_player, int *out_chip_kind) {
  ChipGeometry g = chip_geometry();
  float half_pitch = (g.w + g.gap) / 2.0f;
  for (int i = 0; i < s_player_count; i++) {
    const PlayerShape &ps = s_players[i];
    if (ps.chip_count == 0) continue;
    float rad = ps.counter_rotation_deg * DEG2RAD;
    float dx = (float)(x - ps.counter_cx);
    float dy = (float)(y - ps.counter_cy);
    float lx = dx * cosf(rad) + dy * sinf(rad);
    float ly = -dx * sinf(rad) + dy * cosf(rad);
    if (fabsf(ly) > g.h / 2 + CHIP_HIT_EXTRA) continue;
    for (int pos = 0; pos < ps.chip_count; pos++) {
      if (fabsf(lx - chip_local_x(g, pos, ps.chip_count)) <= half_pitch) {
        *out_player = i;
        *out_chip_kind = ps.chip_kind[pos];
        return true;
      }
    }
  }
  return false;
}

// ---- STATE_GAME: player content ----------------------------------------------

// A small circular +/- tap indicator, placed at `align` within the
// (already-rotated) content box, so it inherits that rotation and lines up
// with whichever half of the wedge point_is_on_right_half() considers "left"
// or "right" for that player - see the comment there. Dim at rest, shown
// only on the active player (refresh_player()), and briefly
// flashed bright when tapped (flash_badge()).
lv_obj_t *make_pm_badge(lv_obj_t *parent, bool plus, lv_align_t align) {
  lv_obj_t *badge = lv_obj_create(parent);
  lv_obj_remove_style_all(badge);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(badge, BADGE_SIZE, BADGE_SIZE);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(badge, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_30, 0);
  lv_obj_align(badge, align, 0, 0);

  lv_obj_t *lbl = make_label(badge, &lv_font_montserrat_14, 0xFFFFFF);
  lv_label_set_text(lbl, plus ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS);
  lv_obj_center(lbl);
  return badge;
}

// Builds the (rotated) caption + value labels, +/- badges and knocked-out
// skull for one player, centered at align offset (dx, dy) with the given
// rotation.
void make_player_content(lv_obj_t *parent, int idx, lv_coord_t dx, lv_coord_t dy,
                          lv_coord_t w, lv_coord_t h, int rotation_deg, const lv_font_t *value_font) {
  PlayerShape &ps = s_players[idx];
  lv_obj_t *content = make_inert(parent);
  lv_obj_set_size(content, w, h);
  lv_obj_align(content, LV_ALIGN_CENTER, dx, dy);
  lv_obj_set_style_transform_pivot_x(content, w / 2, 0);
  lv_obj_set_style_transform_pivot_y(content, h / 2, 0);
  lv_obj_set_style_transform_angle(content, rotation_deg * 10, 0);

  // Text is filled in by refresh_player(): the player's name (or their
  // running life change) in the life view, or the counter's name while the
  // table views a counter. Long names are cut short with "..." - the menu's
  // Players screen has them in full.
  lv_obj_t *name = make_label(content, &lv_font_montserrat_20, 0xCCCCCC);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, w);
  lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
  ps.name_label = name;

  lv_obj_t *value = make_label(content, value_font, 0xFFFFFF);
  lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, 0);
  ps.value_label = value;

  ps.minus_badge = make_pm_badge(content, false, LV_ALIGN_BOTTOM_LEFT);
  ps.plus_badge = make_pm_badge(content, true, LV_ALIGN_BOTTOM_RIGHT);

  lv_obj_t *skull = lv_img_create(content);
  lv_obj_clear_flag(skull, LV_OBJ_FLAG_CLICKABLE);
  lv_img_set_src(skull, &icon_mana_b_s);
  lv_obj_set_style_img_recolor(skull, lv_color_hex(0xE0E0E0), 0);
  lv_obj_set_style_img_recolor_opa(skull, LV_OPA_COVER, 0);
  lv_obj_align(skull, LV_ALIGN_LEFT_MID, 2, 0);
  lv_obj_add_flag(skull, LV_OBJ_FLAG_HIDDEN);
  ps.out_icon = skull;
}

// ---- STATE_GAME: rendering ------------------------------------------------------

// Redraws everything about one player for the table's current view: wedge
// color (active / knocked out), caption, big number, badges and chips.
void refresh_player(int idx) {
  PlayerShape &ps = s_players[idx];
  if (!ps.value_label) return;
  bool active = idx == s_active_player;
  bool out = player_is_out(idx);

  set_arc_look(ps.shape, out ? OUT_COLOR : PLAYER_COLOR[idx], active ? LV_OPA_COVER : LV_OPA_40);
  show_hide(ps.out_icon, out);

  refresh_life_chip(idx);
  for (int k = 0; k < s_counter_kind_count; k++) refresh_counter_chip(idx, k);

  char buf[32];
  bool adjustable = true;
  if (s_view_counter < 0) {
    set_text_int(ps.value_label, "%d", ps.life);
    set_text_color(ps.value_label, 0xFFFFFF);
    bool show_delta = ps.recent_delta != 0 && millis() - ps.recent_delta_ms < DELTA_DISPLAY_MS;
    ps.recent_delta_shown = show_delta;
    bool first_banner = idx == s_first_banner_player && millis() - s_first_banner_ms < FIRST_BANNER_MS;
    if (first_banner && !show_delta) {
      set_text(ps.name_label, "Goes first!");
      set_text_color(ps.name_label, FIRST_BANNER_COLOR);
    } else if (show_delta) {
      set_text_int(ps.name_label, "%+d", ps.recent_delta);
      set_text_color(ps.name_label, ps.recent_delta < 0 ? 0xFF7070 : 0x70E080);
    } else {
      set_text(ps.name_label, slot_display_name(idx, buf, sizeof(buf)));
      set_text_color(ps.name_label, 0xCCCCCC);
    }
  } else {
    uint8_t kind = s_counter_kinds[s_view_counter];
    if (kind == CK_COMMANDER) {
      uint32_t src_color = PLAYER_COLOR[s_cmdr_source];
      if (idx == s_cmdr_source) {
        set_text(ps.name_label, "Attacking");
      } else {
        char name_buf[8];
        snprintf(buf, sizeof(buf), "From %s", slot_display_name(s_cmdr_source, name_buf, sizeof(name_buf)));
        set_text(ps.name_label, buf);
      }
      set_text_color(ps.name_label, src_color);
    } else {
      set_text(ps.name_label, counter_kind_name(kind, buf, sizeof(buf)));
      set_text_color(ps.name_label, counter_color(kind));
    }

    int16_t *value = view_value_ptr(idx, s_view_counter);
    adjustable = value != nullptr;
    if (!value) {
      set_text(ps.value_label, "-");
      set_text_color(ps.value_label, 0x606060);
    } else {
      int lethal = counter_lethal(kind);
      set_text_int(ps.value_label, "%d", *value);
      set_text_color(ps.value_label, lethal > 0 && *value >= lethal ? 0xFF4040 : 0xFFFFFF);
    }
  }
  // Only the active player's halves adjust on tap, so only they get badges.
  show_hide(ps.minus_badge, active && adjustable);
  show_hide(ps.plus_badge, active && adjustable);
}

void refresh_center_button() {
  if (!s_center_label || !s_center_icon) return;
  set_border(s_center_btn, s_battery_low ? 4 : 1, s_battery_low ? 0xFF3030 : 0x808080);
  bool switcher = s_counter_kind_count > 0;
  show_hide(s_center_label, !switcher);
  show_hide(s_center_icon, switcher);
  if (!switcher) return;
  const lv_img_dsc_t *icon = &icon_life_l;
  uint32_t color = LIFE_CHIP_COLOR;
  if (s_view_counter >= 0) {
    uint8_t kind = s_counter_kinds[s_view_counter];
    icon = counter_icon(kind, true);
    color = kind == CK_COMMANDER ? PLAYER_COLOR[s_cmdr_source] : counter_color(kind);
  }
  if (lv_img_get_src(s_center_icon) != icon) lv_img_set_src(s_center_icon, icon);
  set_img_color(s_center_icon, color);
}

void refresh_table() {
  if (s_view_counter >= s_counter_kind_count) s_view_counter = -1;
  for (int i = 0; i < s_player_count; i++) refresh_player(i);
  refresh_center_button();
}

// Game widgets are about to be deleted (New Game) - make sure nothing (e.g.
// a timer) touches them through stale pointers in the meantime.
void forget_game_widgets() {
  s_center_btn = s_center_label = s_center_icon = nullptr;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PlayerShape &ps = s_players[i];
    ps.shape = ps.value_label = ps.name_label = ps.out_icon = nullptr;
    ps.minus_badge = ps.plus_badge = ps.counter_row = ps.life_chip = ps.life_chip_label = nullptr;
    ps.chip_count = 0;
    for (int k = 0; k < MAX_COUNTERS; k++) ps.counter_chip[k] = ps.counter_icon_img[k] = ps.counter_label[k] = nullptr;
  }
}

// ---- STATE_GAME: changing values -------------------------------------------

// Folds the change into the newest history entry if it's a continuation
// (same player and kind, inside the merge window), else starts a new one.
void record_history(int idx, uint8_t kind, int delta, int value_after) {
  unsigned long now = millis();
  bool merge = s_history_open_slot >= 0 && s_history[s_history_open_slot].player == idx &&
               s_history[s_history_open_slot].kind == kind && (now - s_history_last_edit_ms) < HISTORY_MERGE_WINDOW_MS;
  if (!merge) {
    s_history_open_slot = s_history_next;
    HistoryEntry &entry = s_history[s_history_open_slot];
    entry.player = (int8_t)idx;
    entry.kind = kind;
    entry.delta = 0;
    s_history_next = (s_history_next + 1) % HISTORY_CAPACITY;
    if (s_history_count < HISTORY_CAPACITY) s_history_count++;
  }
  HistoryEntry &entry = s_history[s_history_open_slot];
  entry.delta += delta;
  entry.value_after = (int16_t)value_after;
  entry.life_after = (int16_t)s_players[idx].life;
  s_history_last_edit_ms = now;
}

// Changes a life total without logging history (the shared core of normal
// changes, commander damage and undo).
void change_life(int idx, int delta) {
  PlayerShape &ps = s_players[idx];
  ps.life += delta;
  unsigned long now = millis();
  ps.recent_delta = (now - ps.recent_delta_ms < DELTA_DISPLAY_MS) ? ps.recent_delta + delta : delta;
  ps.recent_delta_ms = now;
  // In-RAM only - the roster is flushed to NVS at game-end/roster-edit
  // checkpoints (see save_roster() call sites), not on every knob tick, to
  // avoid adding flash-write traffic to this already-hot path.
  PlayerProfile *profile = profile_for_slot(idx);
  if (profile) profile->total_life_delta += delta;
  refresh_player(idx);
  save_game();
  ble_sync_notify_game_state_changed();
}

void apply_life_delta(int idx, int delta) {
  change_life(idx, delta);
  record_history(idx, CK_NONE, delta, s_players[idx].life);
}

// Adjusts `idx`'s value in the current counter view. Counters never go
// below 0. Commander damage is also life loss, so it's mirrored onto the
// life total; the history entry is tagged with the damage's source.
void apply_view_delta(int idx, int delta) {
  int16_t *value = view_value_ptr(idx, s_view_counter);
  if (!value) return;
  int before = *value;
  *value = (int16_t)clampi(before + delta, 0, COUNTER_MAX_VALUE);
  int applied = *value - before;
  if (applied == 0) return;
  uint8_t kind = s_counter_kinds[s_view_counter];
  if (kind == CK_COMMANDER) {
    change_life(idx, -applied);  // refreshes + saves
    record_history(idx, CK_COMMANDER_BASE + s_cmdr_source, applied, *value);
  } else {
    refresh_player(idx);
    save_game();
    record_history(idx, kind, applied, *value);
  }
}

// A knob detent or +/- tap on player `idx`, in the table's current view.
void adjust_player_value(int idx, int delta) {
  note_view_activity();
  if (s_view_counter < 0) {
    apply_life_delta(idx, delta);
  } else {
    apply_view_delta(idx, delta);
  }
}

// Reverts the newest history entry (a whole merged run, e.g. "-7") and
// removes it. Returns false if there was nothing to undo.
bool undo_last_change() {
  if (s_history_count == 0) return false;
  int slot = newest_history_index();
  HistoryEntry e = s_history[slot];
  s_history_next = slot;
  s_history_count--;
  s_history_open_slot = -1;

  if (e.player < 0 || e.player >= s_player_count) return true;
  PlayerShape &ps = s_players[e.player];
  if (e.kind == CK_NONE) {
    change_life(e.player, -e.delta);
  } else {
    int16_t &value = ps.kind_values[value_slot(e.kind)];
    value = (int16_t)clampi(value - e.delta, 0, COUNTER_MAX_VALUE);
    if (is_commander_source_kind(e.kind)) {
      change_life(e.player, e.delta);  // give back the life it cost
    } else {
      refresh_player(e.player);
      save_game();
    }
  }
  return true;
}

// ---- STATE_GAME: taps ---------------------------------------------------------

// Bearing of screen point (x, y) measured clockwise from straight down
// (0 = bottom), matching wedge_geometry() (slot 0 = bottom, slot i is
// `i * step` degrees clockwise from there).
float bearing_at_point(lv_coord_t x, lv_coord_t y) {
  float dx = (float)(x - 180);
  float dy = (float)(y - 180);
  float bearing = atan2f(-dx, dy) * RAD2DEG;
  if (bearing < 0) bearing += 360.0f;
  return bearing;
}

// Which player's shape contains screen point (x, y)? Mirrors the geometry
// used to build the shapes above.
int player_at_point(lv_coord_t x, lv_coord_t y) {
  if (s_player_count == 1) return 0;
  float bearing = bearing_at_point(x, y);
  float step = 360.0f / s_player_count;
  int idx = ((int)floorf((bearing + step / 2.0f) / step)) % s_player_count;
  if (idx < 0) idx += s_player_count;
  return idx;
}

// True if point (x, y), already known to be inside player `idx`'s wedge, is
// on the half that reads as "right" from that player's seat - i.e. the side
// their +/- badges put the "+" on. Each wedge's content is rotated by
// `idx * step` degrees (see wedge_geometry()) purely as a rotation about the
// screen center with no mirroring, so the whole layout is rotationally
// symmetric: the same "which side of the bisector" test that works for
// player 0 (bisector at bearing 0, right = negative bearing) works for every
// other player once you measure the offset from *their* bisector instead.
bool point_is_on_right_half(lv_coord_t x, lv_coord_t y, int idx) {
  float bearing = bearing_at_point(x, y);
  float step = 360.0f / s_player_count;
  float offset = bearing - idx * step;
  while (offset > 180.0f) offset -= 360.0f;
  while (offset < -180.0f) offset += 360.0f;
  return offset < 0.0f;
}

// True while any full-screen overlay is up, so the knob and background taps
// stay inert underneath it.
bool any_overlay_open() {
  return s_reset_overlay || s_menu_overlay || s_history_overlay || s_players_list_overlay ||
         s_profile_detail_overlay || s_profile_delete_confirm_overlay || s_winner_overlay || s_counter_picker_overlay ||
         s_view_switcher_overlay || s_first_pick_overlay || s_dice_overlay;
}

// Switches the whole table to viewing shown counter k, or life for -1.
void set_view(int k) {
  s_view_counter = k;
  note_view_activity();
  refresh_table();
  ble_sync_notify_game_state_changed();  // the counters characteristic carries the view
}

// Makes `idx` the active player (the one the knob adjusts). Switching
// players always starts a new history entry, even if you switch back within
// the merge window - see record_history().
void set_active_player(int idx) {
  note_view_activity();
  if (idx == s_active_player) return;
  int prev = s_active_player;
  s_active_player = idx;
  s_history_open_slot = -1;
  refresh_player(prev);
  refresh_player(idx);
  save_game();
  ble_sync_notify_game_state_changed();
}

// Only one player can be the monarch: tapping a player's crown makes them
// the monarch, or clears it if they already are.
void toggle_monarch(int idx) {
  bool was = s_players[idx].kind_values[CK_MONARCH] > 0;
  for (int i = 0; i < MAX_PLAYERS; i++) s_players[i].kind_values[CK_MONARCH] = 0;
  if (!was) s_players[idx].kind_values[CK_MONARCH] = 1;
  note_view_activity();
  refresh_table();
  save_game();
}

void show_view_switcher();
void start_first_player_pick();

// The center button opens the view switcher, which also holds the counter
// picker and (at its bottom) the menu.
void menu_button_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_GAME || any_overlay_open() || swallow_wake_tap()) return;
  show_view_switcher();
}

// Briefly flashes a +/- badge bright, then fades it back to its dim resting
// opacity, to show that side has just been tapped.
void anim_set_bg_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void flash_badge(lv_obj_t *badge) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, badge);
  lv_anim_set_exec_cb(&a, anim_set_bg_opa_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
  lv_anim_set_time(&a, 200);
  lv_anim_start(&a);
}

void chip_tapped(int player, int chip_kind) {
  if (chip_kind == CHIP_LIFE) {
    set_active_player(player);
    set_view(-1);
    return;
  }
  uint8_t kind = s_counter_kinds[chip_kind];
  if (kind == CK_MONARCH) {
    toggle_monarch(player);
  } else if (kind == CK_COMMANDER) {
    // "This player's commander is attacking": everyone else now shows (and
    // adjusts) damage from it. The first other player becomes active so
    // the knob is immediately pointed at a target, not the attacker.
    if (s_view_counter == chip_kind && s_cmdr_source == player) {
      set_view(-1);
    } else {
      s_cmdr_source = player;
      set_active_player((player + 1) % s_player_count);
      set_view(chip_kind);
    }
  } else if (s_view_counter == chip_kind) {
    set_view(-1);  // tapping the viewed counter again goes back to life
  } else {
    set_active_player(player);
    set_view(chip_kind);
  }
}

// Tapping an inactive player's wedge only selects them (makes them the
// active player, which the knob adjusts). Tapping the already-active
// player adjusts their value: right half (as read from their seat) up,
// left half down. Chips are checked first.
void game_screen_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_GAME || any_overlay_open() || swallow_wake_tap()) return;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  int chip_player, chip_kind;
  if (counter_chip_at_point(p.x, p.y, &chip_player, &chip_kind)) {
    chip_tapped(chip_player, chip_kind);
    return;
  }

  int idx = player_at_point(p.x, p.y);
  if (idx < 0 || idx >= s_player_count) return;
  if (idx != s_active_player) {
    set_active_player(idx);
    return;
  }
  if (s_view_counter >= 0 && !view_value_ptr(idx, s_view_counter)) return;  // e.g. the attacking commander
  bool increase = point_is_on_right_half(p.x, p.y, idx);
  adjust_player_value(idx, increase ? 1 : -1);
  flash_badge(increase ? s_players[idx].plus_badge : s_players[idx].minus_badge);
}

// Long-pressing anywhere on the table opens the (shared) counter picker.
void game_screen_long_pressed_cb(lv_event_t *e) {
  if (s_state != STATE_GAME || any_overlay_open() || swallow_wake_tap()) return;
  show_counter_picker();
}

// Periodic housekeeping while a game is up: fade out expired running
// changes, and drop a counter view back to life after VIEW_IDLE_RETURN_MS
// of inactivity. Runs for the app's lifetime (started in app_ui_init()).
void ui_tick_timer_cb(lv_timer_t *timer) {
  if (s_state != STATE_GAME || s_shutting_down || !s_scr_game) return;
  unsigned long now = millis();
  for (int i = 0; i < s_player_count; i++) {
    PlayerShape &ps = s_players[i];
    if (ps.recent_delta_shown && now - ps.recent_delta_ms >= DELTA_DISPLAY_MS) refresh_player(i);
  }
  if (s_first_banner_player >= 0 && now - s_first_banner_ms >= FIRST_BANNER_MS) {
    int p = s_first_banner_player;
    s_first_banner_player = -1;
    if (p < s_player_count) refresh_player(p);
  }
  if (VIEW_IDLE_RETURN_MS > 0 && s_view_counter >= 0 && !any_overlay_open() &&
      now - s_view_last_activity_ms >= VIEW_IDLE_RETURN_MS) {
    set_view(-1);
  }
}

// A game-screen wedge. The arc itself has to be a full-screen 360x360
// object (its center is the screen center), and LVGL redraws an object's
// whole area whenever one of its styles changes - so re-highlighting one
// wedge used to redraw the entire screen (~125ms). Parenting the arc to a
// holder sized to just the wedge's bounding box makes LVGL clip that
// redraw to the box instead (children are clipped to their parent).
lv_obj_t *make_clipped_wedge(lv_obj_t *scr, int start_angle, int end_angle, uint32_t color) {
  if (end_angle - start_angle >= 360) return make_wedge_arc(scr, 360, start_angle, end_angle, color, LV_OPA_40);

  // Bounding box of the pie slice: the center plus points along its rim.
  float min_x = 180, max_x = 180, min_y = 180, max_y = 180;
  for (int a = start_angle; a <= end_angle; a += 2) {
    float rad = a * DEG2RAD;  // LVGL arc angles: 0 = 3 o'clock, clockwise (y down)
    float x = 180 + 180 * cosf(rad), y = 180 + 180 * sinf(rad);
    min_x = fminf(min_x, x); max_x = fmaxf(max_x, x);
    min_y = fminf(min_y, y); max_y = fmaxf(max_y, y);
  }
  lv_coord_t x1 = clampi((int)floorf(min_x) - 1, 0, 360), y1 = clampi((int)floorf(min_y) - 1, 0, 360);
  lv_coord_t x2 = clampi((int)ceilf(max_x) + 1, 0, 360), y2 = clampi((int)ceilf(max_y) + 1, 0, 360);

  lv_obj_t *holder = make_inert(scr);
  lv_obj_set_pos(holder, x1, y1);
  lv_obj_set_size(holder, x2 - x1, y2 - y1);
  lv_obj_t *arc = make_wedge_arc(holder, 360, start_angle, end_angle, color, LV_OPA_40);
  // Re-anchor at the holder's top-left (make_wedge_arc() centered it, and
  // in LVGL 8 a plain set_pos would only offset from that center) so the
  // arc's own center stays on the screen center.
  lv_obj_align(arc, LV_ALIGN_TOP_LEFT, -x1, -y1);
  return arc;
}

void show_game_screen(bool reset_life) {
  s_state = STATE_GAME;
  lv_obj_t *scr = make_screen();
  const lv_font_t *value_font = value_font_for_count(s_player_count);

  lv_coord_t content_w, content_h;
  wedge_content_size(s_player_count, &content_w, &content_h);

  if (reset_life) {
    s_active_player = 0;
    s_view_counter = -1;
    s_cmdr_source = 0;
  }

  for (int i = 0; i < s_player_count; i++) {
    int start_angle, rotation;
    lv_coord_t dx, dy;
    wedge_geometry(s_player_count, i, &start_angle, &rotation, &dx, &dy);
    int end_angle = (s_player_count == 1) ? 360 : start_angle + 360 / s_player_count;
    s_players[i].shape = make_clipped_wedge(scr, start_angle, end_angle, PLAYER_COLOR[i]);

    if (reset_life) {
      s_players[i].life = s_starting_life;
      s_players[i].recent_delta = 0;
      // Restart keeps the table's shown counter kinds (same table, same
      // format) but zeroes them; New Game clears them via clear_all_counters().
      memset(s_players[i].kind_values, 0, sizeof(s_players[i].kind_values));
    }

    make_player_content(scr, i, dx, dy, content_w, content_h, rotation, value_font);
    make_counter_row(scr, i);
  }
  refresh_table();  // also fills in every player's caption/value labels

  // SHORT_CLICKED rather than CLICKED so the release that ends a long press
  // (which opens the counter picker) doesn't also count as a +-1 tap.
  lv_obj_add_event_cb(scr, game_screen_clicked_cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(scr, game_screen_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);

  // Center button (menu / view switcher), drawn on top of everything since
  // it's created last. Big enough to hit easily: it's the main way to
  // switch which counter the table shows.
  lv_obj_t *menu_btn = lv_obj_create(scr);
  lv_obj_remove_style_all(menu_btn);
  lv_obj_set_size(menu_btn, 56, 56);
  lv_obj_set_style_radius(menu_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(menu_btn, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(menu_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(menu_btn, lv_color_hex(0x808080), 0);
  lv_obj_set_style_border_width(menu_btn, 1, 0);
  lv_obj_align(menu_btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(menu_btn, menu_button_clicked_cb, LV_EVENT_CLICKED, NULL);
  s_center_btn = menu_btn;

  lv_obj_t *menu_lbl = make_label(menu_btn, &lv_font_montserrat_20, 0xCCCCCC);
  lv_label_set_text(menu_lbl, LV_SYMBOL_LIST);
  lv_obj_center(menu_lbl);
  s_center_label = menu_lbl;

  lv_obj_t *center_icon = lv_img_create(menu_btn);
  lv_obj_clear_flag(center_icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_img_recolor_opa(center_icon, LV_OPA_COVER, 0);
  lv_obj_center(center_icon);
  s_center_icon = center_icon;
  refresh_center_button();

  // Async, not lv_obj_del(): show_game_screen() is called from Restart
  // Game's own click handler (a button inside s_scr_game's tree), so a
  // synchronous delete here would free the still-executing button (and its
  // overlay) out from under LVGL's own event dispatch - see the note in
  // hide_end_game_confirm() for why that's unsafe. This was the actual bug
  // behind Restart Game appearing to keep the pre-restart totals: the freed
  // memory was getting reused for the very labels this function creates
  // above, so those fresh "starting life" labels got clobbered right after
  // being set.
  if (s_scr_game) lv_obj_del_async(s_scr_game);
  s_scr_game = scr;
  lv_scr_load(s_scr_game);

  save_game();
  ble_sync_notify_game_state_changed();

  if (reset_life) start_first_player_pick();  // new game or restart
}

// ---- battery --------------------------------------------------------------
//
// GPIO1 is ADC1 channel 0 on the ESP32-S3, and per Waveshare's own
// 01_ADC_Test example for this exact board (in the Demo.zip linked from the
// README), the battery sits behind an onboard 1:2 divider there - the pin
// reads half the actual battery voltage. The 3.3-4.2V range is a plain
// linear approximation of a single-cell LiPo's discharge curve (not a
// fuel-gauge-accurate reading), which is all a glance at the menu needs.
constexpr int BATTERY_ADC_PIN = 1;
constexpr float BATTERY_EMPTY_V = 3.3f;
constexpr float BATTERY_FULL_V = 4.2f;
constexpr lv_coord_t BATTERY_W = 40;
constexpr lv_coord_t BATTERY_H = 18;

// Below this, a single-cell LiPo is close enough to its protection-circuit
// cutoff (typically ~3.0V at the cell) that continuing to draw from it risks
// an over-discharge brownout instead of a clean shutdown - so this sits a
// little above BATTERY_EMPTY_V's already-conservative "0%" mark, not at it.
constexpr float BATTERY_CRITICAL_V = 3.1f;
// analogRead() on the ESP32 is noisy enough that a single low sample doesn't
// mean much - require this many consecutive checks (spaced
// BATTERY_CRITICAL_CHECK_MS apart) below BATTERY_CRITICAL_V before actually
// shutting down, so one stray reading mid-game can't trigger it.
constexpr int BATTERY_CRITICAL_CONSECUTIVE = 3;
constexpr uint32_t BATTERY_CRITICAL_CHECK_MS = 10000;

float read_battery_voltage() {
  int raw = analogRead(BATTERY_ADC_PIN);
  float pin_voltage = (raw / 4095.0f) * 3.3f;
  return pin_voltage * 2.0f;  // undo the onboard divider
}

int read_battery_percent() {
  float pct = (read_battery_voltage() - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0f;
  return clampi((int)lroundf(pct), 0, 100);
}

void anim_set_width_cb(void *obj, int32_t v) {
  lv_obj_set_width((lv_obj_t *)obj, v);
}

// Re-reads the battery and animates the fill bar to the new level - called
// once when the menu opens (growing from empty) and then periodically while
// it stays open.
void refresh_battery_indicator() {
  if (!s_battery_fill) return;
  int pct = read_battery_percent();
  lv_label_set_text_fmt(s_battery_label, "%d%%", pct);

  uint32_t color = pct <= 20 ? 0xE9503F : (pct <= 50 ? 0xE9C23F : 0x53D07A);
  lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(color), 0);

  lv_coord_t target_w = (lv_coord_t)((BATTERY_W - 4) * pct / 100);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_battery_fill);
  lv_anim_set_exec_cb(&a, anim_set_width_cb);
  lv_anim_set_values(&a, lv_obj_get_width(s_battery_fill), target_w);
  lv_anim_set_time(&a, 500);
  lv_anim_start(&a);
}

void battery_timer_cb(lv_timer_t *timer) {
  refresh_battery_indicator();
}

// Turns the backlight off and puts the ESP32-S3 into deep sleep. This board
// has no PMU/load switch to actually cut battery power to the rest of the
// circuit, so deep sleep - with no wake source armed below - is as close to
// "off" as software can get: current draw drops to the chip's deep-sleep
// floor and stays there until the board is reset or physically power-cycled.
void enter_deep_sleep_cb(lv_timer_t *timer) {
  setUpdutySubdivide(0);
  esp_deep_sleep_start();
}

// Takes over the whole display with a shutdown message, then deep-sleeps a
// couple of seconds later (via enter_deep_sleep_cb) once it's had a chance
// to actually render. Whatever screen/overlay was showing is simply
// abandoned - it's about to be permanently asleep, so there's nothing to
// clean up on the way out.
void show_low_battery_and_shutdown() {
  if (s_shutting_down) return;
  s_shutting_down = true;

  save_game();  // final flush - the game resumes on next boot either way

  lv_obj_t *scr = make_screen();

  lv_obj_t *icon = make_label(scr, &lv_font_montserrat_48, 0xE9503F);
  lv_label_set_text(icon, LV_SYMBOL_BATTERY_EMPTY);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t *title = make_label(scr, &lv_font_montserrat_24, 0xFFFFFF);
  lv_label_set_text(title, "BATTERY CRITICAL");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 20);

  lv_obj_t *sub = make_label(scr, &lv_font_montserrat_14, 0xAAAAAA);
  lv_label_set_text(sub, "Shutting down - recharge and\npower-cycle to resume");
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 55);

  lv_scr_load(scr);

  lv_timer_t *t = lv_timer_create(enter_deep_sleep_cb, 2500, NULL);
  lv_timer_set_repeat_count(t, 1);
}

int s_battery_critical_streak = 0;

// Runs for the whole lifetime of the app (started once in app_ui_init, never
// deleted) so a critically low battery is caught no matter what screen is up
// - not just while the menu happens to be open like the indicator above.
void critical_battery_timer_cb(lv_timer_t *timer) {
  if (s_shutting_down) return;
  if (read_battery_voltage() < BATTERY_CRITICAL_V) {
    s_battery_critical_streak++;
  } else {
    s_battery_critical_streak = 0;
  }
  if (s_battery_critical_streak >= BATTERY_CRITICAL_CONSECUTIVE) {
    show_low_battery_and_shutdown();
  }
}

// In-game low-battery warning (a red rim on the center button, plus the
// percentage at the top of the center-button list), well before the
// critical shutdown above. Hysteresis so it doesn't flicker around the
// threshold, and a reading below BATTERY_ABSENT_V is treated as "no battery
// fitted" (USB power only, floating pin) rather than an empty one.
constexpr int BATTERY_LOW_PCT = 15;
constexpr int BATTERY_LOW_CLEAR_PCT = 20;
constexpr float BATTERY_ABSENT_V = 2.5f;
constexpr uint32_t BATTERY_LOW_CHECK_MS = 20000;

void battery_low_timer_cb(lv_timer_t *timer) {
  if (s_shutting_down) return;
  bool low = false;
  if (read_battery_voltage() >= BATTERY_ABSENT_V) {
    int pct = read_battery_percent();
    s_battery_low_pct = pct;
    low = s_battery_low ? pct < BATTERY_LOW_CLEAR_PCT : pct <= BATTERY_LOW_PCT;
  }
  if (low != s_battery_low) {
    s_battery_low = low;
    refresh_center_button();
  }
}

// Builds the battery icon (frame + nub + fill) and its % label as one
// self-centering row, parented to `parent`.
void make_battery_indicator(lv_obj_t *parent) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);

  lv_obj_t *frame = make_inert(row);
  lv_obj_set_size(frame, BATTERY_W, BATTERY_H);
  lv_obj_set_style_radius(frame, 3, 0);
  lv_obj_set_style_border_width(frame, 2, 0);
  lv_obj_set_style_border_color(frame, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);

  lv_obj_t *nub = make_inert(row);
  lv_obj_set_size(nub, 3, 8);
  lv_obj_set_style_bg_color(nub, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);

  s_battery_fill = make_inert(frame);
  lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
  lv_obj_set_size(s_battery_fill, 0, BATTERY_H - 4);
  lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 2, 0);

  s_battery_label = make_label(row, &lv_font_montserrat_14, 0xFFFFFF);
  lv_label_set_text(s_battery_label, "--%");
}

// ---- menu overlay -----------------------------------------------------------
//
// Most-used first (History, Players, Dice & Coin, Exit), then the two
// game-ending actions at the bottom, both behind a confirmation. Tapping
// outside the buttons closes it too, and the knob scrolls it.

void hide_menu() {
  if (s_battery_timer) {
    lv_timer_del(s_battery_timer);
    s_battery_timer = nullptr;
  }
  s_battery_fill = nullptr;
  s_battery_label = nullptr;
  if (s_menu_overlay) {
    // Async: this runs from a button that's a child of s_menu_overlay, and
    // synchronously deleting an ancestor of the widget whose own event is
    // still on the call stack is undefined behavior in LVGL - it looks like
    // it works until the freed memory gets reused (see the same note on
    // hide_end_game_confirm() and hide_history()).
    lv_obj_del_async(s_menu_overlay);
    s_menu_overlay = nullptr;
  }
  s_knob_scroll_target = nullptr;
  s_state = STATE_GAME;
}

void menu_exit_clicked_cb(lv_event_t *e) {
  hide_menu();
}

bool any_profile_assigned() {
  for (int i = 0; i < s_player_count; i++) {
    if (s_player_profile_id[i] >= 0) return true;
  }
  return false;
}

void menu_restart_clicked_cb(lv_event_t *e) {
  hide_menu();
  show_end_game_confirm(false);
}

void menu_new_game_clicked_cb(lv_event_t *e) {
  hide_menu();
  show_end_game_confirm(true);
}

void menu_history_clicked_cb(lv_event_t *e) {
  hide_menu();
  show_history();
}

void menu_players_clicked_cb(lv_event_t *e) {
  hide_menu();
  show_players_list();
}

void show_dice();

void menu_dice_clicked_cb(lv_event_t *e) {
  hide_menu();
  show_dice();
}

lv_obj_t *make_menu_button(lv_obj_t *parent, const char *text, uint32_t color) {
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(btn, 210, 38);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

  lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_14, 0xFFFFFF);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, 196);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return btn;
}

// A scrollable flex column for an overlay's list/content area. Left
// clickable (the lv_obj default): LVGL only drag-scrolls from a pressed
// object, and it also means a tap in a gap between rows lands here rather
// than on the overlay, so it never counts as a tap "outside".
lv_obj_t *make_scroll_column(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t row_gap) {
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, w, h);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, row_gap, 0);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  return col;
}

void show_menu() {
  s_state = STATE_GAME;  // knob/taps stay blocked via any_overlay_open()
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  // Scrollable (flick or knob): the menu is taller than the round display's
  // safe content area.
  lv_obj_t *content = make_scroll_column(overlay, 230, 260, 10);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(content, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *title = make_label(content, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, "MENU");

  make_battery_indicator(content);

  lv_obj_t *history_btn = make_menu_button(content, "History / Undo", 0x2A2A2A);
  lv_obj_add_event_cb(history_btn, menu_history_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *players_btn = make_menu_button(content, "Players", 0x2A2A2A);
  lv_obj_add_event_cb(players_btn, menu_players_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *dice_btn = make_menu_button(content, "Dice & Coin", 0x2A2A2A);
  lv_obj_add_event_cb(dice_btn, menu_dice_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *exit_btn = make_menu_button(content, "Exit", 0x2E8B3D);
  lv_obj_add_event_cb(exit_btn, menu_exit_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *restart_btn = make_menu_button(content, "Restart Game", 0x6B3A2A);
  lv_obj_add_event_cb(restart_btn, menu_restart_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *new_game_btn = make_menu_button(content, "New Game", 0x8B2E2E);
  lv_obj_add_event_cb(new_game_btn, menu_new_game_clicked_cb, LV_EVENT_CLICKED, NULL);

  close_on_outside_tap(overlay, content, hide_menu);
  s_menu_overlay = overlay;
  s_knob_scroll_target = content;

  refresh_battery_indicator();
  s_battery_timer = lv_timer_create(battery_timer_cb, 4000, NULL);
}

// ---- history overlay --------------------------------------------------------
//
// Newest first. Undo (top) reverts the newest entry - a whole merged run,
// e.g. "-7" - including the life a commander-damage entry cost.

void hide_history() {
  if (s_history_overlay) {
    lv_obj_del_async(s_history_overlay);  // see the note in hide_menu()
    s_history_overlay = nullptr;
  }
  s_knob_scroll_target = nullptr;
  s_state = STATE_GAME;
}

void history_back_clicked_cb(lv_event_t *e) {
  hide_history();
  show_menu();
}

void history_undo_clicked_cb(lv_event_t *e) {
  undo_last_change();
  hide_history();
  show_history();  // rebuild with the entry gone
}

// "Alice  -7 -> 33", "Alice  Poison +2 -> 5", "Bob  Cmdr Alice +3 -> 9".
void format_history_entry(const HistoryEntry &entry, char *out, size_t out_len) {
  char name_buf[8];
  const char *name = slot_display_name(entry.player, name_buf, sizeof(name_buf));
  if (entry.kind == CK_NONE) {
    snprintf(out, out_len, "%s  %+d -> %d", name, entry.delta, entry.value_after);
  } else {
    char kind_buf[24];
    snprintf(out, out_len, "%s  %s %+d -> %d", name, counter_kind_name(entry.kind, kind_buf, sizeof(kind_buf)),
             entry.delta, entry.value_after);
  }
}

void show_history() {
  s_state = STATE_GAME;
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *title = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, "HISTORY");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

  char buf[64];
  if (s_history_count > 0) {
    char entry_buf[56];
    format_history_entry(s_history[newest_history_index()], entry_buf, sizeof(entry_buf));
    snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP " Undo: %s", entry_buf);
    lv_obj_t *undo_btn = make_menu_button(overlay, buf, 0x7A5A1E);
    lv_obj_align(undo_btn, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_event_cb(undo_btn, history_undo_clicked_cb, LV_EVENT_CLICKED, NULL);
  }

  // Scrollable (flick or knob), newest first.
  lv_obj_t *list = make_scroll_column(overlay, 230, 160, 4);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 106);

  if (s_history_count == 0) {
    lv_obj_t *empty = make_label(list, &lv_font_montserrat_14, 0x808080);
    lv_label_set_text(empty, "No changes yet");
  } else {
    for (int i = 0; i < s_history_count; i++) {
      int idx = (s_history_next - 1 - i + HISTORY_CAPACITY) % HISTORY_CAPACITY;
      const HistoryEntry &entry = s_history[idx];
      lv_obj_t *row = make_label(list, &lv_font_montserrat_14, PLAYER_COLOR[entry.player]);
      lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
      lv_obj_set_width(row, 220);
      lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
      format_history_entry(entry, buf, sizeof(buf));
      lv_label_set_text(row, buf);
    }
  }

  lv_obj_t *back_btn = make_menu_button(overlay, LV_SYMBOL_LEFT " Back", 0x2A2A2A);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_add_event_cb(back_btn, history_back_clicked_cb, LV_EVENT_CLICKED, NULL);

  close_on_outside_tap(overlay, list, hide_history);
  s_history_overlay = overlay;
  s_knob_scroll_target = list;
}

// ---- players list overlay --------------------------------------------------
//
// Roster viewer opened from the menu: name + wins/games per row. Tapping a
// row opens that profile's details (stats, plus a Delete button that asks
// for confirmation) rather than going straight to deleting it. No on-device
// rename - no keyboard on this board, see the "player profiles" section.

uint8_t s_pending_profile_id = 0;

void hide_players_list() {
  if (s_players_list_overlay) {
    lv_obj_del_async(s_players_list_overlay);  // see the note in hide_menu()
    s_players_list_overlay = nullptr;
  }
  s_knob_scroll_target = nullptr;
  s_state = STATE_GAME;
}

void players_list_back_clicked_cb(lv_event_t *e) {
  hide_players_list();
  show_menu();
}

void hide_profile_detail() {
  if (s_profile_detail_overlay) {
    lv_obj_del_async(s_profile_detail_overlay);  // see the note in hide_menu()
    s_profile_detail_overlay = nullptr;
  }
}

void hide_profile_delete_confirm() {
  if (s_profile_delete_confirm_overlay) {
    lv_obj_del_async(s_profile_delete_confirm_overlay);  // see the note in hide_menu()
    s_profile_delete_confirm_overlay = nullptr;
  }
}

void profile_delete_confirm_yes_cb(lv_event_t *e) {
  delete_profile(s_pending_profile_id);
  hide_profile_delete_confirm();
  hide_profile_detail();
  hide_players_list();
  show_players_list();  // rebuild with the profile gone
  refresh_table();      // a deleted profile's slot now reads as "P<n>"
}

void profile_delete_confirm_no_cb(lv_event_t *e) {
  hide_profile_delete_confirm();
}

void show_profile_delete_confirm() {
  PlayerProfile *p = find_profile(s_pending_profile_id);
  if (!p) return;
  lv_obj_t *overlay = make_overlay(LV_OPA_80);

  lv_obj_t *msg = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text_fmt(msg, "Delete %s?", p->name);
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t *yes = make_confirm_button(overlay, "Yes", 0x8B2E2E, -50);
  lv_obj_add_event_cb(yes, profile_delete_confirm_yes_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *no = make_confirm_button(overlay, "No", 0x2E8B3D, 50);
  lv_obj_add_event_cb(no, profile_delete_confirm_no_cb, LV_EVENT_CLICKED, NULL);

  s_profile_delete_confirm_overlay = overlay;
}

void profile_detail_delete_cb(lv_event_t *e) {
  show_profile_delete_confirm();
}

void profile_detail_close_cb(lv_event_t *e) {
  hide_profile_detail();
}

void show_profile_detail() {
  PlayerProfile *p = find_profile(s_pending_profile_id);
  if (!p) return;
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *name = make_label(overlay, &lv_font_montserrat_24, 0xFFFFFF);
  lv_label_set_text(name, p->name);
  lv_obj_align(name, LV_ALIGN_CENTER, 0, -78);

  int win_pct = p->games_played ? (int)(p->wins * 100 / p->games_played) : 0;
  lv_obj_t *stats = make_label(overlay, &lv_font_montserrat_14, 0xBBBBBB);
  lv_obj_set_style_text_align(stats, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text_fmt(stats, "Games %u   Wins %u (%d%%)\nNet life change %+ld", (unsigned)p->games_played,
                        (unsigned)p->wins, win_pct, (long)p->total_life_delta);
  lv_obj_align(stats, LV_ALIGN_CENTER, 0, -32);

  lv_obj_t *del = make_confirm_button(overlay, "Delete", 0x8B2E2E, -50);
  lv_obj_add_event_cb(del, profile_detail_delete_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *close = make_confirm_button(overlay, "Close", 0x2A2A2A, 50);
  lv_obj_add_event_cb(close, profile_detail_close_cb, LV_EVENT_CLICKED, NULL);

  close_on_outside_tap(overlay, nullptr, hide_profile_detail);
  s_profile_detail_overlay = overlay;
}

void profile_row_clicked_cb(lv_event_t *e) {
  lv_obj_t *row = lv_event_get_current_target(e);
  s_pending_profile_id = (uint8_t)(intptr_t)lv_obj_get_user_data(row);
  show_profile_detail();
}

void show_players_list() {
  s_state = STATE_GAME;
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *title = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, "PLAYERS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

  lv_obj_t *list = make_scroll_column(overlay, 260, 190, 6);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, -10);

  if (count_in_use_profiles() == 0) {
    lv_obj_t *empty = make_label(list, &lv_font_montserrat_14, 0x808080);
    lv_label_set_text(empty, "No players yet -\nadd one from the MTGCounter app,\nor pick + New Player at setup");
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
  } else {
    for (int i = 0; i < MAX_PROFILES; i++) {
      if (!s_roster.profiles[i].in_use) continue;
      PlayerProfile &p = s_roster.profiles[i];
      lv_obj_t *row = lv_obj_create(list);
      lv_obj_remove_style_all(row);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_size(row, 240, 34);
      lv_obj_set_style_radius(row, 6, 0);
      lv_obj_set_style_bg_color(row, lv_color_hex(0x2A2A2A), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_user_data(row, (void *)(intptr_t)p.id);
      lv_obj_add_event_cb(row, profile_row_clicked_cb, LV_EVENT_CLICKED, NULL);

      lv_obj_t *name_lbl = make_label(row, &lv_font_montserrat_14, 0xFFFFFF);
      lv_label_set_text(name_lbl, p.name);
      lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 10, 0);

      lv_obj_t *stats_lbl = make_label(row, &lv_font_montserrat_14, 0x808080);
      lv_label_set_text_fmt(stats_lbl, "%u/%u " LV_SYMBOL_RIGHT, (unsigned)p.wins, (unsigned)p.games_played);
      lv_obj_align(stats_lbl, LV_ALIGN_RIGHT_MID, -10, 0);
    }
  }

  lv_obj_t *back_btn = make_menu_button(overlay, LV_SYMBOL_LEFT " Back", 0x2A2A2A);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_add_event_cb(back_btn, players_list_back_clicked_cb, LV_EVENT_CLICKED, NULL);

  close_on_outside_tap(overlay, list, hide_players_list);
  s_players_list_overlay = overlay;
  s_knob_scroll_target = list;
}

// ---- end-game confirmation overlay ------------------------------------------
//
// Shared by Restart Game and New Game. Destructive answers are red and safe
// ones green, everywhere.

bool s_pending_game_end_is_new_game = false;

void hide_end_game_confirm() {
  if (s_reset_overlay) {
    // Async, not lv_obj_del(): this runs from the Yes/No buttons, which are
    // children of s_reset_overlay - synchronously deleting an ancestor of
    // the widget whose CLICKED event is still being dispatched is undefined
    // behavior in LVGL (it can appear to work until the freed memory gets
    // reused for something else, e.g. the very screen rebuilt right after -
    // this was the actual cause of "Restart Game" appearing to keep the old
    // totals). lv_obj_del_async() defers the real deletion until after the
    // current event finishes, which is the pattern LVGL's own docs call for
    // here.
    lv_obj_del_async(s_reset_overlay);
    s_reset_overlay = nullptr;
  }
  s_state = STATE_GAME;
}

void finish_new_game() {
  clear_saved_game();
  reset_history();
  clear_all_counters();
  forget_game_widgets();
  show_players_screen();
  if (s_scr_game) {
    lv_obj_del_async(s_scr_game);  // see the note in hide_end_game_confirm()
    s_scr_game = nullptr;
  }
}

void restart_game() {
  reset_history();
  show_game_screen();  // rebuilds with the current s_player_count/s_starting_life
}

void end_game_confirm_yes_cb(lv_event_t *e) {
  hide_end_game_confirm();
  if (any_profile_assigned()) {
    show_winner_prompt(s_pending_game_end_is_new_game);
  } else if (s_pending_game_end_is_new_game) {
    finish_new_game();
  } else {
    restart_game();
  }
}

void end_game_confirm_no_cb(lv_event_t *e) {
  hide_end_game_confirm();
}

lv_obj_t *make_confirm_button(lv_obj_t *parent, const char *text, uint32_t color, lv_coord_t x_off) {
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 90, 44);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_align(btn, LV_ALIGN_CENTER, x_off, 40);

  lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return btn;
}

void show_end_game_confirm(bool is_new_game) {
  s_pending_game_end_is_new_game = is_new_game;
  s_state = STATE_GAME;
  lv_obj_t *overlay = make_overlay(LV_OPA_80);

  lv_obj_t *msg = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(msg, is_new_game ? "New game?" : "Restart game?");
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -36);

  lv_obj_t *sub = make_label(overlay, &lv_font_montserrat_14, 0x999999);
  lv_label_set_text(sub, is_new_game ? "Ends this game and goes to setup" : "Same players, back to starting life");
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, -8);

  lv_obj_t *yes = make_confirm_button(overlay, "Yes", 0x8B2E2E, -50);
  lv_obj_add_event_cb(yes, end_game_confirm_yes_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *no = make_confirm_button(overlay, "No", 0x2E8B3D, 50);
  lv_obj_add_event_cb(no, end_game_confirm_no_cb, LV_EVENT_CLICKED, NULL);

  s_reset_overlay = overlay;
}

// ---- winner prompt overlay --------------------------------------------------
//
// Shown before New Game / Restart Game actually end the game, but only if
// at least one slot has a profile assigned (see any_profile_assigned()) -
// an all-Guest game has nothing to attribute a win to. One button per
// assigned player (players still standing first; knocked-out ones greyed
// and marked) plus "Skip", then finish_game_end() applies the stats and
// carries out whichever action (Restart or New Game) triggered the prompt.

void hide_winner_prompt() {
  if (s_winner_overlay) {
    lv_obj_del_async(s_winner_overlay);  // see the note in hide_menu()
    s_winner_overlay = nullptr;
  }
  s_knob_scroll_target = nullptr;
  s_state = STATE_GAME;
}

void finish_game_end(int winner_slot) {
  for (int i = 0; i < s_player_count; i++) {
    PlayerProfile *p = profile_for_slot(i);
    if (!p) continue;
    p->games_played++;
    if (i == winner_slot) p->wins++;
  }
  save_roster();
  ble_sync_notify_profiles_changed();

  if (s_pending_game_end_is_new_game) {
    finish_new_game();
  } else {
    restart_game();
  }
}

void winner_chosen_cb(lv_event_t *e) {
  lv_obj_t *btn = lv_event_get_current_target(e);
  int slot = (int)(intptr_t)lv_obj_get_user_data(btn);
  hide_winner_prompt();
  finish_game_end(slot);
}

void winner_skip_cb(lv_event_t *e) {
  hide_winner_prompt();
  finish_game_end(-1);
}

void show_winner_prompt(bool is_new_game) {
  s_pending_game_end_is_new_game = is_new_game;
  s_state = STATE_GAME;  // knob/taps stay blocked via any_overlay_open()
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *content = make_scroll_column(overlay, 230, 260, 10);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(content, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *title = make_label(content, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, "Who won?");

  for (int pass = 0; pass < 2; pass++) {  // still standing first, then knocked out
    for (int i = 0; i < s_player_count; i++) {
      PlayerProfile *p = profile_for_slot(i);
      if (!p || player_is_out(i) != (pass == 1)) continue;
      char buf[32];
      if (pass == 1) snprintf(buf, sizeof(buf), "%s (out)", p->name);
      lv_obj_t *btn = make_menu_button(content, pass == 1 ? buf : p->name, pass == 1 ? 0x3A3A3A : PLAYER_COLOR[i]);
      lv_obj_set_user_data(btn, (void *)(intptr_t)i);
      lv_obj_add_event_cb(btn, winner_chosen_cb, LV_EVENT_CLICKED, NULL);
    }
  }

  lv_obj_t *skip_btn = make_menu_button(content, "Skip", 0x2A2A2A);
  lv_obj_add_event_cb(skip_btn, winner_skip_cb, LV_EVENT_CLICKED, NULL);

  s_winner_overlay = overlay;
  s_knob_scroll_target = content;
}

// ---- view switcher overlay ----------------------------------------------------
//
// Opened from the center button: one big button per thing the table can
// show - Life, each shown counter, and for commander damage one button per
// attacker ("Alice's cmdr") - with the current one outlined, then
// "Counters..." (the counter picker, to show/hide counters) and Menu at the
// bottom. With no counters shown it's just Counters... and Menu. Much easier to hit than the chips. Choosing one closes it; so
// does tapping outside.

constexpr intptr_t SWITCH_LIFE = 0x100;
constexpr intptr_t SWITCH_COMMANDER = 0x200;  // | source slot
constexpr intptr_t SWITCH_MENU = 0x300;
constexpr intptr_t SWITCH_PICKER = 0x400;

void hide_view_switcher() {
  if (s_view_switcher_overlay) {
    lv_obj_del_async(s_view_switcher_overlay);  // see the note in hide_menu()
    s_view_switcher_overlay = nullptr;
  }
  s_knob_scroll_target = nullptr;
  s_state = STATE_GAME;
}

void view_switch_clicked_cb(lv_event_t *e) {
  intptr_t code = (intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  hide_view_switcher();
  if (code == SWITCH_MENU) {
    show_menu();
  } else if (code == SWITCH_PICKER) {
    show_counter_picker();
  } else if (code == SWITCH_LIFE) {
    set_view(-1);
  } else if ((code & 0xF00) == SWITCH_COMMANDER) {
    int src = (int)(code & 0xFF);
    int k = find_counter_kind(CK_COMMANDER);
    if (k < 0) return;
    s_cmdr_source = src;
    // Point the knob at a target, not the attacker (same as the chip).
    if (s_active_player == src) set_active_player((src + 1) % s_player_count);
    set_view(k);
  } else {
    set_view((int)code);
  }
}

// One full-width switcher button: icon, text, outlined when `current`.
lv_obj_t *make_switch_button(lv_obj_t *parent, const lv_img_dsc_t *icon_src, uint32_t icon_color, const char *text,
                             uint32_t text_color, bool current, intptr_t code) {
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(btn, 220, 48);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(current ? 0x3F5F86 : 0x2A2A2A), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_white(), 0);
  lv_obj_set_style_border_width(btn, current ? 2 : 0, 0);
  lv_obj_set_user_data(btn, (void *)code);
  lv_obj_add_event_cb(btn, view_switch_clicked_cb, LV_EVENT_CLICKED, NULL);

  if (icon_src) {
    lv_obj_t *icon = lv_img_create(btn);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(icon, icon_src);
    lv_obj_set_style_img_recolor(icon, lv_color_hex(icon_color), 0);
    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
  }

  lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_20, text_color);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, icon_src ? 164 : 200);
  lv_obj_set_style_text_align(lbl, icon_src ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lbl, text);
  lv_obj_align(lbl, icon_src ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, icon_src ? 48 : 0, 0);
  return btn;
}

void show_view_switcher() {
  s_state = STATE_GAME;  // knob/taps stay blocked via any_overlay_open()
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *title = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, s_counter_kind_count > 0 ? "SHOW" : "OPTIONS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  if (s_battery_low) {
    lv_obj_t *batt = make_label(overlay, &lv_font_montserrat_14, 0xFF5050);
    lv_label_set_text_fmt(batt, LV_SYMBOL_BATTERY_1 " Battery %d%%", s_battery_low_pct);
    lv_obj_align(batt, LV_ALIGN_TOP_MID, 0, 14);
  }

  // Scrollable (flick or knob) - with commander damage there can be more
  // buttons than fit.
  lv_obj_t *list = make_scroll_column(overlay, 230, 254, 8);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 64);

  // With no counters shown, life is the only view - no point offering it.
  if (s_counter_kind_count > 0) {
    make_switch_button(list, &icon_life_l, LIFE_CHIP_COLOR, "Life", 0xFFFFFF, s_view_counter < 0, SWITCH_LIFE);
  }

  char buf[32];
  for (int k = 0; k < s_counter_kind_count; k++) {
    uint8_t kind = s_counter_kinds[k];
    if (kind == CK_MONARCH) continue;  // not a number - its crown chips toggle it
    if (kind == CK_COMMANDER) {
      for (int src = 0; src < s_player_count; src++) {
        char name_buf[16];
        snprintf(buf, sizeof(buf), "%s's cmdr", slot_display_name(src, name_buf, sizeof(name_buf)));
        bool current = s_view_counter == k && s_cmdr_source == src;
        make_switch_button(list, &icon_commander_l, PLAYER_COLOR[src], buf, PLAYER_COLOR[src], current,
                           SWITCH_COMMANDER | src);
      }
    } else {
      make_switch_button(list, counter_icon(kind, true), counter_color(kind),
                         counter_kind_name(kind, buf, sizeof(buf)), 0xFFFFFF, s_view_counter == k, k);
    }
  }

  make_switch_button(list, nullptr, 0, LV_SYMBOL_PLUS "  Counters...", 0xCCCCCC, false, SWITCH_PICKER);
  make_switch_button(list, nullptr, 0, LV_SYMBOL_LIST "  Menu", 0xCCCCCC, false, SWITCH_MENU);

  close_on_outside_tap(overlay, list, hide_view_switcher);
  s_view_switcher_overlay = overlay;
  s_knob_scroll_target = list;
}

// ---- counter picker overlay -------------------------------------------------
//
// Opened from the center button's "Counters..." or a long-press on the table: a
// scrollable grid of every counter kind, with the shown ones highlighted.
// Kinds are shared by all players. Tap a kind to show it (a numeric counter
// also becomes the viewed one, so it can be adjusted straight away) or tap
// a highlighted one to hide it - hiding keeps everyone's values, so showing
// it again later picks up where it left off. Changes apply live to the
// chips underneath; Done (or tapping outside the grid) closes it.

constexpr int PICKER_MAX_OPTIONS = CK_BASIC_COUNT;  // basic kinds - 1 + Commander
constexpr lv_coord_t PICKER_CELL_W = 72;
constexpr lv_coord_t PICKER_CELL_H = 62;
constexpr lv_coord_t PICKER_GAP = 6;

lv_obj_t *s_picker_cells[PICKER_MAX_OPTIONS];
int s_picker_cell_count = 0;
lv_obj_t *s_picker_status_label = nullptr;

void refresh_counter_picker() {
  for (int i = 0; i < s_picker_cell_count; i++) {
    lv_obj_t *cell = s_picker_cells[i];
    uint8_t kind = (uint8_t)(intptr_t)lv_obj_get_user_data(cell);
    bool on = find_counter_kind(kind) >= 0;
    lv_obj_set_style_bg_color(cell, lv_color_hex(on ? 0x3F5F86 : 0x2A2A2A), 0);
    lv_obj_set_style_border_width(cell, on ? 2 : 0, 0);
  }
  lv_label_set_text_fmt(s_picker_status_label, "Shared by all players  -  %d/%d", s_counter_kind_count,
                        max_shown_counters());
  lv_obj_set_style_text_color(s_picker_status_label, lv_color_hex(0xAAAAAA), 0);
}

void hide_counter_picker() {
  if (s_counter_picker_overlay) {
    lv_obj_del_async(s_counter_picker_overlay);  // see the note in hide_menu()
    s_counter_picker_overlay = nullptr;
  }
  s_picker_cell_count = 0;
  s_picker_status_label = nullptr;
  s_knob_scroll_target = nullptr;
  note_view_activity();  // don't let the idle timer yank the view right away
  s_state = STATE_GAME;
}

void counter_picker_done_cb(lv_event_t *e) {
  hide_counter_picker();
}

void counter_picker_cell_cb(lv_event_t *e) {
  lv_obj_t *cell = lv_event_get_current_target(e);
  uint8_t kind = (uint8_t)(intptr_t)lv_obj_get_user_data(cell);

  int k = find_counter_kind(kind);
  if (k >= 0) {
    // Only the kind is hidden - every player's value for it stays in
    // kind_values, so showing it again later picks up where it left off.
    for (int j = k; j < s_counter_kind_count - 1; j++) s_counter_kinds[j] = s_counter_kinds[j + 1];
    s_counter_kind_count--;
    if (s_view_counter == k) s_view_counter = -1;
    else if (s_view_counter > k) s_view_counter--;
  } else {
    if (s_counter_kind_count >= max_shown_counters()) {
      lv_label_set_text_fmt(s_picker_status_label, "Max %d - hide one first", max_shown_counters());
      lv_obj_set_style_text_color(s_picker_status_label, lv_color_hex(0xFF5050), 0);
      return;
    }
    int dst = s_counter_kind_count++;
    s_counter_kinds[dst] = kind;
    // Jump straight to a plain counter's view. Commander damage needs an
    // attacker picked first (tap their chip), and Monarch isn't a number.
    if (kind != CK_COMMANDER && kind != CK_MONARCH) s_view_counter = dst;
  }

  note_view_activity();
  for (int i = 0; i < s_player_count; i++) rebuild_counter_chips(i);
  refresh_table();
  save_game();
  refresh_counter_picker();
}

void add_picker_cell(lv_obj_t *list, uint8_t kind) {
  if (s_picker_cell_count >= PICKER_MAX_OPTIONS) return;
  lv_obj_t *cell = lv_obj_create(list);
  lv_obj_remove_style_all(cell);
  lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(cell, PICKER_CELL_W, PICKER_CELL_H);
  lv_obj_set_style_radius(cell, 10, 0);
  lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(cell, lv_color_white(), 0);
  lv_obj_set_user_data(cell, (void *)(intptr_t)kind);
  lv_obj_add_event_cb(cell, counter_picker_cell_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *icon = lv_img_create(cell);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_img_set_src(icon, counter_icon(kind, true));
  lv_obj_set_style_img_recolor(icon, lv_color_hex(counter_color(kind)), 0);
  lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t *lbl = make_label(cell, &lv_font_montserrat_14, 0xFFFFFF);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, PICKER_CELL_W - 6);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  char buf[24];
  lv_label_set_text(lbl, counter_kind_name(kind, buf, sizeof(buf)));
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

  s_picker_cells[s_picker_cell_count++] = cell;
}

void show_counter_picker() {
  s_state = STATE_GAME;  // knob/taps stay blocked via any_overlay_open()
  s_picker_cell_count = 0;
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *title = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, "COUNTERS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  s_picker_status_label = make_label(overlay, &lv_font_montserrat_14, 0xAAAAAA);
  lv_obj_align(s_picker_status_label, LV_ALIGN_TOP_MID, 0, 60);

  // Scrollable grid (flick or knob), three cells per row. Clickable so
  // drags that start between cells still scroll it - see make_scroll_column().
  lv_obj_t *list = lv_obj_create(overlay);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 3 * PICKER_CELL_W + 2 * PICKER_GAP, 186);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(list, PICKER_GAP, 0);
  lv_obj_set_style_pad_column(list, PICKER_GAP, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  // Most-used first: poison and commander damage, then the rest.
  add_picker_cell(list, CK_POISON);
  if (s_player_count > 1) add_picker_cell(list, CK_COMMANDER);
  for (uint8_t kind = CK_POISON + 1; kind < CK_BASIC_COUNT; kind++) add_picker_cell(list, kind);

  lv_obj_t *done_btn = make_menu_button(overlay, "Done", 0x2E8B3D);
  lv_obj_align(done_btn, LV_ALIGN_BOTTOM_MID, 0, -42);
  lv_obj_add_event_cb(done_btn, counter_picker_done_cb, LV_EVENT_CLICKED, NULL);

  close_on_outside_tap(overlay, list, hide_counter_picker);
  s_counter_picker_overlay = overlay;
  s_knob_scroll_target = list;
  refresh_counter_picker();
}

// ---- who goes first ------------------------------------------------------------
//
// Runs automatically when a game starts (New Game or Restart, 2+ players),
// and from Dice & Coin > "Who goes first?". The active-player highlight
// spins round the table, slowing down, and stops on a random player, who
// becomes active and gets a "Goes first!" caption. A transparent overlay
// blocks the knob and taps meanwhile; tapping it skips to the result.

constexpr int FIRST_PICK_MIN_STEPS = 10;
constexpr uint32_t FIRST_PICK_START_MS = 50;

lv_timer_t *s_pick_timer = nullptr;
int s_pick_steps_left = 0;
int s_pick_target = 0;

void finish_first_pick() {
  if (s_pick_timer) {
    lv_timer_del(s_pick_timer);
    s_pick_timer = nullptr;
  }
  if (s_first_pick_overlay) {
    lv_obj_del_async(s_first_pick_overlay);  // may be running from its own click event
    s_first_pick_overlay = nullptr;
  }
  int prev = s_active_player;
  s_active_player = s_pick_target;
  s_history_open_slot = -1;
  s_first_banner_player = s_pick_target;
  s_first_banner_ms = millis();
  if (prev != s_active_player && prev < s_player_count) refresh_player(prev);
  refresh_player(s_active_player);
  save_game();
}

void first_pick_timer_cb(lv_timer_t *timer) {
  int prev = s_active_player;
  s_active_player = (s_active_player + 1) % s_player_count;
  refresh_player(prev);
  refresh_player(s_active_player);
  if (--s_pick_steps_left <= 0) {
    finish_first_pick();
    return;
  }
  lv_timer_set_period(timer, timer->period * 115 / 100);  // slow down each step
}

void first_pick_skip_cb(lv_event_t *e) {
  finish_first_pick();
}

void start_first_player_pick() {
  if (s_player_count < 2 || !s_scr_game || s_first_pick_overlay) return;
  s_pick_target = (int)(esp_random() % (uint32_t)s_player_count);
  // At least FIRST_PICK_MIN_STEPS hops (a couple of laps), landing on the target.
  int steps = (s_pick_target - s_active_player + s_player_count) % s_player_count;
  while (steps < FIRST_PICK_MIN_STEPS) steps += s_player_count;
  s_pick_steps_left = steps;

  s_first_pick_overlay = make_overlay(LV_OPA_TRANSP);
  lv_obj_add_event_cb(s_first_pick_overlay, first_pick_skip_cb, LV_EVENT_CLICKED, NULL);
  s_pick_timer = lv_timer_create(first_pick_timer_cb, FIRST_PICK_START_MS, NULL);
}

// ---- dice & coin overlay --------------------------------------------------------
//
// From the menu: roll a d6 or d20 or flip a coin (the result flickers
// briefly before settling), turn the knob to roll the same thing again, or
// re-run the "who goes first" pick.

lv_obj_t *s_dice_result = nullptr;
lv_obj_t *s_dice_caption = nullptr;
lv_timer_t *s_dice_timer = nullptr;
int s_dice_sides = 20;  // 0 = coin
int s_dice_ticks = 0;
constexpr int DICE_FLICKER_TICKS = 10;
constexpr uint32_t DICE_FLICKER_MS = 60;

void dice_show_random(bool final) {
  if (!s_dice_result) return;
  if (s_dice_sides == 0) {
    lv_label_set_text(s_dice_result, (esp_random() & 1) ? "Heads" : "Tails");
  } else {
    lv_label_set_text_fmt(s_dice_result, "%d", (int)(esp_random() % (uint32_t)s_dice_sides) + 1);
  }
  lv_obj_set_style_text_color(s_dice_result, lv_color_hex(final ? 0xFFFFFF : 0x707070), 0);
}

void dice_timer_cb(lv_timer_t *timer) {
  bool final = --s_dice_ticks <= 0;
  dice_show_random(final);
  if (final) {
    lv_timer_del(timer);
    s_dice_timer = nullptr;
  }
}

void start_dice_roll(int sides) {
  s_dice_sides = sides;
  if (s_dice_caption) {
    if (sides == 0) {
      lv_label_set_text(s_dice_caption, "Coin flip");
    } else {
      lv_label_set_text_fmt(s_dice_caption, "d%d", sides);
    }
  }
  s_dice_ticks = DICE_FLICKER_TICKS;
  if (!s_dice_timer) s_dice_timer = lv_timer_create(dice_timer_cb, DICE_FLICKER_MS, NULL);
}

void hide_dice() {
  if (s_dice_timer) {
    lv_timer_del(s_dice_timer);
    s_dice_timer = nullptr;
  }
  if (s_dice_overlay) {
    lv_obj_del_async(s_dice_overlay);  // see the note in hide_menu()
    s_dice_overlay = nullptr;
  }
  s_dice_result = nullptr;
  s_dice_caption = nullptr;
  s_state = STATE_GAME;
}

void dice_button_cb(lv_event_t *e) {
  start_dice_roll((int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)));
}

void dice_first_player_cb(lv_event_t *e) {
  hide_dice();
  start_first_player_pick();
}

void dice_back_cb(lv_event_t *e) {
  hide_dice();
  show_menu();
}

lv_obj_t *make_dice_button(lv_obj_t *parent, const char *text, int sides, lv_coord_t x_off) {
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(btn, 68, 46);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x3F5F86), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_align(btn, LV_ALIGN_CENTER, x_off, 40);
  lv_obj_set_user_data(btn, (void *)(intptr_t)sides);
  lv_obj_add_event_cb(btn, dice_button_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return btn;
}

void show_dice() {
  s_state = STATE_GAME;  // knob/taps on the table stay blocked via any_overlay_open()
  lv_obj_t *overlay = make_overlay(LV_OPA_90);

  lv_obj_t *title = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(title, "DICE & COIN");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  s_dice_result = make_label(overlay, &lv_font_montserrat_48, 0xFFFFFF);
  lv_label_set_text(s_dice_result, "-");
  lv_obj_align(s_dice_result, LV_ALIGN_CENTER, 0, -46);

  s_dice_caption = make_label(overlay, &lv_font_montserrat_14, 0x999999);
  lv_label_set_text(s_dice_caption, "Tap to roll - turn the knob to roll again");
  lv_obj_align(s_dice_caption, LV_ALIGN_CENTER, 0, -6);

  make_dice_button(overlay, "d6", 6, -76);
  make_dice_button(overlay, "d20", 20, 0);
  make_dice_button(overlay, "Coin", 0, 76);

  lv_obj_t *first_btn = make_menu_button(overlay, "Who goes first?", 0x7A5A1E);
  lv_obj_set_width(first_btn, 200);
  lv_obj_align(first_btn, LV_ALIGN_CENTER, 0, 92);
  lv_obj_add_event_cb(first_btn, dice_first_player_cb, LV_EVENT_CLICKED, NULL);
  if (s_player_count < 2) lv_obj_add_flag(first_btn, LV_OBJ_FLAG_HIDDEN);

  make_back_button(overlay, dice_back_cb);

  close_on_outside_tap(overlay, nullptr, hide_dice);
  s_dice_overlay = overlay;
}

// ---- knob -------------------------------------------------------------

void knob_left_cb(void *, void *) {
  s_knob_delta.fetch_sub(1, std::memory_order_relaxed);
}

void knob_right_cb(void *, void *) {
  s_knob_delta.fetch_add(1, std::memory_order_relaxed);
}

// Requires this many consecutive pulses in the same direction before they
// start changing a value. Damps small/accidental nudges (e.g. brushing the
// knob) without slowing down a deliberate spin: the streak only resets on a
// direction change, so once engaged every further pulse in that direction
// still applies immediately.
constexpr int KNOB_ENGAGE_PULSES = 2;

// Acceleration for life totals: once KNOB_FAST_STREAK pulses in a row have
// each arrived within KNOB_FAST_GAP_MS of the last, each further pulse
// moves life by KNOB_FAST_STEP instead of 1. A slow, careful turn always
// stays at 1. Counters (poison etc.) never accelerate - they're small.
constexpr unsigned long KNOB_FAST_GAP_MS = 50;
constexpr int KNOB_FAST_STREAK = 4;
constexpr int KNOB_FAST_STEP = 5;

// How far one knob pulse scrolls an open overlay's list.
constexpr lv_coord_t KNOB_SCROLL_PX = 36;

int s_knob_streak_dir = 0;
int s_knob_streak_len = 0;
unsigned long s_knob_last_pulse_ms = 0;
int s_knob_fast_streak = 0;

bool knob_pulse_engaged(int dir) {
  if (dir != s_knob_streak_dir) {
    s_knob_streak_dir = dir;
    s_knob_streak_len = 0;
    s_knob_fast_streak = 0;
  }
  s_knob_streak_len++;
  return s_knob_streak_len >= KNOB_ENGAGE_PULSES;
}

// 1, or KNOB_FAST_STEP while the knob is being spun quickly.
int knob_life_step() {
  unsigned long now = millis();
  s_knob_fast_streak = (now - s_knob_last_pulse_ms < KNOB_FAST_GAP_MS) ? s_knob_fast_streak + 1 : 0;
  s_knob_last_pulse_ms = now;
  return s_knob_fast_streak >= KNOB_FAST_STREAK ? KNOB_FAST_STEP : 1;
}

void apply_knob_step(int dir) {
  if (s_shutting_down) return;
  int life_step = knob_life_step();
  switch (s_state) {
    case STATE_SELECT_PLAYERS:
      s_player_count = clampi(s_player_count + dir, 1, MAX_PLAYERS);
      refresh_players_screen();
      break;
    case STATE_ASSIGN_PROFILES:
      s_assign_option = clampi(s_assign_option + dir, 0, assign_option_total() - 1);
      refresh_assign_screen();
      break;
    case STATE_SELECT_LIFE:
      s_starting_life = clampi(s_starting_life + dir * life_step, MIN_LIFE, MAX_LIFE);
      refresh_life_screen();
      break;
    case STATE_GAME:
      if (any_overlay_open()) {
        if (s_dice_overlay) {
          start_dice_roll(s_dice_sides);
          break;
        }
        if (s_knob_scroll_target) lv_obj_scroll_by_bounded(s_knob_scroll_target, 0, -dir * KNOB_SCROLL_PX, LV_ANIM_OFF);
        break;
      }
      adjust_player_value(s_active_player, s_view_counter < 0 ? dir * life_step : dir);
      break;
  }
}


}  // namespace

void app_ui_init(void) {
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);  // full 0-3.3V input range
  lv_timer_create(critical_battery_timer_cb, BATTERY_CRITICAL_CHECK_MS, NULL);
  lv_timer_create(ui_tick_timer_cb, 500, NULL);
  lv_timer_create(dim_timer_cb, 250, NULL);
  lv_timer_create(battery_low_timer_cb, BATTERY_LOW_CHECK_MS, NULL);
  battery_low_timer_cb(nullptr);  // know straight away, not 20s in

  load_roster();

  knob_config_t cfg = {
      .gpio_encoder_a = EXAMPLE_PIN_NUM_ENCODER_A,
      .gpio_encoder_b = EXAMPLE_PIN_NUM_ENCODER_B,
  };
  knob_handle_t knob = iot_knob_create(&cfg);
  if (knob) {
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL);
  }

  SavedGame saved;
  if (load_saved_game(&saved)) {
    s_player_count = saved.player_count;
    s_starting_life = saved.starting_life;
    s_active_player = clampi(saved.active_player, 0, s_player_count - 1);
    for (int i = 0; i < s_player_count; i++) {
      s_players[i].life = saved.life[i];
      // A profile referenced by a saved game may have since been deleted
      // (e.g. from the phone app while the board was off) - fall back to
      // Guest rather than dangling.
      int pid = saved.profile_id[i];
      s_player_profile_id[i] = (pid > 0 && find_profile((uint8_t)pid)) ? pid : -1;
    }

    // Shown kinds: fold old per-source commander kinds into the single
    // Commander counter, and drop empty/unknown/duplicate kinds (or any
    // beyond what this player count shows) rather than trusting flash.
    // Values are kept regardless - they live in kind_values.
    s_counter_kind_count = 0;
    for (int k = 0; k < MAX_COUNTERS; k++) {
      uint8_t kind = saved.counter_kinds[k];
      if (is_commander_source_kind(kind)) kind = CK_COMMANDER;
      if (!counter_kind_valid(kind, s_player_count) || find_counter_kind(kind) >= 0) continue;
      if (s_counter_kind_count >= max_shown_counters()) break;
      s_counter_kinds[s_counter_kind_count++] = kind;
    }
    for (int i = 0; i < s_player_count; i++) {
      for (int v = 0; v < NUM_VALUE_SLOTS; v++) {
        s_players[i].kind_values[v] = (int16_t)clampi(saved.kind_values[i][v], 0, COUNTER_MAX_VALUE);
      }
    }
    show_game_screen(false);  // resume as-is, don't reset life/active player
  } else {
    show_players_screen();
  }

  // Started last, after the roster and any saved game are loaded, so its
  // very first characteristic snapshot already reflects real state instead
  // of the pre-load defaults - see the precondition noted in ble_sync.h.
  ble_sync_init("MTGCounter");
}

void app_ui_tick(void) {
  int delta = s_knob_delta.exchange(0, std::memory_order_relaxed);
  if (delta == 0) return;

  if (!lvgl_port_lock(50)) return;
  // The knob isn't an LVGL input device, so report its activity by hand
  // (keeps the screen from dimming), and let a turn that wakes a dimmed
  // screen do only that.
  lv_disp_trig_activity(NULL);
  if (s_dimmed) {
    wake_display();
    lvgl_port_unlock();
    return;
  }
  while (delta > 0) {
    if (knob_pulse_engaged(1)) apply_knob_step(1);
    delta--;
  }
  while (delta < 0) {
    if (knob_pulse_engaged(-1)) apply_knob_step(-1);
    delta++;
  }
  lvgl_port_unlock();
}

// ---- BLE sync surface (see app_ui.h) --------------------------------------
//
// Defined outside the anonymous namespace above but in the same translation
// unit, so these can still call the (internally-linked) helpers/globals
// declared inside it - the same trick app_ui_init()/app_ui_tick() already
// rely on. Every one of these takes lvgl_port_lock() since, unlike
// app_ui_tick(), they're called from ble_sync.cpp - i.e. from NimBLE's own
// FreeRTOS task, not the Arduino loop() task.

int app_ui_serialize_roster(uint8_t *out, int out_cap) {
  if (!lvgl_port_lock(50)) return 0;
  int written = 0;
  for (int i = 0; i < MAX_PROFILES; i++) {
    if (!s_roster.profiles[i].in_use) continue;
    const PlayerProfile &p = s_roster.profiles[i];
    int name_len = (int)strlen(p.name);
    int record_len = (int)sizeof(BleProfileRecordHeader) + name_len;
    if (written + record_len > out_cap) break;  // doesn't fit - drop, don't split

    BleProfileRecordHeader hdr;
    hdr.id = p.id;
    hdr.flags = 0;
    hdr.games_played = p.games_played;
    hdr.wins = p.wins;
    hdr.total_life_delta = p.total_life_delta;
    hdr.name_len = (uint8_t)name_len;
    memcpy(out + written, &hdr, sizeof(hdr));
    memcpy(out + written + sizeof(hdr), p.name, name_len);
    written += record_len;
  }
  lvgl_port_unlock();
  return written;
}

bool app_ui_apply_profile_write(const uint8_t *data, int len) {
  constexpr uint8_t FLAG_DELETE = 0x01;
  if (len < 3) return false;
  uint8_t id = data[0];
  uint8_t flags = data[1];
  uint8_t name_len = data[2];
  if (name_len > PROFILE_NAME_MAX || len < 3 + name_len) return false;

  char name[PROFILE_NAME_MAX + 1];
  memcpy(name, data + 3, name_len);
  name[name_len] = '\0';

  if (!lvgl_port_lock(50)) return false;
  bool ok;
  if (flags & FLAG_DELETE) {
    ok = id != 0 && find_profile(id) != nullptr;
    if (ok) delete_profile(id);
  } else if (id == 0) {
    ok = create_profile(name) != 0;
  } else {
    PlayerProfile *p = find_profile(id);
    ok = p != nullptr;
    if (ok) {
      strncpy(p->name, name, PROFILE_NAME_MAX);
      p->name[PROFILE_NAME_MAX] = '\0';
      save_roster();
    }
  }
  lvgl_port_unlock();
  return ok;
}

void app_ui_get_game_state(BleGameState *out) {
  if (!lvgl_port_lock(50)) {
    memset(out, 0, sizeof(*out));
    return;
  }
  out->player_count = (uint8_t)s_player_count;
  out->active_player = (uint8_t)s_active_player;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    out->life[i] = (i < s_player_count) ? s_players[i].life : 0;
    int pid = (i < s_player_count) ? s_player_profile_id[i] : -1;
    out->profile_id[i] = (pid > 0) ? (uint8_t)pid : 0;
  }
  lvgl_port_unlock();
}

void app_ui_get_counter_state(BleCounterState *out) {
  static_assert(NUM_VALUE_SLOTS == BLE_COUNTER_VALUE_SLOTS, "BleCounterState layout out of sync");
  static_assert(MAX_COUNTERS == 4 && MAX_PLAYERS == 4, "BleCounterState layout out of sync");
  memset(out, 0, sizeof(*out));
  out->version = BLE_COUNTER_STATE_VERSION;
  out->view = 0xFF;
  out->monarch = 0xFF;
  if (!lvgl_port_lock(50)) return;
  if (s_state == STATE_GAME) {
    out->player_count = (uint8_t)s_player_count;
    out->shown_count = (uint8_t)s_counter_kind_count;
    for (int k = 0; k < s_counter_kind_count; k++) out->shown_kinds[k] = s_counter_kinds[k];
    if (s_view_counter >= 0) out->view = (uint8_t)s_view_counter;
    out->cmdr_source = (uint8_t)s_cmdr_source;
    int monarch = monarch_player();
    if (monarch >= 0) out->monarch = (uint8_t)monarch;
    for (int i = 0; i < s_player_count; i++) {
      memcpy(out->values[i], s_players[i].kind_values, sizeof(out->values[i]));
    }
  }
  lvgl_port_unlock();
}

// The wire format only knows life changes, so counter entries are left out;
// commander damage is sent as the life loss it caused.
int app_ui_serialize_history(uint8_t *out, int out_cap) {
  if (!lvgl_port_lock(50)) return 0;
  int written = 0;
  for (int i = 0; i < s_history_count; i++) {
    if (written + (int)sizeof(BleHistoryEntryWire) > out_cap) break;
    int idx = (s_history_next - 1 - i + HISTORY_CAPACITY) % HISTORY_CAPACITY;
    const HistoryEntry &entry = s_history[idx];
    BleHistoryEntryWire w;
    w.player = (uint8_t)entry.player;
    if (entry.kind == CK_NONE) {
      w.delta = entry.delta;
    } else if (is_commander_source_kind(entry.kind)) {
      w.delta = (int16_t)-entry.delta;
    } else {
      continue;
    }
    w.life_after = entry.life_after;
    memcpy(out + written, &w, sizeof(w));
    written += sizeof(w);
  }
  lvgl_port_unlock();
  return written;
}
