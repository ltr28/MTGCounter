#ifndef LCD_BSP_H
#define LCD_BSP_H
#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "lvgl.h"
#include "esp_check.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sets up the QSPI panel, LVGL display + touch input devices and starts the
// LVGL task. Does not push any UI - call app_ui_init() afterwards.
void lcd_lvgl_Init(void);

// LVGL is not thread safe: any code that touches LVGL objects from outside
// the LVGL task (e.g. the Arduino loop() or the knob timer callback) must
// hold this lock first.
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
