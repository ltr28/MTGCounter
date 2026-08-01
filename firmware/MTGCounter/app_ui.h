#ifndef APP_UI_H
#define APP_UI_H

// Builds the first screen (player count picker) and wires up the rotary
// knob. Call once after lcd_lvgl_Init().
void app_ui_init(void);

// Must be called regularly from loop() (a few ms is fine). Drains rotation
// events queued by the knob driver and applies them to whichever screen is
// currently active.
void app_ui_tick(void);

#endif
