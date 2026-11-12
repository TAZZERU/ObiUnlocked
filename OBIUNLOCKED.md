# ObiUnlocked

Fork von [OpenOBI / OBI Energy Tracker Local Cloud](https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud)
(atc1441). Läuft auf demselben **OBI/heyOBI-Gateway (ESP32-C3 + SX1262)** und kann alles, was OpenOBI kann. Der
OBI-Lesekopf am Stromzähler funktioniert unverändert weiter. Zusätzlich hört das Gateway in den Funkpausen andere
868-MHz-Geräte in der Umgebung mit:

| Funk | Geräte | Was sichtbar ist |
|---|---|---|
| **wireless M-Bus T1/C1** (868,95 MHz) | Heizkostenverteiler, Rauchmelder, Wasser-, Wärme-, Gaszähler von Minol, Zenner, Techem, ista, Brunata, Qundis, Kamstrup, Diehl … | Hersteller, ID, Gerätetyp, RSSI, Status. Mit AES-Schlüssel alle Messwerte (Verbrauch, Stichtagswerte, Datum, Fehlerflags …) |
| **LoRaWAN** (ein EU868-Kanal + SF wählbar) | beliebige LoRaWAN-Sensoren | DevAddr, Frame-Zähler, Port, Länge, RSSI/SNR, bei Join-Requests die DevEUI. Nutzdaten sind netzseitig verschlüsselt. |

Auf der Weboberfläche unter **📟 Funkgeräte** (`/meters`) siehst du alle empfangenen Geräte und wählst sie einzeln
aus. Ausgewählte Geräte gehen per MQTT an Home Assistant (Auto-Discovery).

> ⚠️ Nur zum Empfang. Es wird nichts auf wM-Bus/LoRaWAN gesendet. Lies nur Geräte aus, deren Daten dir zustehen
> (deine Wohnung). Die Lizenz des Originalprojekts gilt weiter: PolyForm Noncommercial, siehe [LICENSE](LICENSE).

## Installieren (OpenOBI ist schon drauf)

1. Firmware bauen (siehe unten) oder die fertige `firmware.bin` für `obi_gateway_c3` nehmen.
2. Weboberfläche des Gateways öffnen → **Settings → Firmware → manuell flashen**: `firmware.bin` hochladen.
   Alternativ ohne Web-Login: `curl -F "file=@firmware.bin" http://<gateway-ip>/api/selfupdate`. Mit Login
   gehen Bauen und Hochladen in einem Schritt: `pio run -e obi_gateway_c3 -t push`, dazu Adresse und Login in
   `platformio.local.ini` eintragen (siehe `platformio.local.ini.example`).
3. Das Gateway startet neu. Dual-OTA: Ein falsches Image wird abgelehnt, die laufende Firmware bleibt erhalten.
   Alle Einstellungen bleiben erhalten, also WLAN, MQTT, gekoppelte Lesekopf-Geräte und die Historie.
4. In den ersten 2 Minuten nach dem Start bleibt das Funkmodul komplett beim OBI-Lesekopf. Danach erscheinen nach
   und nach wM-Bus-Geräte unter **📟 Funkgeräte**.

Zurück zu OpenOBI geht jederzeit über denselben Weg mit einer OpenOBI-`firmware.bin`.

## Bedienung

**Funkgeräte → wM-Bus**
- Jede Zeile ist ein Gerät (Hersteller + 8-stellige ID, die meist auf dem Gerät aufgedruckt ist). Ein Klick öffnet
  die Details mit allen dekodierten Datensätzen, dem Rohtelegramm und dem entschlüsselten Klartext.
- **Name** vergeben, z. B. „HKV Bad“. Der Name erscheint so auch in Home Assistant.
- **AES-Schlüssel** eintragen (32 Hex-Zeichen). Fast alle Minol-, Zenner-, Techem- und ista-Geräte senden
  verschlüsselt (Security Mode 5 oder 7). Den Schlüssel bekommst du vom Messdienstleister bzw. über den Vermieter.
  Bei falschem Schlüssel zeigt die Statusspalte „falscher Key“.
- **Häkchen „HA“** = an MQTT/Home Assistant senden.

**Funkgeräte → LoRaWAN**: Erst unter *Einstellungen* aktivieren und Kanal und SF wählen (Standard 868,1 MHz / SF7).
Ein Klick auf eine Zeile vergibt einen Namen.

**Funkgeräte → Live-Log**: die letzten Telegramme als Rohdaten, auch solche mit CRC-Fehler (meist schwacher Empfang).

**Funkgeräte → Einstellungen**
- *OBI-Gateway* an/aus. Aus bedeutet: kein Beacon, also kein Lesekopf, dafür 100 % Funkzeit für wM-Bus/LoRaWAN.
- *wireless M-Bus* / *LoRaWAN-Monitor* an/aus. Sind beide an, wechseln sie sich sekundenweise ab.
- *OBI-Hörfenster* (Standard 300 ms): So lange bleibt das Funkmodul nach jedem Beacon mindestens auf LoRa.
- *nicht verfügbar nach* (Standard 26 h): Danach meldet HA ein stummes Gerät als „nicht verfügbar“. Rauchmelder
  senden teils nur wenige Male am Tag, deshalb ist der Wert großzügig gewählt.

## Wie der Funkchip geteilt wird

Der SX1262 kann immer nur **eine** Modulation hören. Jede Sekunde läuft so ab:

```
0 ms      OBI-Beacon (LoRa 869,5 MHz) → OBI-Hörfenster (300 ms, verlängert sich bei jedem OBI-Paket um 150 ms)
~300 ms   Umschalten (~1–2 ms, ohne Chip-Reset) → wM-Bus 868,95 MHz bzw. LoRaWAN
970 ms    zurück auf LoRa (läuft gerade ein Telegramm, wird bis zu 25 ms gewartet)
1000 ms   nächster Beacon
```

Das Funkmodul bleibt die **ganze Sekunde auf OBI**, solange
- ein Reader-OTA vorbereitet ist oder läuft, das „Bind all“-Fenster offen ist oder ein zugeordneter Lesekopf gerade
  koppelt,
- die ersten 2 Minuten nach dem Start laufen,
- der Energie-Report eines Lesekopfs fällig ist (aus Intervall und letztem Report: 1,5 s davor bis 2,5 s danach).

Oben auf der Seite zeigt ein Balken die tatsächliche Aufteilung der Funkzeit. Dazu kommen Statistiken. **„Versatz
max“** gibt an, wie spät nach dem Beacon bisher OBI-Pakete kamen. Liegt der Wert in der Nähe des Hörfensters,
erhöhe das Fenster.

**Kompromiss:** In den Funkpausen kann ein wM-Bus-Telegramm verloren gehen. Diese Geräte wiederholen ihre Daten aber
ständig (Heizkostenverteiler etwa alle 2–4 Minuten), deshalb kommt in der Praxis trotzdem jeder Wert an. Fehlen
OBI-Werte, schalte zum Test wM-Bus/LoRaWAN kurz ab.

## MQTT / Home Assistant

Basis-Topic wie bei OpenOBI (Settings → MQTT, Standard `obi`).

| Topic | Inhalt |
|---|---|
| `<base>/wmbus/<MFR><ID>` | JSON (retained): `id, mfr, type, ver, mode, rssi, count, status, dec, last_seen, v{…}` |
| `<base>/wmbus/<MFR><ID>/status` | `online` / `offline` |
| `<base>/lorawan/<DevAddr>` bzw. `<base>/lorawan/J<DevEUI>` | JSON: `mtype, fcnt, fport, len, rssi, snr, sf, freq, count, last_seen` |
| `homeassistant/device/obiu_wm_<MFR><ID>/config` | HA **Device-Discovery** (ein Gerät mit allen Sensoren) |

Beispiel für einen Heizkostenverteiler:

```json
{"id":"12345678","mfr":"MIN","type":"hca","ver":1,"mode":"T1","rssi":-71,"count":42,"status":0,
 "dec":"decrypted","last_seen":"2026-11-12T15:04:11Z",
 "v":{"hca":123456,"date_s1":"2025-12-31","hca_s1":10,"error_flags":0}}
```

Die Schlüssel in `v` sind stabil: Größe + `_max`/`_min`/`_err` + `_s<n>` (Speicher; `_s1` ist meist der
Stichtag) + `_t<n>` (Tarif). Energie wird in kWh angegeben, Volumen in m³, Temperatur in °C, Leistung in W. Damit
landen Wärme-, Wasser- und Gaszähler direkt im HA-Energie-Dashboard.

Wird ein Gerät abgewählt oder vergessen, entfernt die Firmware es auch aus Home Assistant.
**Voraussetzung:** Home Assistant **2024.11 oder neuer**, denn erst ab dieser Version gibt es MQTT Device-Discovery.

## Bauen

```bash
cd open_obi_energy_meter
pip install platformio
pio run -e obi_gateway_c3          # → .pio/build/obi_gateway_c3/firmware.bin
```

Host-Tests für den wM-Bus-Decoder (unabhängiger Python-Encoder mit AES/CMAC, ASan/UBSan):

```bash
pip install pytest pycryptodome    # OpenSSL-Header nötig (macOS: brew install openssl)
python -m pytest tests/test_wmbus.py
```

## Code-Überblick

| Datei | Aufgabe |
|---|---|
| `include/wmbus_core.h`, `src/wmbus_core.cpp` | portabler wM-Bus-Decoder: T1 (3-aus-6), C1 Frame A/B, CRC, ELL/AFL/TPL, AES Mode 5/7, DIF/VIF |
| `src/wmbus_aes_mbedtls.cpp` | AES-Block über mbedtls (Hardware-AES des C3) |
| `include/unl.h`, `src/unl.cpp` | Gerätelisten, NVS, Web-API `/api/unl*`, MQTT + HA-Discovery |
| `src/main.cpp` (Abschnitt „shared-radio scheduler“) | Zeitscheiben-Steuerung des SX1262 |
| `web/meters.html` | Seite **📟 Funkgeräte** |
| `tests/test_wmbus.py`, `tests/wmbus_native.cpp` | Host-Tests |

Die Upstream-Dateien sind nur an wenigen, kommentierten Stellen angepasst (Hooks in `gateway_web.cpp`, Scheduler in
`main.cpp`, Navigation in `index.html`). So bleiben Merges von OpenOBI einfach:

```bash
git fetch upstream
git merge upstream/main
```

Die automatische GitHub-Update-Prüfung von OpenOBI ist in diesem Fork **aus**, solange unter *Settings → Firmware*
kein Repository eingetragen ist. Sonst würde ein OpenOBI-Release diesen Fork überschreiben.

## Grenzen / bekannte Punkte

- **Nicht auf echter Hardware getestet:** Die Funk-Zeitscheiben (Modemwechsel LoRa ⇄ GFSK) laufen bisher nur im
  Build und in den Host-Tests. Die Statistik auf `/meters` (Umschaltungen, Fehler, Funkzeit, OBI-Versatz) ist dafür
  da, das im Betrieb zu prüfen.
- wM-Bus **S1/S2** (868,3 MHz) und **ELL-Verschlüsselung** (CI 0x8D/0x8F, AES-CTR) werden nicht unterstützt.
  Herstellerspezifische Telegramme (CI 0xA0–0xB7) erscheinen nur als Rohdaten.
- Telegramme länger als ~170 Byte (T1) werden verworfen. Das ist bei Verbrauchsgeräten unüblich.
- Im RAM: bis zu 36 wM-Bus- und 24 LoRaWAN-Geräte. Bei vollem Speicher wird das am längsten stumme Gerät ohne
  Auswahl/Name/Key ersetzt.
