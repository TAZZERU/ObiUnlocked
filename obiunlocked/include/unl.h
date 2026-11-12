// unl.h -- ObiUnlocked: third-party 868 MHz devices next to the OBI reader link.
//
// The gateway's single SX1262 is time-shared (see the scheduler in main.cpp): the OBI LoRa link keeps
// absolute priority (1 Hz beacon, reader frames, pairing, reader OTA), and the idle remainder of every
// second is spent listening for
//   * wireless M-Bus T1/C1 on 868.95 MHz -- heat cost allocators, smoke detectors, water/heat meters
//     (Minol, Zenner, Techem, ista, Brunata, Qundis, ...), decoded by wmbus_core.cpp
//   * LoRaWAN uplinks on one configurable channel/SF -- metadata only (DevAddr, FCnt, FPort, RSSI);
//     the application payload stays AES-encrypted without that device's session keys.
//
// Everything heard is listed on the /meters page. Devices the user selects there are published to MQTT
// (<base>/wmbus/<key>, <base>/lorawan/<key>) with Home Assistant device-based discovery.
//
// Threading: unl_rx_push(), unl_radio_cfg() and the unl_note_*() hooks are called from the high-priority
// LoRa task and only touch a FreeRTOS queue / plain counters. Everything else runs on the web task.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

class PubSubClient;

// ---- radio side (LoRa task) -------------------------------------------------------------------------------
enum UnlRxKind : uint8_t { UNL_RX_WMBUS = 1, UNL_RX_LORAWAN = 2 };
enum UnlSlot : uint8_t { UNL_SLOT_OBI = 0, UNL_SLOT_WMBUS = 1, UNL_SLOT_LORAWAN = 2, UNL_SLOT_COUNT = 3 };

struct UnlRadioCfg {
  bool     obiOn = true;          // run the OBI gateway (beacon, readers). Off = third-party listening only.
  bool     wmbusOn = true;        // listen for wireless M-Bus T1/C1 in the idle time
  bool     lorawanOn = false;     // listen for LoRaWAN uplinks in the idle time
  uint16_t obiWindowMs = 300;     // LoRa listen window after every beacon before the radio may switch away
  uint32_t lorawanFreqKHz = 868100;
  uint8_t  lorawanSf = 7;
};

void unl_radio_cfg(UnlRadioCfg &out);    // consistent copy of the current settings
void unl_rx_push(uint8_t kind, const uint8_t *data, size_t len, int16_t rssi, int8_t snrQ4, uint8_t sf,
                 uint32_t freqKHz);      // snrQ4 = SNR * 4 (LoRaWAN only)
void unl_note_slot(uint8_t slot, uint32_t ms);   // airtime bookkeeping per slot
void unl_note_switch(bool ok);                   // a modem switch happened (ok = all settings applied)
void unl_note_obi_rx(uint16_t msAfterBeacon);    // an OBI reader frame arrived this long after our beacon
void unl_note_defer();                           // a slot end was postponed because a frame was mid-air

// ---- web task ------------------------------------------------------------------------------------------------
void unl_setup();                                // load settings + known devices from NVS (call in setup())
void unl_service();                              // decode queued frames; call from the web task loop
void unl_register_routes(WebServer &srv, WebServer::THandlerFunction (*guard)(WebServer::THandlerFunction));
void unl_mqtt_reset();                           // MQTT (re)connected: re-announce everything
void unl_mqtt_service(PubSubClient &mqtt, const String &base, const String &gwAvailTopic, const String &gwUid,
                      uint32_t &pubCount);
void unl_factory_reset();                        // wipe ObiUnlocked settings + device lists
