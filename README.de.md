# ObiUnlocked (Deutsch)

> English version: [README.md](README.md)

Firmware für das **OBI/heyOBI-Energie-Gateway** (ESP32-C3 + SX1262). Sie macht das Gerät zu einem lokalen
868-MHz-Empfänger für die ganze Wohnung:

- **OBI-Stromzähler-Lesekopf:** funktioniert wie mit OpenOBI und hat beim Funk immer Vorrang
- **wireless M-Bus:** Heizkostenverteiler, Rauchmelder, Wasser-, Wärme- und Gaszähler (Minol, Zenner, Qundis,
  Techem, ista, Diehl, Kamstrup …)
- **Funk-Wettersensoren:** LaCrosse/TFA IT+, Bresser 5-in-1/6-in-1/7-in-1
- **LoRaWAN:** Monitor für Geräte in der Umgebung, mit eigenen Session-Keys auch mit entschlüsselten Messwerten

Alles erscheint im Dashboard des Gateways mit verständlichen Bezeichnungen, also „Zählerstand“, „Stand am
Stichtag“ oder „Letzter Alarm“ statt Rohdaten. Geräte, die du unter „In der Nähe“ hinzufügst, gehen an MQTT und Home Assistant.

ObiUnlocked ist ein Fork von [OpenOBI](https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud) (atc1441).
Dessen Doku, also Reverse Engineering, eigene Cloud und Flashen des Original-Gateways, liegt unverändert in
[OPENOBI.md](OPENOBI.md) und den nummerierten Ordnern.

> Nur Empfang: ObiUnlocked sendet auf wM-Bus, LoRaWAN und den Wetterfrequenzen nichts. Lies nur Geräte aus, deren
> Daten dir zustehen. Lizenz: PolyForm Noncommercial (siehe [LICENSE](LICENSE)), wie beim Originalprojekt.

## Installation

### Auf dem Gateway läuft schon OpenOBI (oder ein älteres ObiUnlocked)

Öffne im Browser **Einstellungen → Firmware** und lade `obi_gateway_c3-<version>.bin` von der
[Release-Seite](https://github.com/TAZZERU/ObiUnlocked/releases) hoch. WLAN, MQTT, gekoppelte Leseköpfe und die
Historie bleiben erhalten. Ein fehlerhaftes Image weist das Gateway ab, die laufende Firmware bleibt dann
unverändert. Künftige Updates holt sich das Gateway direkt aus diesem Repository: **Einstellungen → Firmware → nach Updates
suchen**, oder du schaltest dort die automatische Aktualisierung ein.

### Auf dem Gateway läuft noch die Original-Firmware von OBI

Der Bootloader ist gesperrt, Flashen über USB, UART oder JTAG geht also nicht. Das Gateway nimmt aber ein
**unsigniertes Update „aus seiner Cloud“** an. Dafür spielt dein Computer einmalig diese Cloud und liefert
ObiUnlocked als Update aus. Danach braucht das Gateway nie wieder eine Cloud. Diesen Weg und die Werkzeuge dafür
haben **atc1441 und die OpenOBI-Mitwirkenden** entwickelt (siehe [Credits](#credits)).

**Am einfachsten mit dem Installationsskript:**

```bash
git clone https://github.com/TAZZERU/ObiUnlocked.git && cd ObiUnlocked
python3 04-connect-your-own-cloud/tools/install_obiunlocked.py
```

Das Skript erledigt alles nacheinander:
- Firmware herunterladen
- Gateway per Bluetooth finden
- Schlüssel holen
- Zertifikate erzeugen
- temporären Server starten
- WLAN setzen und Lesekopf koppeln
- Download-Fortschritt anzeigen

Es fragt nur nach WLAN und OBI-Login (oder einem Schlüssel, den du schon hast). Voraussetzung: Computer mit
Python 3 und Bluetooth im selben Netz wie das Gateway.

**Oder von Hand** (Werkzeuge in `04-connect-your-own-cloud/tools`, vorher `pip install cryptography bleak paho-mqtt`):

1. **TEA-Schlüssel holen.**
   - Bluetooth-Namen `OBI-XXXXXX` finden: `python ble_scan.py`. Vorher das Gateway neu einstecken oder seine
     Taste 2 s halten.
   - `python fetch_tea_key.py` ausführen, mit beliebigem OBI-Login. Alternativ den Schlüssel per UART auslesen.
2. **Zertifikate erzeugen:** `python gen_certs.py --host <IP dieses Computers>`
3. **Server mit ObiUnlocked starten:** `python mqtts_server.py --host 0.0.0.0 --port 8883 --ota-firmware obi_gateway_c3-<version>.bin`
4. **Gateway per Bluetooth einrichten:** in einem zweiten Terminal
   `python ble_provision.py --config pki/ble_config.json --key <TEA-KEY> --unbind --pair-sensor --ssid <WLAN> --password <WLAN-Passwort>`.
   Das WLAN-Passwort darf höchstens 32 Zeichen haben.
5. **Warten:** Das Gateway lädt ObiUnlocked herunter (im Server-Log zu sehen) und startet damit neu.
6. **Einrichten:** Mit dem Hotspot **`ObiUnlocked-XXXXXX`** verbinden, `http://192.168.4.1` öffnen und WLAN
   (und bei Bedarf MQTT) eintragen. Den Server danach beenden.
7. **Sichern:** Sofort unter **Debug → Dump full flash** ein Backup ziehen. Die Original-Firmware liegt noch im
   zweiten Slot. Das nächste Update überschreibt diesen Slot.

Das Update ist sicher gegen Fehlversuche: Ein kaputtes Image wird abgelehnt. Mehr Details stehen in
[OPENOBI.md](OPENOBI.md#stock-to-custom).

### Andere Boards / selbst bauen

```bash
cd obiunlocked
pip install platformio
pio run -e obi_gateway_c3        # -> .pio/build/obi_gateway_c3/firmware.bin
```

Mit `pio run -e obi_gateway_c3 -t push` wird gebaut und direkt hochgeladen. Adresse und Login stehen in
`platformio.local.ini`. Heltec, LILYGO, XIAO und generische ESP32 + SX1262 haben keinen gesperrten Bootloader.
Sie flashst du per USB (`-t upload`) oder mit der passenden `.bin` aus dem Release.

## Weboberfläche

Eine gemeinsame Navigationsleiste auf allen Seiten, auch fürs Handy:

| Seite | Inhalt |
|---|---|
| **Stromzähler** | OBI-Leseköpfe: Werte, Binden, Namen, Kachel-Layout, Intervall, Lesekopf-Firmware über LoRa |
| **Meine Geräte** | Deine hinzugefügten Funkgeräte mit allen Werten – **nur diese** gehen per MQTT an Home Assistant |
| **In der Nähe** | Alles, was das Gateway empfängt (auch Nachbarn); filtern, „Hinzufügen“, Klick öffnet Details (Name, AES-/LoRaWAN-Schlüssel, Rohdaten) |
| **Empfang** | aktive Empfänger, Funkzeit, Statistik, Live-Log |
| **Verlauf / OBI-Funk / Einstellungen / Debug** | aus OpenOBI |
| **Hilfe** | eingebaute Anleitung |

Geräte, deren Telegramme nur mit Übertragungsfehlern ankommen, erscheinen trotzdem (Gerätekopf hat eine eigene
Prüfsumme) – markiert als **„Empfang zu schwach“** mit Fehlerzähler.

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
python -m pytest tests
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

Der eingebaute Updater und die Update-Anzeige in Home Assistant prüfen standardmäßig die Releases von
`TAZZERU/ObiUnlocked`. Ein Fork trägt unter Einstellungen → Firmware sein eigenes Repository ein.

## Credits

ObiUnlocked baut vollständig auf **[OpenOBI](https://github.com/atc1441/OBI_Energy_Tracker_Local_Cloud)** von
**[atc1441](https://github.com/atc1441)** (Aaron Christophel) auf. Von ihm stammen:
- das Reverse Engineering des OBI/heyOBI-Systems
- der Weg, das gesperrte Gateway über seinen eigenen Update-Kanal zu befreien
- die komplette offene Gateway-Firmware, die ObiUnlocked erweitert

Danke dafür, und an alle OpenOBI-Mitwirkenden: Patric, PNP, Michel Ferreira Ribeiro, Stefan Hacker, 6ffm70,
Ronald Fiering, Merlin Schumacher, 85368.MB, sirdeekay, RoXXoR und weitere. Wenn dir das Projekt gefällt,
unterstütze bitte auch das Original. Die Funk-Decoder sind eigene Implementierungen. Die Formate dafür stammen
aus [wmbusmeters](https://github.com/wmbusmeters/wmbusmeters) und [rtl_433](https://github.com/merbanan/rtl_433)
(siehe [NOTICE.md](NOTICE.md)).

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
