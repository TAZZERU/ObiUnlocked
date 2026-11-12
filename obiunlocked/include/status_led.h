// status_led.h — non-blocking status LED with distinct blink patterns.
// ---------------------------------------------------------------------------
// Compiled only when the selected board defines PIN_STATUS_LED (e.g. the OBI gateway's GPIO0); on boards
// without one every call is a no-op, so main.cpp can call these unconditionally. The main loop picks the
// steady state each tick; LoRa RX activity is shown as a brief one-shot blip that overlays the pattern.
#pragma once

enum LedState {
  LED_OFF,          // dark
  LED_BOOT,         // fast blink — early boot
  LED_PORTAL,       // double-blink w/ long gap — no WiFi / setup portal open
  LED_WIFI,         // brief heartbeat every ~3 s — connected & idle
  LED_OTA,          // fast blink — firmware update in progress
  LED_RESET_ARMED,  // very fast blink — reset button held, wipe imminent
};

void led_setup();
void led_loop();               // call every loop()
void led_set(LedState s);      // set the steady pattern (ignored if already active)
void led_blip();               // one-shot activity flash (e.g. a LoRa frame arrived)
void led_write(bool on);       // raw override (used for the terminal factory-reset confirmation)
