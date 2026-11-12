"""Check the wireless M-Bus decoder against an independent Python encoder.

Frames are built here from scratch (CRC, 3-out-of-6, frame format A/B, AES-CBC mode 5 and mode 7 with
pycryptodome's AES + CMAC) and fed to the C++ decoder through tests/wmbus_native.cpp.
"""

import os
from pathlib import Path
import shutil
import subprocess

from Crypto.Cipher import AES
from Crypto.Hash import CMAC
import pytest

ROOT = Path(__file__).resolve().parents[1]
KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")


def _openssl_flags():
    for prefix in ("/opt/homebrew/opt/openssl", "/usr/local/opt/openssl"):
        if Path(prefix, "include/openssl/aes.h").exists():
            return ["-I", f"{prefix}/include", "-L", f"{prefix}/lib"]
    return []


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("wmbus") / "native"
    subprocess.run([
        os.environ.get("CXX", shutil.which("g++") or "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-Wno-deprecated-declarations",            # OpenSSL 3 marks the low-level AES API deprecated
        "-fsanitize=undefined,address", "-g", *_openssl_flags(),
        "-I", str(ROOT / "include"), str(ROOT / "src/wmbus_core.cpp"),
        str(ROOT / "tests/wmbus_native.cpp"), "-lcrypto", "-o", str(binary),
    ], check=True)
    return binary


def run(native, *args):
    out = subprocess.check_output([str(native), *args], text=True)
    lines = out.strip().splitlines()
    info, recs = {}, {}
    for line in lines:
        if line.startswith("rec "):
            _, key, *fields = line.split()
            recs[key] = dict(f.split("=", 1) for f in fields)
        else:
            info.update(dict(f.split("=", 1) for f in line.split()))
    return info, recs


# ---------------------------------------------------------------- independent encoder
def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x3D65) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return (~crc & 0xFFFF).to_bytes(2, "big")


def frame_a(body):
    """body = L-field-less frame (C .. end); returns frame format A bytes with block CRCs."""
    full = bytes([len(body)]) + body
    out = full[:10] + crc16(full[:10])
    rest = full[10:]
    for i in range(0, len(rest), 16):
        blk = rest[i:i + 16]
        out += blk + crc16(blk)
    return out


def frame_b(body):
    l_field = len(body) + 2 if len(body) + 1 + 2 <= 128 else len(body) + 4
    full = bytes([l_field]) + body
    if len(full) + 2 <= 128:
        return full + crc16(full)
    return full[:126] + crc16(full[:126]) + full[126:] + crc16(full[126:])


ENC3OF6 = [0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13, 0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29]


def encode_3of6(data):
    bits = "".join(f"{ENC3OF6[b >> 4]:06b}{ENC3OF6[b & 15]:06b}" for b in data)
    bits += "0" * ((-len(bits)) % 8)
    raw = int(bits, 2).to_bytes(len(bits) // 8, "big")
    return raw + bytes(20)  # trailing noise the radio would also deliver (0x00 is no valid 3-of-6 code)


def link_header(mfr="MIN", ident=0x12345678, ver=0x01, dev_type=0x08, c=0x44):
    m = ((ord(mfr[0]) - 64) << 10) | ((ord(mfr[1]) - 64) << 5) | (ord(mfr[2]) - 64)
    return bytes([c]) + m.to_bytes(2, "little") + ident.to_bytes(4, "little") + bytes([ver, dev_type])


HCA_RECORDS = bytes.fromhex(
    "0b6e563412"      # BCD6 HCA units = 123456
    "426c3f3c"        # storage 1, date type G = 2025-12-31
    "4b6e100000"      # storage 1, BCD6 HCA = 10
    "02fd170000"      # int16 error flags = 0
    "0c1378563412"    # BCD8 volume 12345678 * 10^-3 m3
)


def mode5(records, acc=0x2A, key=KEY, hdr=None, status=0x00):
    hdr = hdr or link_header()
    plain = b"\x2f\x2f" + records
    plain += b"\x2f" * ((-len(plain)) % 16)
    blocks = len(plain) // 16
    cw = (5 << 8) | (blocks << 4)
    iv = hdr[1:9] + bytes([acc]) * 8
    enc = AES.new(key, AES.MODE_CBC, iv).encrypt(plain)
    return hdr + bytes([0x7A, acc, status]) + cw.to_bytes(2, "little") + enc


def mode7(records, mcr=0x00000102, acc=0x11, key=KEY, ident=0x87654321):
    hdr = link_header(mfr="ZRI", ident=ident, dev_type=0x07)
    plain = b"\x2f\x2f" + records
    plain += b"\x2f" * ((-len(plain)) % 16)
    blocks = len(plain) // 16
    kdf = bytes([0x00]) + mcr.to_bytes(4, "little") + ident.to_bytes(4, "little") + b"\x07" * 7
    kenc = CMAC.new(key, ciphermod=AES).update(kdf).digest()
    enc = AES.new(kenc, AES.MODE_CBC, bytes(16)).encrypt(plain)
    fcl = 0x2000 | 0x0800 | 0x0400              # MCL + MCR + MAC present (MAC content is not checked)
    afl = fcl.to_bytes(2, "little") + bytes([0x25]) + mcr.to_bytes(4, "little") + bytes(8)
    cw = (7 << 8) | (blocks << 4)
    return hdr + bytes([0x90, len(afl)]) + afl + bytes([0x7A, acc, 0x00]) + cw.to_bytes(2, "little") + enc


# ---------------------------------------------------------------- tests
def test_cmac_rfc4493(native):
    key = "2b7e151628aed2a6abf7158809cf4f3c"
    assert run_raw(native, "cmac", key, "") == "bb1d6929e95937287fa37d129b756746"
    assert run_raw(native, "cmac", key, "6bc1bee22e409f96e93d7e117393172a") == "070a16b46b4d4144f79bdd9dd04a287c"
    msg = ("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
           "30c81c46a35ce411")
    assert run_raw(native, "cmac", key, msg) == "dfa66747de9ae63030ca32611497c827"


def run_raw(native, *args):
    return subprocess.check_output([str(native), *args], text=True).strip()


def test_t1_plain(native):
    body = link_header() + bytes([0x78]) + HCA_RECORDS
    info, recs = run(native, "radio", encode_3of6(frame_a(body)).hex())
    assert info["rr"] == "0" and info["mode"] == "T1"
    assert info["mfr"] == "MIN" and info["id"] == "12345678" and info["typekey"] == "hca"
    assert info["decrypt"] == "plain"
    assert float(recs["hca"]["value"]) == 123456
    assert float(recs["hca_s1"]["value"]) == 10
    assert recs["date_s1"]["date"] == "2025-12-31"
    assert float(recs["error_flags"]["value"]) == 0
    assert float(recs["volume"]["value"]) == pytest.approx(12345.678)


def test_c1_frame_a_mode5(native):
    raw = bytes([0x54, 0xCD]) + frame_a(mode5(HCA_RECORDS)) + bytes(30)
    info, recs = run(native, "radio", raw.hex(), KEY.hex())
    assert info["mode"] == "C1a" and info["sec"] == "5" and info["decrypt"] == "decrypted"
    assert float(recs["hca"]["value"]) == 123456


def test_c1_frame_b_mode5_wrong_and_missing_key(native):
    raw = bytes([0x54, 0x3D]) + frame_b(mode5(HCA_RECORDS))
    info, _ = run(native, "radio", raw.hex())
    assert info["mode"] == "C1b" and info["decrypt"] == "no_key"
    info, _ = run(native, "radio", raw.hex(), "ff" * 16)
    assert info["decrypt"] == "wrong_key"
    info, recs = run(native, "radio", raw.hex(), KEY.hex())
    assert info["decrypt"] == "decrypted" and float(recs["hca"]["value"]) == 123456


def test_long_frame_b_two_crcs(native):
    records = b"".join(bytes([0x04, 0x13]) + (1000 + i).to_bytes(4, "little") for i in range(25))
    body = link_header(dev_type=0x07) + bytes([0x78]) + records
    assert len(body) + 4 + 1 > 128
    info, recs = run(native, "radio", (bytes([0x54, 0x3D]) + frame_b(body)).hex())
    assert info["rr"] == "0" and info["records"] == "25"  # 25 records, no TPL -> no status
    assert float(recs["volume"]["value"]) == pytest.approx(1.024)   # same key 25x: the last record wins here


def test_t1_mode7_afl(native):
    records = bytes.fromhex("0413e8030000" "02fd170000")
    raw = encode_3of6(frame_a(mode7(records)))
    info, recs = run(native, "radio", raw.hex(), KEY.hex())
    assert info["mfr"] == "ZRI" and info["sec"] == "7" and info["decrypt"] == "decrypted"
    assert float(recs["volume"]["value"]) == pytest.approx(1.0)


def test_crc_error_rejected(native):
    raw = bytearray(bytes([0x54, 0xCD]) + frame_a(link_header() + bytes([0x78]) + HCA_RECORDS))
    raw[14] ^= 0x01
    info, _ = run(native, "radio", bytes(raw).hex())
    assert info["rr"] == "-3"


def test_long_tpl_identity(native):
    meter = link_header(mfr="TCH", ident=0x11223344, ver=0x45, dev_type=0x07)   # C M A ver type
    tpl = bytes([0x72]) + meter[3:7] + meter[1:3] + meter[7:9] + bytes([0x05, 0x00, 0x00, 0x00])
    body = link_header(mfr="MIN", ident=0x99999999, dev_type=0x37) + tpl + bytes.fromhex("0413e8030000")
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["mfr"] == "TCH" and info["id"] == "11223344" and info["type"] == "7"
    assert float(recs["volume"]["value"]) == pytest.approx(1.0)


def test_manufacturer_specific_tail_and_garbage(native):
    body = link_header(dev_type=0x1A) + bytes([0x7A, 0x01, 0x00, 0x00, 0x00]) + bytes.fromhex("02fd1701000fdeadbeef")
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["typekey"] == "smoke" and recs["mfr_data"]["hex"] == "deadbeef"
    assert float(recs["error_flags"]["value"]) == 1
    run(native, "radio", os.urandom(255).hex())  # must not crash on noise (ASan/UBSan enabled)


# ---------------------------------------------------------------- manufacturer formats + S1
def encode_manchester(data, invert=False):
    bits = "".join(f"{b:08b}" for b in data)
    chips = "".join(("01" if c == "1" else "10") if invert else ("10" if c == "1" else "01") for c in bits)
    raw = int(chips, 2).to_bytes(len(chips) // 8, "big")
    return raw + bytes(8)   # 0x00 = invalid chip pairs, like the noise after a real frame


@pytest.mark.parametrize("invert", [False, True])
def test_s1_manchester(native, invert):
    body = link_header() + bytes([0x78]) + HCA_RECORDS
    info, recs = run(native, "s1", encode_manchester(frame_a(body), invert).hex())
    assert info["rr"] == "0" and info["mode"] == "S1"
    assert float(recs["hca"]["value"]) == 123456


def test_izar_reference_telegram(native):
    # reference telegram + expected values from the wmbusmeters izar driver documentation
    body = bytes.fromhex("44304C72242421D401A2013D4013DD8B46A4999C1293E582CC")
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["mfr"] == "SAP" and info["id"] == "21242472" and info["mfrdec"] == "izar"
    assert float(recs["volume"]["value"]) == pytest.approx(3.488)
    assert float(recs["volume_s1"]["value"]) == pytest.approx(3.486)
    assert recs["date_s1"]["date"] == "2019-09-30"
    assert float(recs["battery_years"]["value"]) == 14.5
    assert float(recs["transmit_period"]["value"]) == 8


def test_techem_water_mkradio3(native):
    body = bytes.fromhex("4468503132333474" "62A2069F255900B029310000000306060906030609070606050509050505050407040605070500")
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["mfr"] == "TCH" and info["mfrdec"] == "techem_water"
    assert float(recs["volume"]["value"]) == pytest.approx(13.8)
    assert float(recs["volume_s1"]["value"]) == pytest.approx(8.9)
    assert recs["date_s1"]["date"] == "2018-12-31"   # 0x259F: same date packing as the FHKV test below


def test_techem_hca_fhkv3(native):
    body = bytes.fromhex("446850226677116980A0119F27020480048300C408F709143C003D341A2B0B2A0707000000000000062D114457563D71A1850000")
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["mfrdec"] == "techem_hca" and info["id"] == "11776622"
    assert float(recs["hca"]["value"]) == 131
    assert float(recs["hca_s1"]["value"]) == 1026
    assert recs["date"]["date"] == "2020-02-08" and recs["date_s1"]["date"] == "2019-12-31"
    assert float(recs["room_temp"]["value"]) == pytest.approx(22.44)
    assert float(recs["radiator_temp"]["value"]) == pytest.approx(25.51)


# ---------------------------------------------------------------- interpretation layer
QDS_REAL = ("4944934441206813180778"   # Qundis water meter seen in the field (L C M ID ver type CI=78)
            "0DFF5F35" "00822C0000F10007C113FFFF054510003F3C376808005F3A173710009F00AB00A9009000900093"
            "00B000C200C100AB00A900B5002F" "046D03114C3B")


def test_qundis_walkby_real_telegram(native):
    body = bytes.fromhex(QDS_REAL)[1:]
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["mfr"] == "QDS" and info["id"] == "13682041"
    assert float(recs["volume"]["value"]) == pytest.approx(104.505)
    assert float(recs["volume_s1"]["value"]) == pytest.approx(86.837) and recs["date_s1"]["date"] == "2025-12-31"
    assert float(recs["volume_s2"]["value"]) == pytest.approx(103.717) and recs["date_s2"]["date"] == "2026-10-31"
    assert float(recs["volume_since_due"]["value"]) == pytest.approx(17.668)
    assert recs["datetime"]["datetime"] == "2026-11-12T17:03"
    assert "mfr_data" not in recs and "date_err" not in recs


def test_qundis_walkby_reference_blob(native):
    blob = ("0082da0000600107c113ffff48200000bf2c91170000df2120200000008001000000060019001000160018000d"
            "001300350017002f")
    body = link_header(mfr="QDS", ident=0x12353648, ver=0x18, dev_type=0x07) + bytes([0x78]) + bytes.fromhex("0dff5f35" + blob)
    _, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert float(recs["volume"]["value"]) == pytest.approx(2.048)
    assert float(recs["volume_s1"]["value"]) == pytest.approx(1.791) and recs["date_s1"]["date"] == "2021-12-31"
    assert float(recs["volume_s2"]["value"]) == pytest.approx(2.020) and recs["date_s2"]["date"] == "2022-01-31"


def test_smoke_detector_ei_head_and_labels(native):
    head = (0x0F << 8) | 5                   # battery nibble 0xF -> 3.00 V, dust 5
    recs_hex = ("8440FF2C" + head.to_bytes(4, "little").hex() +
                "82106C3F3C"                 # tariff 1 date (last alarm) 2025-12-31
                "8310FD6102000000")          # tariff 1 cumulation counter (alarms) = 2
    body = link_header(mfr="EIE", ident=0x01097274, ver=0x0C, dev_type=0x1A) + bytes([0x7A, 0x01, 0x04, 0x00, 0x00]) + bytes.fromhex(recs_hex)
    info, recs = run(native, "radio", (bytes([0x54, 0xCD]) + frame_a(body)).hex())
    assert info["typekey"] == "smoke"
    assert float(recs["battery_voltage"]["value"]) == pytest.approx(3.0)
    assert float(recs["dust_level"]["value"]) == 5 and recs["head_status"]["text"] == "OK"
    assert recs["last_alarm"]["date"] == "2025-12-31" and float(recs["alarms"]["value"]) == 2
    assert recs["status"]["text"] == "Batterie_schwach"      # TPL status 0x04 = power low
