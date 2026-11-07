#!/usr/bin/env python3
"""Decode raw packets logged with `log_packets: true` into Brilliant frames.

Usage: python3 decode.py logfile.txt
Lines look like "... Packet ch 50: 7b570080015728c0..."; every frame with a valid CRC is printed
once per counter value.
"""

import re
import sys


def crc16_pl1167(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def frame_from_payload(hex_payload):
    """Bits after the address: 8 bits sync word rest, 4 bits trailer, then bytes LSB first."""
    bits = "".join(f"{b:08b}" for b in bytes.fromhex(hex_payload))[12:]
    return [int(bits[i:i + 8][::-1], 2) for i in range(0, len(bits) - 7, 8)]


def main(path):
    seen = set()
    for line in open(path, errors="replace"):
        m = re.search(r"Packet ch (\d+): ([0-9a-fA-F]{64})", line)
        if not m:
            continue
        f = frame_from_payload(m.group(2))
        if f[0] != 0x0E or crc16_pl1167(f[0:15]) != (f[15] | f[16] << 8):
            continue
        key = (tuple(f[1:6]), f[7])
        if key in seen:
            continue
        seen.add(key)
        remote = " ".join(f"{b:02X}" for b in f[1:6])
        params = " ".join(f"{b:02X}" for b in f[9:13])
        print(f"ch {m.group(1)} remote {remote} counter {f[7]:02X} command {f[8]:02X} "
              f"params {params} byte13 {f[13]:02X}")


if __name__ == "__main__":
    main(sys.argv[1])
