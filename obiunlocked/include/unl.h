// unl.h -- ObiUnlocked: third-party 868 MHz devices next to the OBI reader link.
//
// The gateway's single SX1262 is time-shared (scheduler in main.cpp): the OBI LoRa link keeps absolute
// priority (1 Hz beacon, reader frames, pairing, reader OTA); the idle remainder of every second goes to one
// "listener" at a time:
//   UL_WMB_TC   wireless M-Bus T1 + C1, 868.95 MHz   (every other period when enabled -- most devices live here)
//   UL_WMB_S1   wireless M-Bus S1, 868.3 MHz
//   UL_WX_BRES  Bresser 5/6/7-in-1 weather stations, 868.3 MHz
//   UL_WX_LACR  LaCrosse IT+ / TFA sensors, 868.3 MHz
//   UL_LORAWAN  LoRaWAN uplinks, rotating over the selected EU868 channels and spreading factors
//
// Everything heard is listed on the dashboard (Funkgeräte). Selected devices are published to MQTT
// (<base>/wmbus|weather|lorawan/<key>) with Home Assistant device-based discovery.
//
// Threading: unl_rx_push(), unl_radio_cfg() and the unl_note_*() hooks are called from the high-priority
// LoRa task and only touch a FreeRTOS queue / plain counters. Everything else runs on the web task.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

class PubSubClient;

enum UnlListener : uint8_t { UL_WMB_TC = 0, UL_WMB_S1, UL_WX_BRES, UL_WX_LACR, UL_LORAWAN, UL_COUNT };
static const uint32_t UNL_LW_CH_KHZ[8] = { 868100, 868300, 868500, 867100, 867300, 867500, 867700, 867900 };

struct UnlRadioCfg {
  bool     obiOn = true;                                   // OBI gateway (beacon + readers)
  uint8_t  listen = (1 << UL_WMB_TC) | (1 << UL_WMB_S1) | (1 << UL_WX_BRES) | (1 << UL_WX_LACR);
  uint16_t obiWindowMs = 300;                              // LoRa listen window after every beacon
  uint8_t  lwChMask = 0x07;                                // bit i = UNL_LW_CH_KHZ[i]
  uint8_t  lwSfMask = 0x01;                                // bit 0 = SF7 ... bit 5 = SF12
};

void unl_radio_cfg(UnlRadioCfg &out);    // consistent copy of the current settings
void unl_rx_push(uint8_t listener, const uint8_t *data, size_t len, int16_t rssi, int8_t snrQ4, uint8_t sf,
                 uint32_t freqKHz);
void unl_note_slot(uint8_t slot, uint32_t ms);   // airtime per slot: 0 = OBI, 1 + listener
void unl_note_switch(bool ok);
void unl_note_obi_rx(uint16_t msAfterBeacon);
void unl_note_defer();

void unl_setup();                                // load settings + known devices from NVS (call in setup())
void unl_service();                              // decode queued frames; call from the web task loop
void unl_register_routes(WebServer &srv, WebServer::THandlerFunction (*guard)(WebServer::THandlerFunction));
void unl_mqtt_reset();                           // MQTT (re)connected: re-announce everything
void unl_mqtt_service(PubSubClient &mqtt, const String &base, const String &gwAvailTopic, const String &gwUid,
                      uint32_t &pubCount);
void unl_factory_reset();
