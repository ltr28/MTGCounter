// BLE GATT server for the MTGCounter Android companion app - see
// ble_sync.h. Kept as a thin transport layer, deliberately separate from
// app_ui.cpp's state: it only talks to app state through the small extern
// surface in app_ui.h (app_ui_serialize_roster / app_ui_apply_profile_write
// / app_ui_get_game_state / app_ui_get_counter_state /
// app_ui_serialize_history), each of which is
// already thread-safe against the LVGL/Arduino loop task.
//
// Library: NimBLE-Arduino, not the stock Bluedroid "ESP32 BLE Arduino"
// stack - roughly a third the RAM/flash footprint and BLE-only, which
// matters since LVGL is already using a meaningful chunk of the S3's heap.
//
// Wire format: a custom GATT service, no JSON/CBOR - see the UUID table
// below. Reads rely on the ATT "read blob" mechanism (built into both
// NimBLE's and Android's BLE stacks) to transparently fetch values longer
// than one MTU packet, so the Profile Roster/History characteristics are
// just set to the full serialized blob and read normally - no hand-rolled
// chunking protocol needed for reads. Writes (profile create/rename/delete
// from the phone) are small (at most 3 + PROFILE_NAME_MAX = 18 bytes), well
// under the ~247-byte MTU requested on connect (see ServerCallbacks below),
// so they fit in a single ATT Write Request without needing one either.
//
// A write's result (e.g. the id a new profile was assigned) isn't echoed
// back in the write response - plain ATT writes don't carry a return
// payload. Instead, a successful write immediately refreshes and notifies
// the Profile Roster characteristic, the same as any other roster change,
// and the phone picks up the result from that.
#include <NimBLEDevice.h>
#include "ble_sync.h"
#include "app_ui.h"
#include "lcd_bsp.h"  // lvgl.h, for the game-state notify debounce timer

namespace {

constexpr char SERVICE_UUID[] = "8f6a2c00-2b1a-4f6e-9b0a-6f1d2a3c4e00";
constexpr char CHR_PROFILE_ROSTER_UUID[] = "8f6a2c01-2b1a-4f6e-9b0a-6f1d2a3c4e00";
constexpr char CHR_PROFILE_WRITE_UUID[] = "8f6a2c02-2b1a-4f6e-9b0a-6f1d2a3c4e00";
constexpr char CHR_GAME_STATE_UUID[] = "8f6a2c03-2b1a-4f6e-9b0a-6f1d2a3c4e00";
constexpr char CHR_HISTORY_UUID[] = "8f6a2c04-2b1a-4f6e-9b0a-6f1d2a3c4e00";
// Extra counters (BleCounterState in app_ui.h) - read + notify, refreshed
// alongside Game State. A separate characteristic so Game State's layout
// never changes for phones that only know life totals.
constexpr char CHR_COUNTERS_UUID[] = "8f6a2c05-2b1a-4f6e-9b0a-6f1d2a3c4e00";

// NimBLE's default max attribute value length (BLE_ATT_ATTR_MAX_LEN) is 512
// bytes - comfortably more than 16 profiles * (12-byte header + up to 15
// name bytes) = 432 bytes worst case, but app_ui_serialize_roster() and
// app_ui_serialize_history() both drop-rather-than-split a record that
// wouldn't fit, so this cap is safe even if that ever changes.
constexpr int SERIALIZE_BUF_SIZE = 512;
constexpr uint32_t GAME_STATE_NOTIFY_DEBOUNCE_MS = 150;

NimBLEServer *s_server = nullptr;
NimBLECharacteristic *s_chr_roster = nullptr;
NimBLECharacteristic *s_chr_game_state = nullptr;
NimBLECharacteristic *s_chr_history = nullptr;
NimBLECharacteristic *s_chr_counters = nullptr;
lv_timer_t *s_game_state_debounce_timer = nullptr;

void refresh_roster_characteristic() {
  static uint8_t buf[SERIALIZE_BUF_SIZE];
  int len = app_ui_serialize_roster(buf, sizeof(buf));
  s_chr_roster->setValue(buf, len);
}

void refresh_history_characteristic() {
  static uint8_t buf[SERIALIZE_BUF_SIZE];
  int len = app_ui_serialize_history(buf, sizeof(buf));
  s_chr_history->setValue(buf, len);
}

void refresh_counters_characteristic() {
  BleCounterState state;
  app_ui_get_counter_state(&state);
  s_chr_counters->setValue(reinterpret_cast<uint8_t *>(&state), sizeof(state));
}

void refresh_game_state_characteristic() {
  BleGameState state;
  app_ui_get_game_state(&state);
  s_chr_game_state->setValue(reinterpret_cast<uint8_t *>(&state), sizeof(state));
}

// Requests a larger MTU (see the file comment above) and re-arms
// advertising once a client disconnects, so the next phone can find it.
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    server->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    NimBLEDevice::startAdvertising();
  }
};

class ProfileWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) override {
    const NimBLEAttValue &value = chr->getValue();
    if (app_ui_apply_profile_write(value.data(), value.size())) {
      refresh_roster_characteristic();
      s_chr_roster->notify();
    }
  }
};

// One-shot fire after the debounce window (see ble_sync_notify_game_state_changed()).
void game_state_debounce_cb(lv_timer_t *timer) {
  s_game_state_debounce_timer = nullptr;
  refresh_game_state_characteristic();
  refresh_history_characteristic();
  refresh_counters_characteristic();
  s_chr_game_state->notify();
  s_chr_counters->notify();
}

ServerCallbacks s_server_callbacks;
ProfileWriteCallbacks s_profile_write_callbacks;

}  // namespace

void ble_sync_init(const char *device_name) {
  NimBLEDevice::init(device_name);
  // Just Works bonding: this board has a knob and a touchscreen but no way
  // to accept a typed passkey, and a knob-only numeric-comparison UX isn't
  // worth building for a life counter with no sensitive data. Bonding (the
  // first `true`) means a phone doesn't need to re-pair every session.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*MITM=*/false, /*secure connections=*/true);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(&s_server_callbacks);

  NimBLEService *service = s_server->createService(SERVICE_UUID);

  s_chr_roster =
      service->createCharacteristic(CHR_PROFILE_ROSTER_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic *chr_write = service->createCharacteristic(CHR_PROFILE_WRITE_UUID, NIMBLE_PROPERTY::WRITE);
  chr_write->setCallbacks(&s_profile_write_callbacks);

  s_chr_game_state =
      service->createCharacteristic(CHR_GAME_STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_chr_history = service->createCharacteristic(CHR_HISTORY_UUID, NIMBLE_PROPERTY::READ);
  s_chr_counters =
      service->createCharacteristic(CHR_COUNTERS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  refresh_roster_characteristic();
  refresh_game_state_characteristic();
  refresh_history_characteristic();
  refresh_counters_characteristic();

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setName(device_name);
  advertising->addServiceUUID(service->getUUID());
  advertising->enableScanResponse(true);
  advertising->start();
}

void ble_sync_notify_profiles_changed() {
  if (!s_server || s_server->getConnectedCount() == 0) return;
  refresh_roster_characteristic();
  s_chr_roster->notify();
}

void ble_sync_notify_game_state_changed() {
  if (!s_server || s_server->getConnectedCount() == 0) return;
  if (s_game_state_debounce_timer) {
    lv_timer_reset(s_game_state_debounce_timer);
    return;
  }
  s_game_state_debounce_timer = lv_timer_create(game_state_debounce_cb, GAME_STATE_NOTIFY_DEBOUNCE_MS, NULL);
  lv_timer_set_repeat_count(s_game_state_debounce_timer, 1);
}
