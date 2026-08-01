// Life-counter application built on top of the board bring-up in lcd_bsp.cpp.
//
// Flow:
//   1. STATE_SELECT_PLAYERS - rotate the knob to pick 1-4 players, tap to confirm.
//   2. STATE_SELECT_LIFE    - rotate the knob to pick a starting total (1-100,
//      defaulted per player count), tap to confirm.
//   3. STATE_GAME           - one shape per player (rectangles for 1/2/4 players,
//      pie wedges for 3), each with its life total rotated to face outward from
//      the center. Tapping a shape only selects it as "active" (highlighted);
//      the knob is the only thing that changes a total, and it always acts on
//      whichever shape is active. A center RESET button asks for confirmation
//      and returns to step 1.
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
#include <stdint.h>
#include "lcd_bsp.h"
#include "lcd_config.h"
#include "bidi_switch_knob.h"
#include "app_ui.h"

namespace {

enum AppState {
  STATE_SELECT_PLAYERS,
  STATE_SELECT_LIFE,
  STATE_GAME,
};

constexpr int MAX_PLAYERS = 4;
constexpr int MIN_LIFE = 1;
constexpr int MAX_LIFE = 100;
constexpr float RAD2DEG = 57.29577951308232f;
constexpr uint32_t PLAYER_COLOR[MAX_PLAYERS] = {0xE9503F, 0x4F8FE9, 0x53D07A, 0xE9C23F};

struct PlayerShape {
  lv_obj_t *shape = nullptr;  // lv_obj rectangle (1/2/4 players) or lv_arc wedge (3 players)
  lv_obj_t *value_label = nullptr;
  int life = 0;
};

AppState s_state = STATE_SELECT_PLAYERS;
std::atomic<int> s_knob_delta{0};

int s_player_count = 2;
int s_starting_life = 20;
int s_active_player = 0;
PlayerShape s_players[MAX_PLAYERS];

lv_obj_t *s_scr_players = nullptr;
lv_obj_t *s_players_value_label = nullptr;
lv_obj_t *s_players_dots[MAX_PLAYERS];

lv_obj_t *s_scr_life = nullptr;
lv_obj_t *s_life_value_label = nullptr;

lv_obj_t *s_scr_game = nullptr;
lv_obj_t *s_reset_overlay = nullptr;

void show_players_screen();
void show_life_screen();
void show_game_screen();

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

// ---- STATE_SELECT_PLAYERS -------------------------------------------------

void refresh_players_screen() {
  lv_label_set_text_fmt(s_players_value_label, "%d", s_player_count);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    bool on = i < s_player_count;
    lv_obj_set_style_bg_color(s_players_dots[i], lv_color_hex(on ? 0x4F8FE9 : 0x303030), 0);
    lv_obj_set_style_bg_opa(s_players_dots[i], LV_OPA_COVER, 0);
  }
}

void players_screen_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_SELECT_PLAYERS) return;
  show_life_screen();
}

void show_players_screen() {
  s_state = STATE_SELECT_PLAYERS;
  lv_obj_t *scr = make_screen();

  lv_obj_t *title = make_label(scr, &lv_font_montserrat_20, 0xAAAAAA);
  lv_label_set_text(title, "PLAYERS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 78);

  s_players_value_label = make_label(scr, &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_align(s_players_value_label, LV_ALIGN_CENTER, 0, -10);

  int dot_span = 30 * (MAX_PLAYERS - 1);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    lv_obj_t *dot = make_inert(scr);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, -dot_span / 2 + i * 30, 50);
    s_players_dots[i] = dot;
  }

  lv_obj_t *hint = make_label(scr, &lv_font_montserrat_14, 0x808080);
  lv_label_set_text(hint, "Rotate to choose - tap to continue");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -70);

  lv_obj_add_event_cb(scr, players_screen_clicked_cb, LV_EVENT_CLICKED, NULL);

  refresh_players_screen();

  if (s_scr_players) lv_obj_del(s_scr_players);
  s_scr_players = scr;
  lv_scr_load(s_scr_players);
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
  if (s_state != STATE_SELECT_LIFE) return;
  show_game_screen();
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
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -70);

  lv_obj_add_event_cb(scr, life_screen_clicked_cb, LV_EVENT_CLICKED, NULL);

  refresh_life_screen();

  if (s_scr_life) lv_obj_del(s_scr_life);
  s_scr_life = scr;
  lv_scr_load(s_scr_life);
}

// ---- STATE_GAME: layout ---------------------------------------------------
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
  return n == 1 ? &lv_font_montserrat_48 : &lv_font_montserrat_32;
}

// Builds the (rotated) name + value label pair for one player, parented
// directly to `parent`, centered at align offset (dx, dy) with the given
// rotation.
void make_player_content(lv_obj_t *parent, int idx, lv_coord_t dx, lv_coord_t dy,
                          lv_coord_t w, lv_coord_t h, int rotation_deg, const lv_font_t *value_font) {
  lv_obj_t *content = make_inert(parent);
  lv_obj_set_size(content, w, h);
  lv_obj_align(content, LV_ALIGN_CENTER, dx, dy);
  lv_obj_set_style_transform_pivot_x(content, w / 2, 0);
  lv_obj_set_style_transform_pivot_y(content, h / 2, 0);
  lv_obj_set_style_transform_angle(content, rotation_deg * 10, 0);

  lv_obj_t *name = make_label(content, &lv_font_montserrat_14, 0xCCCCCC);
  lv_label_set_text_fmt(name, "P%d", idx + 1);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *value = make_label(content, value_font, 0xFFFFFF);
  lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, 0);
  s_players[idx].value_label = value;
}

// ---- STATE_GAME: life totals + active-player highlight ---------------------

void update_life_label(int idx) {
  lv_label_set_text_fmt(s_players[idx].value_label, "%d", s_players[idx].life);
}

void refresh_active_visual() {
  for (int i = 0; i < s_player_count; i++) {
    bool active = (i == s_active_player);
    lv_obj_set_style_arc_opa(s_players[i].shape, active ? LV_OPA_COVER : LV_OPA_40, LV_PART_INDICATOR);
  }
}

// Which player's shape contains screen point (x, y)? Mirrors the geometry
// used to build the shapes above.
int player_at_point(lv_coord_t x, lv_coord_t y) {
  if (s_player_count == 1) return 0;

  // Bearing measured clockwise from straight down, matching wedge_geometry()
  // (slot 0 = bottom, slot i is `i * step` degrees clockwise from there).
  float dx = (float)(x - 180);
  float dy = (float)(y - 180);
  float bearing = atan2f(-dx, dy) * RAD2DEG;
  if (bearing < 0) bearing += 360.0f;
  float step = 360.0f / s_player_count;
  int idx = ((int)floorf((bearing + step / 2.0f) / step)) % s_player_count;
  if (idx < 0) idx += s_player_count;
  return idx;
}

void show_reset_confirm();

void reset_button_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_GAME) return;
  show_reset_confirm();
}

void game_screen_clicked_cb(lv_event_t *e) {
  if (s_state != STATE_GAME || s_reset_overlay) return;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  int idx = player_at_point(p.x, p.y);
  if (idx < 0 || idx >= s_player_count) return;
  s_active_player = idx;
  refresh_active_visual();
}

void show_game_screen() {
  s_state = STATE_GAME;
  lv_obj_t *scr = make_screen();
  const lv_font_t *value_font = value_font_for_count(s_player_count);

  lv_coord_t content_w, content_h;
  wedge_content_size(s_player_count, &content_w, &content_h);

  for (int i = 0; i < s_player_count; i++) {
    int start_angle, rotation;
    lv_coord_t dx, dy;
    wedge_geometry(s_player_count, i, &start_angle, &rotation, &dx, &dy);
    int end_angle = (s_player_count == 1) ? 360 : start_angle + 360 / s_player_count;

    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_remove_style_all(arc);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, 360, 360);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(arc, start_angle, end_angle);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 100);  // indicator = full bg range = the whole wedge

    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 180, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(PLAYER_COLOR[i]), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_40, LV_PART_INDICATOR);

    s_players[i].shape = arc;
    s_players[i].life = s_starting_life;

    make_player_content(scr, i, dx, dy, content_w, content_h, rotation, value_font);
    update_life_label(i);
  }

  s_active_player = 0;
  refresh_active_visual();

  lv_obj_add_event_cb(scr, game_screen_clicked_cb, LV_EVENT_CLICKED, NULL);

  // Center reset button, drawn on top of everything since it's created last.
  lv_obj_t *reset_btn = lv_obj_create(scr);
  lv_obj_remove_style_all(reset_btn);
  lv_obj_set_size(reset_btn, 44, 44);
  lv_obj_set_style_radius(reset_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(reset_btn, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(reset_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(reset_btn, lv_color_hex(0x808080), 0);
  lv_obj_set_style_border_width(reset_btn, 1, 0);
  lv_obj_align(reset_btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(reset_btn, reset_button_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *reset_lbl = make_label(reset_btn, &lv_font_montserrat_20, 0xCCCCCC);
  lv_label_set_text(reset_lbl, LV_SYMBOL_REFRESH);
  lv_obj_center(reset_lbl);

  if (s_scr_game) lv_obj_del(s_scr_game);
  s_scr_game = scr;
  lv_scr_load(s_scr_game);
}

// ---- reset confirmation overlay -------------------------------------------

void hide_reset_confirm() {
  if (s_reset_overlay) {
    lv_obj_del(s_reset_overlay);
    s_reset_overlay = nullptr;
  }
  s_state = STATE_GAME;
}

void reset_confirm_yes_cb(lv_event_t *e) {
  hide_reset_confirm();
  show_players_screen();
  if (s_scr_game) {
    lv_obj_del(s_scr_game);
    s_scr_game = nullptr;
  }
}

void reset_confirm_no_cb(lv_event_t *e) {
  hide_reset_confirm();
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

void show_reset_confirm() {
  s_state = STATE_GAME;  // knob still adjusts life while this is open
  lv_obj_t *overlay = lv_obj_create(s_scr_game);
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, 360, 360);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  // Left clickable (default for lv_obj) so it eats taps outside Yes/No
  // instead of them falling through to game_screen_clicked_cb underneath.

  lv_obj_t *msg = make_label(overlay, &lv_font_montserrat_20, 0xFFFFFF);
  lv_label_set_text(msg, "New game?");
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t *yes = make_confirm_button(overlay, "Yes", 0x2E8B3D, -50);
  lv_obj_add_event_cb(yes, reset_confirm_yes_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *no = make_confirm_button(overlay, "No", 0x8B2E2E, 50);
  lv_obj_add_event_cb(no, reset_confirm_no_cb, LV_EVENT_CLICKED, NULL);

  s_reset_overlay = overlay;
}

// ---- knob -------------------------------------------------------------

void knob_left_cb(void *, void *) {
  s_knob_delta.fetch_sub(1, std::memory_order_relaxed);
}

void knob_right_cb(void *, void *) {
  s_knob_delta.fetch_add(1, std::memory_order_relaxed);
}

void apply_knob_step(int dir) {
  switch (s_state) {
    case STATE_SELECT_PLAYERS:
      s_player_count = clampi(s_player_count + dir, 1, MAX_PLAYERS);
      refresh_players_screen();
      break;
    case STATE_SELECT_LIFE:
      s_starting_life = clampi(s_starting_life + dir, MIN_LIFE, MAX_LIFE);
      refresh_life_screen();
      break;
    case STATE_GAME:
      if (s_reset_overlay) break;  // ignore knob while the confirm dialog is up
      s_players[s_active_player].life += dir;
      update_life_label(s_active_player);
      break;
  }
}

}  // namespace

void app_ui_init(void) {
  knob_config_t cfg = {
      .gpio_encoder_a = EXAMPLE_PIN_NUM_ENCODER_A,
      .gpio_encoder_b = EXAMPLE_PIN_NUM_ENCODER_B,
  };
  knob_handle_t knob = iot_knob_create(&cfg);
  if (knob) {
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL);
  }

  show_players_screen();
}

void app_ui_tick(void) {
  int delta = s_knob_delta.exchange(0, std::memory_order_relaxed);
  if (delta == 0) return;

  if (!lvgl_port_lock(50)) return;
  while (delta > 0) { apply_knob_step(1); delta--; }
  while (delta < 0) { apply_knob_step(-1); delta++; }
  lvgl_port_unlock();
}
