# Notices

## License of this repository

ObiUnlocked is a derivative of OpenOBI and is distributed under the same license, the
[PolyForm Noncommercial License 1.0.0](LICENSE). Anyone who receives a copy must also receive the license
terms and these notices:

Required Notice: Copyright atc1441 (https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud)

Required Notice: Copyright the ObiUnlocked contributors (wireless M-Bus / weather / LoRaWAN receiver,
interpretation layer and web UI)

## Third-party software linked into the firmware

| Component | License | Use |
|---|---|---|
| Arduino core for ESP32 / ESP-IDF (incl. mbedTLS, lwIP, FreeRTOS) | LGPL-2.1+ / Apache-2.0 / MIT | platform |
| [RadioLib](https://github.com/jgromes/RadioLib) | MIT | SX1262 driver |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | MIT | MQTT client |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | MIT | WiFi setup portal |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Apache-2.0 | BTHome output |
| GxEPD2 / Adafruit GFX (e-paper boards only) | GPL-3.0 / BSD | display |

Binaries built from this repository contain these components. Their licenses apply to them.

## Reference sources (no code copied)

The protocol decoders are independent implementations. Formats, field layouts and test vectors were taken from
public documentation and these projects. They are credited here, but no source code from them is included:

- [wmbusmeters](https://github.com/wmbusmeters/wmbusmeters) (GPL-3.0): Qundis, Techem, Diehl IZAR,
  Ei Electronics telegram layouts and reference telegrams used in the tests
- [rtl_433](https://github.com/merbanan/rtl_433) (GPL-2.0+): Bresser and LaCrosse sensor layouts and
  reference messages
- EN 13757-3/-4/-7, OMS specification, LoRaWAN 1.0.x specification, Cayenne LPP format description

## Trademarks and vendor material

OBI, heyOBI, SUMEC and all meter manufacturers' names belong to their owners. They are used only to describe
compatibility. No vendor firmware is required to build or run ObiUnlocked. See [DISCLAIMER.md](DISCLAIMER.md).
