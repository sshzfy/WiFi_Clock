#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ota_pack.py — 把 App 的 .bin 打包成 .ota 固件包。

用法:
    python ota_pack.py <app.bin> <out.ota> --version 1.0.1

.ota 格式（与 Boot/Inc/ota_pkg.h 一致，全部小端）:
    +0x00  magic        'OTA1' = 0x4F544131
    +0x04  header_size  64
    +0x06  header_crc16 头部前 60 B 的 CRC16-CCITT
    +0x08  fw_version   major<<16 | minor<<8 | patch
    +0x0C  fw_size      payload 字节数（4 字节对齐）
    +0x10  fw_crc32     payload 的 CRC32（zlib）
    +0x14  hw_id        0x04074001
    +0x18  build_time   Unix 时间戳
    +0x1C  flags        保留
    +0x20  min_boot_ver 保留
    +0x24  reserved[28]
    +0x40  payload      App .bin（0xFF 填充到 4 字节对齐）
"""

import argparse
import struct
import time
import zlib

MAGIC      = 0x4F544131
HEADER_SIZE = 64
HW_ID      = 0x04074001


def crc16_ccitt(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def main():
    ap = argparse.ArgumentParser(description="Pack App .bin into .ota")
    ap.add_argument("bin", help="input .bin (Keil fromelf output)")
    ap.add_argument("out", help="output .ota")
    ap.add_argument("--version", required=True, help="major.minor.patch, e.g. 1.0.1")
    ap.add_argument("--hw-id", type=lambda x: int(x, 0), default=HW_ID)
    args = ap.parse_args()

    fw = open(args.bin, "rb").read()

    pad = (-len(fw)) % 4
    if pad:
        fw += b"\xFF" * pad

    maj, mino, pat = (int(x) for x in args.version.split("."))
    version = (maj << 16) | (mino << 8) | pat
    fw_crc = zlib.crc32(fw) & 0xFFFFFFFF

    hdr = struct.pack("<IHHIIIIIII28s",
                      MAGIC, HEADER_SIZE, 0,
                      version, len(fw), fw_crc, args.hw_id,
                      int(time.time()), 0, 0, b"\x00" * 28)

    hdr_crc = crc16_ccitt(hdr[:60])
    hdr = hdr[:6] + struct.pack("<H", hdr_crc) + hdr[8:]

    body = hdr + fw
    with open(args.out, "wb") as f:
        f.write(body)

    print(f"{args.out}: {len(fw)} B payload, crc32=0x{fw_crc:08X}, "
          f"version={args.version} (0x{version:06X})")


if __name__ == "__main__":
    main()
