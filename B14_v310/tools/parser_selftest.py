#!/usr/bin/env python3
"""Offline sanity test for the v3.4 RAW18 alignment logic.
No ESP-IDF or vehicle is required. This only verifies the parser signature/ranges.
"""
L = 18
SAMPLES = [
    bytes.fromhex("00 43 01 4C 01 56 87 09 00 AF 1E 68 2F 05 40 01 62 4C"),
    bytes.fromhex("00 43 01 4B 01 57 87 09 00 AE 1D 68 2F 05 40 01 62 4B"),
    bytes.fromhex("00 42 01 4D 01 57 87 09 00 B0 1E 66 2F 05 40 01 62 4D"),
]

def plausible(p, repeat=True):
    if len(p) != L:
        return False
    rpm = ((p[0] << 8) | p[1]) * 12.5
    inj = ((p[2] << 8) | p[3]) / 100.0
    maf = ((p[4] << 8) | p[5]) * 0.005
    ect = p[6] - 50
    o2 = p[7] * 0.01
    speed = p[8] * 2
    bat = p[9] * 0.08
    tps = p[10] * 0.02
    ign = 110 - p[11]
    aac = p[12] / 2.0
    af = p[16]
    if repeat and p[17] != p[3]:
        return False
    return (
        0 <= rpm <= 7500 and 0 <= inj <= 30 and 0.20 <= maf <= 5.50 and
        -20 <= ect <= 125 and 0 <= o2 <= 1.20 and 0 <= speed <= 240 and
        7.0 <= bat <= 16.5 and 0.20 <= tps <= 5.20 and -20 <= ign <= 60 and
        0 <= aac <= 100 and 50 <= af <= 150
    )

assert all(plausible(x, True) for x in SAMPLES)
stream = b"\x99\x88\x77\x66\x55" + b"".join(SAMPLES)
found = None
for off in range(len(stream) - L * 3 + 1):
    if all(plausible(stream[off + k*L: off + (k+1)*L], True) for k in range(3)):
        found = off
        break
assert found == 5, found
print("PASS: RAW18 alignment/signature logic locks at offset", found)
print("PASS: 3 consecutive B14 sample payloads are plausible")
