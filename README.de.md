# ObiUnlocked

Firmware für das **OBI/heyOBI-Energie-Gateway** (ESP32-C3 + SX1262). Sie macht das Gerät zu einem lokalen
868-MHz-Empfänger für die ganze Wohnung:

- **OBI-Stromzähler-Lesekopf:** funktioniert wie mit OpenOBI und hat beim Funk immer Vorrang
- **wireless M-Bus:** Heizkostenverteiler, Rauchmelder, Wasser-, Wärme- und Gaszähler (Minol, Zenner, Qundis,
  Techem, ista, Diehl, Kamstrup …)
- **Funk-Wettersensoren:** LaCrosse/TFA IT+, Bresser 5-in-1/6-in-1/7-in-1
- **LoRaWAN:** Monitor für Geräte in der Umgebung, mit eigenen Session-Keys auch mit entschlüsselten Messwerten

Alles erscheint im Dashboard des Gateways mit verständlichen Bezeichnungen, also „Zählerstand“, „Stand am
Stichtag“ oder „Letzter Alarm“ statt Rohdaten. Einzelne Geräte gibst du per Schalter an MQTT und Home Assistant
weiter.

ObiUnlocked ist ein Fork von [OpenOBI](https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud) (atc1441).
Dessen Doku, also Reverse Engineering, eigene Cloud und Flashen des Original-Gateways, liegt unverändert in
[OPENOBI.md](OPENOBI.md) und den nummerierten Ordnern.

> Nur Empfang: ObiUnlocked sendet auf wM-Bus, LoRaWAN und den Wetterfrequenzen nichts. Lies nur Geräte aus, deren
> Daten dir zustehen. Lizenz: PolyForm Noncommercial (siehe [LICENSE](LICENSE)), wie beim Originalprojekt.

## Installation

**OpenOBI läuft schon auf dem Gateway:** Öffne im Browser **Einstellungen → Firmware**, lade `firmware.bin` hoch,
fertig. WLAN, MQTT, gekoppelte Leseköpfe und die Historie bleiben erhalten. Ein fehlerhaftes Image weist das
Gateway ab, die laufende Firmware bleibt dann unverändert.

**Original-Gateway mit Hersteller-Firmware:** Einmalig den Weg aus OpenOBI gehen
([Stock-Gateway → eigene Firmware](OPENOBI.md#stock-to-custom)). Danach geht jedes Update über die Weboberfläche.

**Bauen:**

```bash
cd obiunlocked
pip install platformio
pio run -e obi_gateway_c3        # -> .pio/build/obi_gateway_c3/firmware.bin
```

Mit `pio run -e obi_gateway_c3 -t push` wird gebaut und direkt hochgeladen. Adresse und Login stehen in
`platformio.local.ini` (siehe `platformio.local.ini.example`). Die anderen Board-Varianten aus OpenOBI
(Heltec, LILYGO, XIAO, generic) bauen genauso.

## Dashboard

| Bereich | Inhalt |
|---|---|
| **Stromzähler** | OBI-Leseköpfe: Bezug, Einspeisung, Leistung, Batterie, Intervall, Binden, Namen, Kachel-Layout, Lesekopf-Firmware per LoRa |
| **Funkgeräte** | Alle empfangenen Geräte als Liste: Name/Typ, Hersteller, **aktueller Wert**, Signal, zuletzt gesehen, Verschlüsselung. Filter nach Zähler / Heizkosten / Rauchmelder / Wetter / LoRaWAN. Ein Klick öffnet die Details. |
| **Empfang** | Welche Empfänger aktiv sind, Aufteilung der Funkzeit, Statistik, Live-Log |

In den **Details** eines Geräts:
- alle Messwerte mit deutscher Bezeichnung und Einheit
- Name vergeben
- AES-Schlüssel eintragen (wM-Bus) bzw. NwkSKey/AppSKey (LoRaWAN)
- Technik-Infos und Rohdaten (Telegramm und entschlüsselter Klartext)
- Gerät vergessen

Die übrigen Seiten (Verlauf, OBI-Funk, Einstellungen, Debug) stammen aus OpenOBI.

## Was ausgewertet wird

| Gerät | Ohne Schlüssel | Mit Schlüssel / Details |
|---|---|---|
| Unverschlüsselte OMS-Zähler (z. B. Qundis, viele Wasserzähler) | Zählerstand, Stichtagswerte, Datum, Status | – |
| Qundis Walk-by (herstellereigener Block) | Zählerstand, Stand am Jahres- und Monatsstichtag, Verbrauch seit Stichtag, Fehlerdatum | – |
| Techem Heizkostenverteiler / Wasserzähler | aktuelle und vorige Periode, Stichtage, Raum-/Heizkörpertemperatur | – |
| Diehl/Sappel IZAR-Wasserzähler | Zählerstand, Monatswert, Batterie-Restlaufzeit | – |
| Minol, Zenner, ista, Techem (verschlüsselt, AES Mode 5/7) | Hersteller, ID, Typ, Signal, Status | alle Messwerte |
| Rauchmelder (OMS, Ei-Electronics-Bauart) | Status | letzter Alarm, Anzahl Alarme, Demontagen, Testknopf, Verschmutzung, Batteriespannung, Fehlertext |
| LaCrosse IT+ | Temperatur, Luftfeuchte, Batterie | – |
| Bresser 5/6/7-in-1 | Temperatur, Feuchte, Wind, Regen, UV, Licht (6/7-in-1 experimentell) | – |
| LoRaWAN | DevAddr, Frame-Zähler, Port, Signal; bei Join-Requests die DevEUI | Nutzdaten, Cayenne LPP als Messwerte |

Aus Zählerstand und Stichtagswert errechnet die Firmware zusätzlich **„Verbrauch seit Stichtag“**.

**AES-Schlüssel** für Messdienst-Geräte bekommst du vom Messdienst bzw. über Vermieter oder Hausverwaltung, mit
den Geräte-IDs aus der Liste. Ohne Schlüssel lassen sich verschlüsselte Werte nicht lesen.

## Wie der Funkchip geteilt wird

Der SX1262 hört immer nur eine Modulation. Jede Sekunde läuft so ab:

```
0 ms        OBI-Beacon (LoRa 869,5 MHz), dann OBI-Hörfenster (300 ms, verlängert sich bei OBI-Verkehr)
~300 ms     Umschalten (~1–2 ms, ohne Chip-Reset) auf einen Empfänger:
              wM-Bus T1/C1 (868,95 MHz) jede zweite Sekunde,
              sonst reihum: wM-Bus S1, Bresser, LaCrosse (868,3 MHz), LoRaWAN (Kanäle/SF reihum)
970 ms      zurück auf LoRa; läuft gerade ein Telegramm, wird bis zu 25 ms gewartet
```

Komplett auf OBI bleibt der Chip in diesen Fällen:
- beim Koppeln eines Lesekopfs, beim Lesekopf-Update und im „Alle binden“-Fenster
- in den ersten 2 Minuten nach dem Start
- rund um jeden fälligen Zählerstand eines Lesekopfs

Je weniger Empfänger aktiv sind (Tab **Empfang**), desto öfter kommt jeder dran. Funk-Zähler wiederholen ihre Daten
ständig, Heizkostenverteiler etwa alle paar Minuten, deshalb kommt trotz Zeitteilung jeder Wert an. Rauchmelder
senden teils nur wenige Male am Tag. Bis ein Rauchmelder auftaucht, kann es deshalb dauern.

## MQTT / Home Assistant

Das Basis-Topic ist dasselbe wie in OpenOBI (Einstellungen → MQTT).

| Topic | Inhalt |
|---|---|
| `<base>/wmbus/<HERSTELLER><ID>` | `{"rssi","count","last_seen","id","mfr","type","mode","decryption","v":{…}}` |
| `<base>/weather/<protokoll>-<id>` | `{"rssi","count","last_seen","channel","v":{"temperature","humidity",…}}` |
| `<base>/lorawan/<DevAddr>` | `{"rssi","count","last_seen","fcnt","fport","snr","sf","freq","v":{…}}` |
| `…/status` | `online` / `offline` (nach einstellbarer Zeit ohne Empfang, Standard 26 h) |
| `homeassistant/device/obiu_…/config` | HA-Geräte-Discovery: ein Gerät mit allen Sensoren |

Beispiel, dein Qundis-Wasserzähler:

```json
{"rssi":-111,"count":12,"last_seen":"2026-11-12T17:03:11Z","id":"13682041","mfr":"QDS","type":"Wasserzähler",
 "mode":"C1a","decryption":"plain",
 "v":{"volume":104.505,"date_s1":"2025-12-31","volume_s1":86.837,"date_s2":"2026-10-31","volume_s2":103.717,
      "volume_since_due":17.668,"datetime":"2026-11-12T17:03"}}
```

Die Werte haben feste Einheiten: kWh, m³, °C, W, mm. Gerätklassen und State-Classes sind gesetzt, Wasser-, Gas-
und Wärmezähler erscheinen deshalb direkt im HA-Energie-Dashboard. Schaltest du ein Gerät ab oder vergisst es,
entfernt die Firmware es auch aus Home Assistant. Voraussetzung ist **Home Assistant 2024.11 oder neuer**
(MQTT-Geräte-Discovery).

## Entwicklung

```bash
cd obiunlocked
pip install pytest pycryptodome          # OpenSSL-Header nötig (macOS: brew install openssl)
python -m pytest tests/test_wmbus.py tests/test_radio_cores.py
```

Die Decoder sind plattformunabhängig und werden auf dem Rechner getestet. Grundlage sind:
- Referenz-Telegramme aus wmbusmeters und rtl_433
- ein echtes Qundis-Telegramm
- ein unabhängiger Python-Encoder (AES, CMAC, CRC, 3-aus-6, Manchester)

| Datei | Aufgabe |
|---|---|
| `src/wmbus_core.cpp` | wM-Bus: T1/C1/S1, CRC, ELL/AFL/TPL, AES Mode 5/7, DIF/VIF, Hersteller-Formate, Interpretation |
| `src/wx_core.cpp` | LaCrosse IT+, Bresser 5/6/7-in-1 |
| `src/lorawan_core.cpp` | LoRaWAN-Uplinks: Parsing, MIC, Entschlüsselung, Cayenne LPP |
| `src/unl.cpp` | Gerätelisten, Telegramm-Cache, NVS, Web-API `/api/unl*`, MQTT + HA |
| `src/main.cpp` (Abschnitt *shared-radio scheduler*) | Zeitscheiben des Funkchips |
| `web/index.html` | Dashboard |

**Speicher:** Das Radio-Log nutzt einen gemeinsamen 4-KB-Puffer statt fester 15 KB, und es gibt 16 statt
64 Lesekopf-Plätze. Dadurch liegt der feste RAM-Bedarf (~89 KB) trotz der neuen Funktionen auf dem Niveau des
Originals.

Updates aus OpenOBI übernehmen:

```bash
git fetch upstream
git merge upstream/main
```

Die automatische GitHub-Update-Prüfung von OpenOBI ist hier **aus**, solange unter Einstellungen → Firmware kein
Repository eingetragen ist. Ein OpenOBI-Release würde sonst ObiUnlocked überschreiben.

## Grenzen

- Die Zeitscheiben-Steuerung ist auf echter Hardware noch wenig erprobt. Fehlen OBI-Werte, das Hörfenster erhöhen
  oder Empfänger abschalten.
- Nicht unterstützt:
  - wM-Bus mit ELL-Verschlüsselung (CI 0x8D/0x8F)
  - Kamstrup-Kompakttelegramme
  - Telegramme über ~170 Byte (T1)
- Bresser 6-in-1 und 7-in-1 sind nach rtl_433 umgesetzt, aber nicht mit echter Hardware geprüft.
- LoRaWAN: Pro Zeitscheibe wird nur ein Kanal mit einem SF gehört. SF11/12 geht nur ohne OBI.
- Speicherplätze: 80 wM-Bus-, 16 Wetter- und 24 LoRaWAN-Geräte. Ist die Liste voll, ersetzt die Firmware das am
  längsten stumme Gerät ohne Auswahl und ohne Namen.
