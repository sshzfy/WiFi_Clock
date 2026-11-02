#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ota_pack.py — 把 App 的 .bin 打包成 .ota 固件包。

用法:
    py ota_pack.py <app.bin> <out.ota> --version 1.0.1

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

    fw = open(args.bin, "rb").read() # 

    pad = (-len(fw)) % 4 # 4字节对齐所差的字节数
    if pad:
        fw += b"\xFF" * pad # 使用0xFF 来补齐4字节

    maj, mino, pat = (int(x) for x in args.version.split("."))
    version = (maj << 16) | (mino << 8) | pat
    fw_crc = zlib.crc32(fw) & 0xFFFFFFFF

    hdr = struct.pack("<IHHIIIIIII28s", # <：小端模式，I：uint32（4字节），H：uint16（2字节），28s：28字节字段
                      MAGIC, 			# MAGIC
					  HEADER_SIZE, 		# OTA 头部大小
					  0,				# 头部 CRC16结果，占位待回填
                      version, 			# 版本号
					  len(fw), 			# payload 长度（4字节对齐）
					  fw_crc, 			# payload 的 CRC32校验结果
					  args.hw_id,		# 硬件 ID
                      int(time.time()), # 打包时的 UNIX 时间戳
					  0, 				# falgs 位保留
					  0,				# min_boot_ver 位保留
					  b"\x00" * 28)		# 28字节填充位

    hdr_crc = crc16_ccitt(hdr[:60]) # 头部 CRC16校验结果
    hdr = hdr[:6] + struct.pack("<H", hdr_crc) + hdr[8:] # 回填 CRC16结果

    body = hdr + fw # 最终输出：头部+固件
    with open(args.out, "wb") as f:
        f.write(body)

    print(f"{args.out}: {len(fw)} B payload, crc32=0x{fw_crc:08X}, "
          f"version={args.version} (0x{version:06X})")


if __name__ == "__main__":
    main()
