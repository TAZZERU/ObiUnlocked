#!/usr/bin/env python3
"""Install ObiUnlocked on an OBI / heyOBI gateway that still runs the original firmware -- in one run.

The stock gateway's bootloader is locked, but it accepts an unsigned firmware update from "its cloud". This
script plays that cloud once, using the OpenOBI tools next to it (method and tools by atc1441 + contributors):

  1. download the newest ObiUnlocked firmware for the gateway (or use --firmware)
  2. find the gateway over Bluetooth (OBI-XXXXXX)
  3. get its TEA key -- via your OBI login, or paste it
  4. create certificates for this computer (gen_certs.py)
  5. start the temporary update server with ObiUnlocked as the update (mqtts_server.py)
  6. hand the gateway your WiFi + this server over Bluetooth, optionally pair the meter reader (ble_provision.py)
  7. follow the download until the gateway reboots into ObiUnlocked

Run it from anywhere:  python3 04-connect-your-own-cloud/tools/install_obiunlocked.py
Needs: Python 3.8+, Bluetooth, the gateway and this computer in the same network.
"""
import argparse
import getpass
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PKI = os.path.join(HERE, "pki")
DEPS = {"cryptography": "cryptography", "bleak": "bleak"}


def say(msg=""):
    print(msg, flush=True)


def step(n, msg):
    say(f"\n\033[1m[{n}/7] {msg}\033[0m")


def ask(prompt, default=None, secret=False):
    shown = f"{prompt} [{default}]: " if default else f"{prompt}: "
    val = getpass.getpass(shown) if secret else input(shown)
    return val.strip() or (default or "")


def yes(prompt, default=True):
    val = input(f"{prompt} [{'Y/n' if default else 'y/N'}]: ").strip().lower()
    return default if not val else val.startswith(("y", "j"))


def die(msg):
    say(f"\n\033[31mStopped: {msg}\033[0m")
    sys.exit(1)


# ------------------------------------------------------------------------------------------------ helpers
def ensure_deps():
    missing = []
    for mod, pkg in DEPS.items():
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if not missing:
        return
    say(f"Missing Python packages: {', '.join(missing)}")
    if not yes("Install them now with pip?"):
        die("install them with: python3 -m pip install " + " ".join(missing))
    subprocess.check_call([sys.executable, "-m", "pip", "install", *missing])


def download_firmware(repo, dest_dir):
    api = f"https://api.github.com/repos/{repo}/releases/latest"
    req = urllib.request.Request(api, headers={"Accept": "application/vnd.github+json", "User-Agent": "obiunlocked-installer"})
    with urllib.request.urlopen(req, timeout=20) as r:
        rel = json.load(r)
    asset = next((a for a in rel.get("assets", []) if re.match(r"obi_gateway_c3-.*\.bin$", a["name"])), None)
    if not asset:
        die(f"no obi_gateway_c3 firmware in the latest release of {repo}")
    path = os.path.join(dest_dir, asset["name"])
    if not os.path.exists(path):
        say(f"Downloading {asset['name']} ({asset['size'] // 1024} KB) from {repo} {rel.get('tag_name', '')} ...")
        req = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "obiunlocked-installer"})
        with urllib.request.urlopen(req, timeout=120) as r, open(path, "wb") as f:
            f.write(r.read())
    return path


def check_image(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 100_000 or data[0] != 0xE9:
        die(f"{path} is not an ESP32 firmware image")
    say(f"Firmware: {os.path.basename(path)}, {len(data):,} bytes")


def scan_gateways(seconds):
    import asyncio
    from bleak import BleakScanner

    async def run():
        found = await BleakScanner.discover(timeout=seconds)
        return [d for d in found if d.name and d.name.startswith("OBI-")]
    return asyncio.run(run())


def pick_gateway(address):
    if address:
        return address, None
    while True:
        say("Scanning for OBI gateways (10 s) ...")
        devs = scan_gateways(10)
        if len(devs) == 1:
            say(f"Found {devs[0].name} ({devs[0].address})")
            return devs[0].address, devs[0].name
        if devs:
            for i, d in enumerate(devs, 1):
                say(f"  {i}) {d.name}  {d.address}")
            i = int(ask("Which one", "1")) - 1
            return devs[i].address, devs[i].name
        say("No gateway found. Unplug and replug it, or hold its button for ~2 s, so it advertises over Bluetooth.")
        if not yes("Scan again?"):
            die("no gateway found")


def get_key(key, ble_name):
    if key:
        return key
    say("The TEA key unlocks the gateway's Bluetooth setup. Two ways to get it:")
    say("  1) with any OBI account (the script asks the OBI cloud for this gateway's key)")
    say("  2) paste a key you already have (e.g. read over UART)")
    if ask("Choose", "1") == "2":
        key = ask("TEA key (32 hex)")
    else:
        sys.path.insert(0, HERE)
        from fetch_tea_key import get_tea_key
        name = ble_name or ask("Gateway Bluetooth name (OBI-XXXXXX)")
        key = get_tea_key(ask("OBI account email"), ask("OBI account password", secret=True), name)
    key = (key or "").replace(" ", "")
    if not re.fullmatch(r"[0-9a-fA-F]{32}", key):
        die("no valid TEA key (need 32 hex characters)")
    return key


class Server:
    """mqtts_server.py in the background: output goes to a log file, OTA progress is parsed live."""

    def __init__(self, firmware, port, log_path):
        self.log_path = log_path
        self.percent = -1
        self.connected = False
        self.done = False
        self.proc = subprocess.Popen(
            [sys.executable, "-u", os.path.join(HERE, "mqtts_server.py"), "--host", "0.0.0.0", "--port", str(port),
             "--ota-firmware", firmware],
            cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        with open(self.log_path, "w") as log:
            for line in self.proc.stdout:
                log.write(line)
                log.flush()
                if "CONNECT" in line:
                    self.connected = True
                m = re.search(r"\((\d+)% of \d+\)", line)
                if m:
                    self.percent = int(m.group(1))
                if "[LAST" in line:
                    self.done = True

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.alive():
            self.proc.terminate()


# ------------------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--firmware", help="use this .bin instead of downloading the latest release")
    ap.add_argument("--repo", default="TAZZERU/ObiUnlocked", help="GitHub repo to download the firmware from")
    ap.add_argument("--key", help="gateway TEA key (skips the OBI login)")
    ap.add_argument("--address", help="gateway Bluetooth address (skips the scan)")
    ap.add_argument("--host", help="this computer's LAN IP (default: detected)")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--ssid", help="WiFi name for the gateway")
    ap.add_argument("--wifi-password", help="WiFi password for the gateway")
    ap.add_argument("--no-pair-reader", action="store_true", help="don't pair the OBI meter reader during setup")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds to wait for the firmware download")
    a = ap.parse_args()

    say("\033[1mObiUnlocked installer for stock OBI / heyOBI gateways\033[0m")
    say("Uses the OpenOBI method and tools by atc1441 and contributors. Your gateway, your responsibility:")
    say("the update itself is brick-safe (a bad image is rejected), but it replaces the vendor firmware.")
    if not yes("Continue?"):
        return

    ensure_deps()
    sys.path.insert(0, HERE)

    step(1, "Firmware")
    fw = os.path.abspath(a.firmware) if a.firmware else download_firmware(a.repo, HERE)
    check_image(fw)

    step(2, "Find the gateway over Bluetooth")
    address, ble_name = pick_gateway(a.address)

    step(3, "TEA key")
    key = get_key(a.key, ble_name)
    say("Key OK.")

    step(4, "Certificates for this computer")
    from gen_certs import lan_ip
    host = a.host or ask("This computer's IP in your network", lan_ip())
    subprocess.check_call([sys.executable, os.path.join(HERE, "gen_certs.py"), "--host", host, "--port", str(a.port)], cwd=HERE)
    cfg = os.path.join(PKI, "ble_config.json")
    if not os.path.exists(cfg):
        die("gen_certs.py did not create pki/ble_config.json")

    step(5, "Start the temporary update server")
    log_path = os.path.join(HERE, "install_server.log")
    srv = Server(fw, a.port, log_path)
    time.sleep(2)
    if not srv.alive():
        die(f"update server did not start, see {log_path} (port {a.port} in use?)")
    say(f"Server running on {host}:{a.port} (log: {log_path}).")
    say("macOS / Windows may ask whether Python may accept incoming connections -- allow it.")

    try:
        step(6, "Send WiFi + server to the gateway over Bluetooth")
        ssid = a.ssid or ask("WiFi name (2.4 GHz)")
        pw = a.wifi_password if a.wifi_password is not None else ask("WiFi password", secret=True)
        if len(pw) > 32:
            say("Warning: the stock firmware only handles WiFi passwords up to 32 characters.")
        cmd = [sys.executable, os.path.join(HERE, "ble_provision.py"), "--config", cfg, "--key", key, "--unbind",
               "--address", address, "--ssid", ssid, "--password", pw]
        if a.no_pair_reader:
            cmd.append("--no-pair-sensor")
        else:
            say("The meter reader can be paired now. If asked, press the button on the reader once.")
        rc = subprocess.call(cmd, cwd=HERE)
        if rc != 0:
            die("Bluetooth setup failed. Hold the gateway button ~5 s to reopen Bluetooth and run the script again.")

        step(7, "Waiting for the gateway to download ObiUnlocked")
        say("This takes a few minutes. Keep this computer awake and the gateway powered.")
        t0, last = time.time(), None
        while not srv.done:
            if not srv.alive():
                die(f"update server stopped, see {log_path}")
            if time.time() - t0 > a.timeout:
                die(f"no complete download after {a.timeout} s, see {log_path}")
            state = (f"{srv.percent}%" if srv.percent >= 0 else
                     "gateway connected, waiting for it to start the download" if srv.connected else
                     "waiting for the gateway to join WiFi and connect")
            if state != last:
                say(f"  {state}")
                last = state
            time.sleep(1)
        say("  100% -- download complete, the gateway now checks the image and reboots.")
        time.sleep(15)
    except KeyboardInterrupt:
        die("aborted")
    finally:
        srv.stop()

    say("\n\033[1mDone.\033[0m Next:")
    say("  1. Join the WiFi hotspot  ObiUnlocked-XXXXXX  and open  http://192.168.4.1")
    say("  2. Enter your WiFi (and MQTT if you use Home Assistant); the dashboard is then at the gateway's IP.")
    say("  3. Right away: Debug -> 'Dump full flash' to keep a backup of the original firmware.")
    say("  4. Meter reader not showing? Power meters -> 'Bind all readers for 3 min' and press the reader's button.")
    say("If the hotspot doesn't appear within ~2 minutes, check " + log_path)


if __name__ == "__main__":
    main()
