# ObiUnlocked

**Local firmware for the OBI / heyOBI energy gateway (ESP32-C3 + SX1262) that turns it into an 868 MHz
receiver for your whole flat.** Your OBI electricity-meter reader keeps working. In the radio's idle time the
gateway also picks up the meters, smoke detectors and sensors around you, shows them in a web dashboard in plain
words, and sends the ones you choose to MQTT / Home Assistant. No cloud.

> German version: [README.de.md](README.de.md) · Built on [OpenOBI](https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud)
> by **atc1441** and contributors, see [Credits](#credits) · upstream docs: [OPENOBI.md](OPENOBI.md)

| | |
|---|---|
| **OBI meter reader** | Everything OpenOBI does (pairing, decryption, import/export/power, history, reader OTA). It always gets the radio first. |
| **wireless M-Bus** (T1, C1, S1) | Heat cost allocators, water / heat / gas meters and smoke detectors from Minol, Zenner, Qundis, Techem, ista, Diehl, Kamstrup, Ei Electronics and others |
| **Weather sensors** (868.3 MHz) | LaCrosse / TFA IT+, Bresser 5-in-1 / 6-in-1 / 7-in-1 |
| **LoRaWAN** | Monitor for the uplinks around you. With your own device's session keys it also decrypts the payload and decodes Cayenne LPP. |

Readings show up as *meter reading*, *value at due date*, *consumption since due date*, *last alarm*,
*battery voltage* and so on, never as raw bytes. The same names, units and Home Assistant device / state classes
go out over MQTT.

## Install

### Gateway already runs OpenOBI (or an older ObiUnlocked)

Open the gateway's web UI → **Settings → Firmware** and upload `obi_gateway_c3-<version>.bin` from the
[Releases page](https://github.com/TAZZERU/ObiUnlocked/releases). WiFi, MQTT, paired readers and history are
kept. A broken image is rejected and the running firmware stays.

To get future updates straight from GitHub, enter `TAZZERU/ObiUnlocked` as the repository in the same place.

### Gateway still runs the original OBI / heyOBI firmware

The stock ESP32-C3 has a **locked bootloader**: ROM download mode is fused off, so USB/UART/JTAG flashing is
impossible. The gateway does accept an **unsigned firmware update from its cloud**, though. You point it at a
small MQTT server on your own computer, once, and serve ObiUnlocked as the "update". After that it never needs a
cloud again.

This method was found and built by **atc1441 and the OpenOBI contributors** (see [Credits](#credits)), and all
tools used below come from their work.

**Easiest: let the installer do it.**

```bash
git clone https://github.com/TAZZERU/ObiUnlocked.git && cd ObiUnlocked
python3 04-connect-your-own-cloud/tools/install_obiunlocked.py
```

The installer works through every step below on its own: it downloads the latest firmware, finds the gateway
over Bluetooth, gets its key, creates certificates, runs the temporary server, sets WiFi, pairs the reader and
shows the download progress. It only asks for your WiFi and your OBI login (or a key you already have).
`--help` lists options for unattended use.

**Or step by step by hand.** You need:
- a computer on the same network, with Python 3 and Bluetooth
- `pip install cryptography bleak paho-mqtt`
- an OBI account (any account works), or physical UART access to the gateway

1. **Get the gateway's TEA key.** It protects the Bluetooth setup channel.
   - Find the Bluetooth name `OBI-XXXXXX`: power the gateway, or hold its button for 2 s, then run
     `python 04-connect-your-own-cloud/tools/ble_scan.py`.
   - Run `python 04-connect-your-own-cloud/tools/fetch_tea_key.py` and enter your OBI login and that name.
   - Without an account: read the key over UART instead
     ([details](04-connect-your-own-cloud/README.md#way-2--from-uart-physical-access-to-the-gateway)).
2. **Create your own certificates.** Use the LAN IP of the computer that will act as the temporary cloud:
   ```bash
   cd 04-connect-your-own-cloud/tools
   python gen_certs.py --host 192.168.1.50
   ```
3. **Start the temporary cloud with ObiUnlocked as the update:**
   ```bash
   python mqtts_server.py --host 0.0.0.0 --port 8883 --ota-firmware obi_gateway_c3-<version>.bin
   ```
4. **Point the gateway at it over Bluetooth.** In a second terminal (same folder), give it WiFi, your CA and the broker, and
   release it from the previous owner. `--pair-sensor` also pairs your meter reader while Bluetooth is open.
   ```bash
   python ble_provision.py --config pki/ble_config.json --key <TEA-KEY> --unbind \
       --pair-sensor --ssid <your-wifi> --password <your-wifi-password>
   ```
   No Python? Use the browser tool [06-tools/obi_gateway_ble.html](06-tools/obi_gateway_ble.html) instead.
   Keep the WiFi password at 32 characters or less on stock firmware.
5. **Wait.** The gateway joins your WiFi, connects to your server, downloads ObiUnlocked in chunks (watch the
   server log) and reboots into it.
6. **Set it up.** Join the WiFi hotspot **`ObiUnlocked-XXXXXX`**, open `http://192.168.4.1`, enter your WiFi
   (and MQTT if you like). The dashboard is then reachable at the gateway's IP. Stop the temporary server. Your
   meter reader re-pairs within a minute; if not, use **Power meters → Bind all readers for 3 min** and press
   the reader's button.
7. **Back up the original firmware.** It is still in the second firmware slot. Open **Debug → Dump full flash**
   and keep the file. Your next update overwrites that slot, so do this right away.

> Step 5 is the only hard-to-reverse step: a bad image can't be fixed over UART. The update is brick-safe,
> though. A broken image is rejected and the old firmware keeps running, and step 7 gives you a full backup
> for going back.

More background, a German version and the manual variants of each step are in
[OPENOBI.md](OPENOBI.md#stock-to-custom) and [04-connect-your-own-cloud](04-connect-your-own-cloud/README.md).

### Other boards / build it yourself

```bash
cd obiunlocked
pip install platformio
pio run -e obi_gateway_c3          # -> .pio/build/obi_gateway_c3/firmware.bin
pio run -e obi_gateway_c3 -t push  # build + upload, address/login in platformio.local.ini
```

Supported boards:
- Heltec Vision Master E290 / WiFi LoRa 32 V3
- LILYGO T-Beam
- Seeed XIAO ESP32S3
- generic ESP32 / ESP32-S3 + SX1262

These boards have no locked bootloader. Flash them over USB (`-t upload`) or use the matching `.bin` from the
release.

## Using it

The web UI has one navigation bar on every page and works on phones:

| Page | What it is for |
|---|---|
| **Power meters** | OBI readers: readings, binding, names, tile layout, report interval, reader firmware over LoRa |
| **My devices** | The radio devices you added, with all their readings. **Only these are sent to MQTT / Home Assistant.** |
| **Nearby** | Everything the gateway hears, neighbours included. Filter by meters / heat cost / smoke detectors / weather / LoRaWAN, then *Add* what is yours. Click a row for details, name, keys, raw telegrams. |
| **Reception** | Which receivers run, how airtime is split, statistics, live log |
| **History / OBI radio / Settings / Debug** | From OpenOBI: energy history, LoRa packet log, WiFi / MQTT / login / firmware, flash tools |
| **Help** | Built-in manual (German) |

**Encrypted devices.** Most metering services send AES-128 encrypted. Without the key you still see
manufacturer, ID, type, signal and status. Ask the metering company, or your landlord, for the device key and
quote the ID shown in the list. Enter it in the device details to see every reading. Security modes 5 and 7 are
supported.

**"Reception too weak".** The gateway heard the device, but the telegram had bit errors. The first block has its
own checksum, so manufacturer, ID and type are still trustworthy and the device appears with an error counter.
Move the gateway higher or more central, or switch off receivers you don't need, until the readings come through.

## How the radio is shared

The SX1262 hears one modulation at a time. Every second runs like this:

```
0 ms      OBI beacon (LoRa 869.5 MHz), then a 300 ms OBI window (extended while OBI frames arrive)
~300 ms   switch (~1-2 ms, no chip reset) to one receiver:
            wM-Bus T1/C1 every other second, otherwise in turn S1, Bresser, LaCrosse, LoRaWAN
970 ms    back to LoRa; a frame that is mid-air gets up to 25 ms to finish
```

The radio stays on OBI for the whole second in these cases:
- while a reader pairs, while a reader OTA runs, and during "bind all"
- for the first 2 minutes after boot
- around every reader's due report

Meters repeat their data constantly, so sharing costs latency, not data. Smoke detectors send only a few times a
day, so they can take a while to show up.

## MQTT / Home Assistant

| Topic | Payload |
|---|---|
| `<base>/wmbus/<MFR><ID>` | `{"rssi","count","last_seen","id","mfr","type","mode","decryption","v":{…}}` |
| `<base>/weather/<protocol>-<id>` | `{"rssi","count","last_seen","channel","v":{"temperature","humidity",…}}` |
| `<base>/lorawan/<DevAddr>` | `{"rssi","count","last_seen","fcnt","fport","snr","sf","freq","v":{…}}` |
| `…/status` | `online` / `offline`, configurable timeout, default 26 h |
| `homeassistant/device/obiu_…/config` | Device-based discovery: one HA device with all its sensors |

Example (a Qundis water meter):

```json
{"rssi":-111,"count":12,"last_seen":"2026-11-12T17:03:11Z","id":"13682041","mfr":"QDS","type":"Wasserzähler",
 "mode":"C1a","decryption":"plain",
 "v":{"volume":104.505,"date_s1":"2025-12-31","volume_s1":86.837,"date_s2":"2026-10-31","volume_s2":103.717,
      "volume_since_due":17.668,"datetime":"2026-11-12T17:03"}}
```

- **Value keys** are stable: quantity + `_max` / `_min` / `_err` + `_s<n>` (storage, `_s1` = due date) + `_t<n>`
  (tariff).
- **Units:** kWh, m³, °C, W, mm.
- **Energy dashboard:** water, gas and heat meters plug straight into the HA energy dashboard.
- **Removal:** removing a device from *My devices* also removes it from Home Assistant.
- **Requirement:** Home Assistant 2024.11 or later.

## What gets decoded

| Source | Without key | With key |
|---|---|---|
| Plain OMS meters (e.g. many water meters) | readings, due-date values, dates, status | – |
| Qundis walk-by block | meter reading, year- and month-end values, consumption since due date | – |
| Techem HCA / water (FHKV III, MK Radio 3/4) | current and previous period, due dates, room / radiator temperature | – |
| Diehl / Sappel IZAR water | reading, month value, battery years | – |
| Encrypted OMS (Minol, Zenner, ista, Techem …) | ID, type, signal, status | all readings |
| Smoke detectors (OMS, Ei Electronics type) | status | last alarm, alarm / removal / test counters, dust level, battery voltage, fault text |
| LaCrosse IT+ · Bresser 5/6/7-in-1 | temperature, humidity, wind, rain, UV, light (6/7-in-1 experimental) | – |
| LoRaWAN | DevAddr, frame counter, port, signal; DevEUI of join requests | payload, Cayenne LPP readings |

Not supported:
- wM-Bus link-layer encryption (CI 0x8D / 0x8F)
- Kamstrup compact frames
- T1 telegrams above ~170 bytes

LoRaWAN listens to one channel/SF at a time. SF11/12 are available only with OBI disabled, because those frames
are longer than the idle time.

## Development

```bash
cd obiunlocked
pip install -r tests/requirements.txt          # needs OpenSSL headers (macOS: brew install openssl)
python -m pytest tests                          # 60+ host tests, decoders built with ASan/UBSan
```

The decoders are plain C++ and are tested on the host against:
- reference telegrams from wmbusmeters and rtl_433
- real captures from the field
- an independent Python encoder (AES, CMAC, CRC, 3-out-of-6, Manchester)

| Path | Role |
|---|---|
| `obiunlocked/src/wmbus_core.cpp` | wM-Bus: T1/C1/S1, CRC, ELL/AFL/TPL, AES mode 5/7, DIF/VIF, manufacturer formats, interpretation |
| `obiunlocked/src/wx_core.cpp` | LaCrosse IT+, Bresser 5/6/7-in-1 |
| `obiunlocked/src/lorawan_core.cpp` | LoRaWAN uplink parsing, MIC, decryption, Cayenne LPP |
| `obiunlocked/src/unl.cpp` | device tables, telegram cache, NVS, `/api/unl*`, MQTT + HA discovery |
| `obiunlocked/src/main.cpp` | OpenOBI gateway + the *shared-radio scheduler* section |
| `obiunlocked/web/` | `index.html` (dashboard), `app.css` / `app.js` (shared look and nav), the OpenOBI pages |

**Footprint (C3):**
- about 90 KB static RAM, the same as plain OpenOBI. The radio log was moved to a shared pool and the reader
  table trimmed.
- web pages are minified and gzip-compressed into flash.
- CI checks that the image stays clear of the 256 KB history reserve in the OTA slot.

**Staying in sync with OpenOBI:**

```bash
git remote add upstream https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud.git
git fetch upstream && git merge upstream/main
```

The in-app GitHub updater only looks for releases once you enter a repository under *Settings → Firmware*, for
example your fork. Otherwise an OpenOBI release would replace ObiUnlocked.

## Credits

ObiUnlocked stands entirely on the shoulders of **[OpenOBI](https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud)**
by **[atc1441](https://github.com/atc1441)** (Aaron Christophel). His work made this possible:
- reverse engineering the OBI / heyOBI system: LoRa protocol, ECDH + TEA, BLE and cloud API
- the trick to free the locked gateway through its own update path
- the complete open gateway firmware that ObiUnlocked extends: reader pairing, energy decoding, history,
  reader OTA, web UI, MQTT

Thank you, and thanks to everyone who contributed to OpenOBI: Patric, PNP, Michel Ferreira Ribeiro,
Stefan Hacker, 6ffm70, Ronald Fiering, Merlin Schumacher, 85368.MB, sirdeekay, RoXXoR and others.

If you like this project, please also star and support the original:
https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud

The radio decoders are independent implementations. Their formats were learned from
[wmbusmeters](https://github.com/wmbusmeters/wmbusmeters) and [rtl_433](https://github.com/merbanan/rtl_433),
credited in [NOTICE.md](NOTICE.md).

## Legal

- **License:** [PolyForm Noncommercial 1.0.0](LICENSE), inherited from OpenOBI. Personal, hobby, research and
  other noncommercial use, modification and sharing are fine. Selling it or using it commercially is not. Keep
  the `LICENSE` file and the `Required Notice` lines when you share it ([NOTICE.md](NOTICE.md)).
- **Third-party:** libraries and reference sources are listed in [NOTICE.md](NOTICE.md). No vendor firmware is
  needed to build ObiUnlocked.
- **Radio:** ObiUnlocked only receives on wM-Bus / LoRaWAN / weather frequencies. Many countries restrict what
  you may do with radio traffic that is not meant for you. In Germany, §5 TDDDG prohibits intercepting such
  messages. *Nearby* inevitably lists your neighbours' devices, so only add, decrypt and forward devices whose
  data belongs to you.
- **Use at your own risk.** Flashing replaces the vendor firmware. Read [DISCLAIMER.md](DISCLAIMER.md).
