#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ota_send.py — 通过 USART2 把 .ota 发送给设备（离线升级）。

协议（与 User/Application/Src/OTA.c 一致）:
    PC -> MCU: 0x55 0xAA 'O' 'T' 'A'           握手
    MCU -> PC: 0xAA 0x55 'O' 'K'
    PC -> MCU: 元数据 20 B <HHIIII: magic=0x55AA, len=20,
                          total_len, crc32, version, hw_id>
    MCU -> PC: 0xAA 0x55 'O' 'K'
    PC -> MCU: 裸 .ota 文件字节流
    MCU -> PC: 0xAA 0x55 'D' 'O' 'N' 'E'       成功（随后复位）
           或 0xAA 0x55 'F' 'A' 'I' 'L'       失败

依赖 pyserial:
    pip install pyserial

用法:
    python ota_send.py firmware.ota COM3 --version 1.0.1
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("需要 pyserial: pip install pyserial")
    sys.exit(1)

META_MAGIC = 0x55AA
META_LEN   = 20
HW_ID      = 0x04074001


def read_exact(ser, n, timeout_s):
    """读满 n 字节或超时"""
    ser.timeout = timeout_s
    buf = b""
    deadline = time.time() + timeout_s
    while len(buf) < n and time.time() < deadline:
        b = ser.read(n - len(buf))
        if b:
            buf += b
    return buf


def main():
    ap = argparse.ArgumentParser(description="Send .ota to device over USART2")
    ap.add_argument("ota", help=".ota file (from ota_pack.py)")
    ap.add_argument("port", help="serial port, e.g. COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--version", required=True, help="major.minor.patch, must match package")
    args = ap.parse_args()

    data = open(args.ota, "rb").read()
    if len(data) < 64:
        print("invalid .ota: too short")
        sys.exit(1)

    hdr = data[:64]
    magic, hsize, hcrc, ver, fw_size, fw_crc, hw_id = struct.unpack_from("<IHHIIII", hdr, 0)
    if magic != 0x4F544131 or hsize != 64 or hw_id != HW_ID:
        print(f"invalid .ota header: magic=0x{magic:08X} hw_id=0x{hw_id:08X}")
        sys.exit(1)

    maj, mino, pat = (int(x) for x in args.version.split("."))
    version = (maj << 16) | (mino << 8) | pat
    if version != ver:
        print(f"version mismatch: package=0x{ver:06X} args=0x{version:06X}")
        sys.exit(1)

    print(f"open {args.port} @{args.baud}")
    ser = serial.Serial(args.port, args.baud, timeout=1)
    ser.reset_input_buffer()

    print("handshake ...")
    ser.write(b"\x55\xAAOTA")
    ack = read_exact(ser, 4, 10)
    if ack != b"\xAA\x55OK":
        print(f"handshake failed: {ack!r}")
        sys.exit(1)
    print("  ok")

    print("send metadata ...")
    meta = struct.pack("<HHIIII", META_MAGIC, META_LEN,
                       len(data), fw_crc, version, hw_id)
    ser.write(meta)
    ack = read_exact(ser, 4, 10)
    if ack != b"\xAA\x55OK":
        print(f"metadata rejected: {ack!r}")
        sys.exit(1)
    print("  ok")

    print(f"send {len(data)} B ...")
    t0 = time.time()
    chunk = 4096
    for i in range(0, len(data), chunk):
        ser.write(data[i:i + chunk])
    elapsed = time.time() - t0
    print(f"  done in {elapsed:.1f}s")

    print("wait result (up to 90s: verify + backup + param) ...")
    result = read_exact(ser, 6, 90)
    if result == b"\xAA\x55DONE":
        print("SUCCESS - device will reboot into new firmware")
    elif result == b"\xAA\x55FAIL":
        print("FAILED - check device log / package")
        sys.exit(1)
    else:
        print(f"no valid response: {result!r}")
        sys.exit(1)

    ser.close()


if __name__ == "__main__":
    main()
