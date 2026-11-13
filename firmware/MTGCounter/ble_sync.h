#ifndef BLE_SYNC_H
#define BLE_SYNC_H

// GATT server exposing the player-profile roster, a live game-state
// snapshot, and this game's history to the MTGCounter Android companion
// app. See ble_sync.cpp for the wire format and firmware/README.md for the
// full protocol writeup - this header only exposes the lifecycle hooks
// app_ui.cpp calls into.

// Starts advertising and the GATT server under `device_name`. Call once
// from app_ui_init(), after the roster/saved game have been loaded from NVS
// (see load_roster()/load_saved_game() in app_ui.cpp), so the very first
// client read can't race an empty roster.
void ble_sync_init(const char *device_name);

// Call after any roster mutation (profile create/rename/delete, or a game
// ending and updating games_played/wins) so connected phones get a fresh
// Profile Roster notify. A no-op if nothing is connected.
void ble_sync_notify_profiles_changed();

// Call after any change to the live game (life total, active player,
// profile assignment, counters / counter view, or a new history entry).
// Refreshes and notifies Game State and Counters together. Internally debounced
// (~150ms via an LVGL one-shot timer) so a fast knob spin doesn't flood the
// link with one notification per detent. A no-op if nothing is connected.
void ble_sync_notify_game_state_changed();

#endif
