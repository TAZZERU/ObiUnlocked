// status_led.cpp — non-blocking blink-pattern engine (see status_led.h).
#include "status_led.h"
#include "board_config.h"

#ifdef PIN_STATUS_LED
#include <Arduino.h>

struct Step { uint16_t ms; uint8_t on; };
// each pattern is a looped sequence of on/off steps
static const Step P_BOOT[]   = { {100, 1}, {100, 0} };                 // fast blink
static const Step P_PORTAL[] = { {120, 1}, {160, 0}, {120, 1}, {1600, 0} };  // double-blink, long gap
static const Step P_WIFI[]   = { {60, 1}, {2940, 0} };                // brief heartbeat every 3 s
static const Step P_OTA[]    = { {70, 1}, {70, 0} };                  // fast blink
static const Step P_ARM[]    = { {40, 1}, {40, 0} };                  // very fast blink

struct Pat { const Step *s; uint8_t n; };
static Pat patOf(LedState st) {
  switch (st) {
    case LED_BOOT:        return { P_BOOT,   2 };
    case LED_PORTAL:      return { P_PORTAL, 4 };
    case LED_WIFI:        return { P_WIFI,   2 };
    case LED_OTA:         return { P_OTA,    2 };
    case LED_RESET_ARMED: return { P_ARM,    2 };
    default:              return { nullptr,  0 };
  }
}

static LedState s_state = LED_OFF;
static uint8_t  s_idx   = 0;
static uint32_t s_next  = 0;
static uint32_t s_blip  = 0;      // millis() until which the activity blip forces the LED on

static inline void raw(bool on) {
#if defined(STATUS_LED_ACTIVE_LOW) && STATUS_LED_ACTIVE_LOW
  digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH);
#else
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
#endif
}

void led_setup() { pinMode(PIN_STATUS_LED, OUTPUT); raw(false); }
void led_write(bool on) { raw(on); }

void led_set(LedState st) {
  if (st == s_state) return;
  s_state = st;
  s_idx = 0;
  Pat p = patOf(st);
  if (p.n) { raw(p.s[0].on); s_next = millis() + p.s[0].ms; }
  else raw(false);
}

void led_blip() { s_blip = millis() + 40; }

void led_loop() {
  uint32_t now = millis();
  if ((int32_t)(s_blip - now) > 0) { raw(true); return; }   // activity blip overlays the pattern
  Pat p = patOf(s_state);
  if (!p.n) { raw(false); return; }
  if ((int32_t)(now - s_next) >= 0) {
    s_idx = (uint8_t)((s_idx + 1) % p.n);
    s_next = now + p.s[s_idx].ms;
    raw(p.s[s_idx].on);
  }
}

#else   // no status LED on this board — no-ops
void led_setup() {}
void led_loop() {}
void led_set(LedState) {}
void led_blip() {}
void led_write(bool) {}
#endif
