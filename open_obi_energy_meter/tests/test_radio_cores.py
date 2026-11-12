"""Weather-sensor and LoRaWAN cores against reference vectors and an independent Python encoder."""

import os
from pathlib import Path
import shutil
import subprocess

from Crypto.Cipher import AES
from Crypto.Hash import CMAC
import pytest

ROOT = Path(__file__).resolve().parents[1]


def _openssl_flags():
    for prefix in ("/opt/homebrew/opt/openssl", "/usr/local/opt/openssl"):
        if Path(prefix, "include/openssl/aes.h").exists():
            return ["-I", f"{prefix}/include", "-L", f"{prefix}/lib"]
    return []


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("radio") / "native"
    subprocess.run([
        os.environ.get("CXX", shutil.which("g++") or "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-Wno-deprecated-declarations", "-fsanitize=undefined,address", "-g", *_openssl_flags(),
        "-I", str(ROOT / "include"), str(ROOT / "src/wmbus_core.cpp"), str(ROOT / "src/wx_core.cpp"),
        str(ROOT / "src/lorawan_core.cpp"), str(ROOT / "tests/radio_native.cpp"), "-lcrypto", "-o", str(binary),
    ], check=True)
    return binary


def run(native, *args):
    out = subprocess.check_output([str(native), *map(str, args)], text=True)
    res = {}
    for line in out.splitlines():
        if line.startswith("lpp "):
            k, v = line[4:].split("=", 1)
            res["lpp_" + k] = float(v.split()[0])
            continue
        for f in line.split():
            k, v = f.split("=", 1)
            res[k] = v
    return res


def crc8(data, poly=0x31, init=0):
    c = init
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def lfsr_digest16(data, gen, key):
    s = 0
    for byte in data:
        for i in range(7, -1, -1):
            if (byte >> i) & 1:
                s ^= key
            key = (key >> 1) ^ gen if key & 1 else key >> 1
    return s


# ---------------------------------------------------------------- weather sensors
def test_lacrosse(native):
    msg = bytes.fromhex("9284486a")            # rtl_433 example: id 10, 4.8 C, no humidity sensor
    r = run(native, "wx", "lacrosse", (msg + bytes([crc8(msg)])).hex())
    assert r["ok"] == "1" and r["id"] == "10" and float(r["temp"]) == pytest.approx(4.8) and "hum" not in r
    msg = bytes([0x90 | 0x3, (0x2 << 6) | 0x20 | 0x6, 0x25, 0x80 | 55])   # id 14, new batt, 62.5-40=22.5 C, weak, 55 %
    r = run(native, "wx", "lacrosse", (msg + bytes([crc8(msg)])).hex())
    assert r["id"] == "14" and r["newbatt"] == "1" and float(r["temp"]) == 22.5 and r["hum"] == "55" and r["battlow"] == "1"
    assert run(native, "wx", "lacrosse", (msg + bytes([crc8(msg) ^ 1])).hex())["ok"] == "0"


def test_bresser5_reference(native):
    msg = bytes.fromhex("ec91ffff1ffbefe7feadedfff7136e0000e004101801521200080000")[:26]
    r = run(native, "wx", "bresser", msg.hex())
    assert r["ok"] == "1" and r["proto"] == "2" and r["id"] == str(0x6E)
    assert float(r["temp"]) == pytest.approx(-11.8) and r["hum"] == "52"
    assert float(r["wavg"]) == pytest.approx(0.4) and float(r["wdir"]) == 315 and float(r["rain"]) == pytest.approx(1.2)
    assert r["battlow"] == "0"


def test_bresser6_reference(native):
    r = run(native, "wx", "bresser", "5eaa188002c318fa8ffb27681184 81fff07200".replace(" ", ""))
    assert r["ok"] == "1" and r["proto"] == "3" and r["id"] == str(0x188002C3)
    assert float(r["temp"]) == pytest.approx(11.8) and r["hum"] == "81"


def test_bresser7_synthetic(native):
    m = bytearray(25)
    m[2:4] = b"\x12\x34"
    m[4], m[5] = 0x27, 0x00                    # wind dir 270
    m[6] = 0x10                                # type 1, ch 0 (raw byte, before whitening)
    m[7], m[8], m[9] = 0x01, 0x20, 0x08        # gust 1.2, avg 0.8
    m[10:13] = b"\x00\x01\x23"                 # rain 12.3
    m[14], m[15] = 0x21, 0x50                  # 21.5 C
    m[16] = 0x45                               # 45 %
    m[17:20] = b"\x01\x23\x45"                 # 12345 lux
    m[20], m[21] = 0x01, 0x20                  # UV 1.2
    w = bytearray(b ^ 0xAA for b in m)
    w[6] = m[6]                                # type nibble is read from the raw byte
    m2 = bytearray(b ^ 0xAA for b in w)        # what the decoder sees after whitening
    d = lfsr_digest16(m2[2:25], 0x8810, 0xBA95) ^ 0x6DF1
    w[0], w[1] = ((d >> 8) ^ 0xAA), ((d & 0xFF) ^ 0xAA)
    r = run(native, "wx", "bresser", bytes(w).hex())
    assert r["ok"] == "1" and r["proto"] == "4" and r["id"] == str(0x1234)
    assert float(r["temp"]) == 21.5 and r["hum"] == "45" and float(r["rain"]) == pytest.approx(12.3)
    assert float(r["wdir"]) == 270 and float(r["wgust"]) == pytest.approx(1.2) and r["lux"] == "12345"


def test_bresser_noise_rejected(native):
    for _ in range(20):
        assert run(native, "wx", "bresser", os.urandom(26).hex())["ok"] == "0"


# ---------------------------------------------------------------- LoRaWAN
NWK = bytes.fromhex("2B7E151628AED2A6ABF7158809CF4F3C")
APP = bytes.fromhex("000102030405060708090A0B0C0D0E0F")
DEVADDR = 0x260B1234


def lw_uplink(payload, fcnt32, fport=1, fopts=b""):
    def blk(i):
        return bytes([1, 0, 0, 0, 0, 0]) + DEVADDR.to_bytes(4, "little") + fcnt32.to_bytes(4, "little") + bytes([0, i])
    key = APP if fport else NWK
    enc = bytearray(payload)
    for i in range(0, len(enc), 16):
        s = AES.new(key, AES.MODE_ECB).encrypt(blk(i // 16 + 1))
        for k in range(min(16, len(enc) - i)):
            enc[i + k] ^= s[k]
    msg = bytes([0x40]) + DEVADDR.to_bytes(4, "little") + bytes([len(fopts)]) + (fcnt32 & 0xFFFF).to_bytes(2, "little") + fopts + bytes([fport]) + bytes(enc)
    b0 = bytes([0x49, 0, 0, 0, 0, 0]) + DEVADDR.to_bytes(4, "little") + fcnt32.to_bytes(4, "little") + bytes([0, len(msg)])
    mic = CMAC.new(NWK, ciphermod=AES).update(b0 + msg).digest()[:4]
    return msg + mic


def test_lorawan_decrypt_cayenne(native):
    lppdata = bytes.fromhex("0167011003680A0473271A")   # ch1 temp 27.2, ch3 hum 5 %, ch4 pressure 1001.0
    r = run(native, "lw", lw_uplink(lppdata, 0x00010005).hex(), NWK.hex(), APP.hex(), 0)   # high half 1: found by +1
    assert r["parse"] == "1" and r["devaddr"] == f"{DEVADDR:08x}" and r["fcnt32"] == str(0x00010005)
    assert r["payload"] == lppdata.hex() and r["lppcount"] == "3"
    assert r["lpp_temp_1"] == pytest.approx(27.2) and r["lpp_hum_3"] == 5 and r["lpp_pressure_4"] == pytest.approx(1001.0)


def test_lorawan_wrong_key_and_non_lpp(native):
    up = lw_uplink(b"\xde\xad\xbe\xef\x01", 7)
    assert run(native, "lw", up.hex(), APP.hex(), APP.hex(), 0)["len"] == "-1"
    r = run(native, "lw", up.hex(), NWK.hex(), APP.hex(), 0)
    assert r["payload"] == "deadbeef01" and r["lppcount"] == "-1"
