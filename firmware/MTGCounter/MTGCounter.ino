// MTG / general life counter for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
// See app_ui.cpp for the UI/state machine and README.md for setup instructions.
#include "lcd_bsp.h"
#include "app_ui.h"

void setup() {
  Serial.begin(115200);
  lcd_lvgl_Init();  // brings up the QSPI panel, touch, backlight and LVGL task
  app_ui_init();    // builds the first screen and starts the knob driver
}

void loop() {
  app_ui_tick();
  delay(5);
}
