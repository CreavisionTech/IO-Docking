#!/usr/bin/env python3
"""全自动烧录 IO Dock。

流程:检测端口 -> 发送 BOOTLOADER(若板卡遗留二进制模式则先发二进制 MODE_EXIT)
     -> 等待 RPI-RP2 盘 -> 写入 uf2 并刷盘 -> 等待重启。

用法:
    python flash_auto.py [COMxx]     # 不指定端口则自动按 VID:PID=1209:8888 识别
"""
import os
import shutil
import string
import struct
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

import serial
import serial.tools.list_ports

UF2 = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "io_dock.uf2")


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def find_dock_port():
    for p in serial.tools.list_ports.comports():
        hwid = getattr(p, "hwid", "") or ""
        if "1209" in hwid and "8888" in hwid:
            return p.device
    return None


def find_rp2_drive():
    for letter in string.ascii_uppercase:
        p = letter + ":\\"
        try:
            info = p + "INFO_UF2.TXT"
            if os.path.isfile(info):
                with open(info, encoding="utf-8", errors="replace") as f:
                    if "RPI-RP2" in f.read():
                        return p
        except OSError:
            pass
    return None


def wait_for_rp2(timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        d = find_rp2_drive()
        if d:
            return d
        time.sleep(0.25)
    return None


def enter_bootloader(port):
    """尝试让板卡进入 bootrom。先按文本模式发 BOOTLOADER;若无效则视为二进制模式,
    先发二进制 MODE_EXIT 退回文本再发 BOOTLOADER。"""
    # 1) 文本模式直接发
    s = serial.Serial(port, 115200, timeout=1)
    s.reset_input_buffer()
    s.write(b"BOOTLOADER\n")
    time.sleep(0.4)
    s.close()
    if wait_for_rp2(3):
        return True

    # 2) 二进制模式:发 MODE_EXIT 后再发文本 BOOTLOADER
    s = serial.Serial(port, 115200, timeout=1)
    s.reset_input_buffer()
    hdr = bytes([0xCB, 0x01, 0x0F, 0x00]) + struct.pack("<HH", 0, 0)
    s.write(hdr + struct.pack("<H", crc16(hdr)))
    time.sleep(0.4)
    s.write(b"BOOTLOADER\n")
    time.sleep(0.4)
    s.close()
    return wait_for_rp2(60) is not None


def validate_uf2(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    total = len(data) // 512
    for i in range(total):
        block = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, number, count, family = struct.unpack_from("<8I", block)
        if (m0, m1, flags, size, number, count, family) != (0x0A324655, 0x9E5D5157, 0x2000, 256, i, total, 0xE48BFF56):
            raise ValueError(f"Invalid RP2040 UF2 block {i}")
        if addr != 0x10000000 + i * 256 or addr + size > 0x107FF000:
            raise ValueError("UF2 would overwrite configuration or an invalid address")
        if struct.unpack_from("<I", block, 508)[0] != 0x0AB16F30:
            raise ValueError(f"Invalid UF2 trailer {i}")
    return data


def main():
    # Validate the full image before resetting the connected board.
    data = validate_uf2(UF2)
    port = sys.argv[1] if len(sys.argv) > 1 else find_dock_port()
    if not port:
        print("未找到 IO Dock 串口(VID:PID=1209:8888),请用参数指定 COM 口")
        sys.exit(1)
    print(f"[1/4] 连接 {port} 并发送 BOOTLOADER ...")
    if not enter_bootloader(port):
        print("      未能进入 bootrom,请确认板卡连接正常")
        sys.exit(1)

    print("[2/4] 等待 RPI-RP2 盘 ...")
    drive = wait_for_rp2(60)
    if not drive:
        print("      未找到 RPI-RP2 盘")
        sys.exit(1)
    print(f"      盘: {drive}")

    print("[3/4] 写入 uf2 并刷盘 ...")
    dst = drive + "io_dock.uf2"
    with open(dst, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    print(f"      已写入 {dst} ({len(data)} 字节)")

    print("[4/4] 等待重启 ...")
    time.sleep(3)
    ports = [p.device for p in serial.tools.list_ports.comports()
             if "1209" in getattr(p, "hwid", "") and "8888" in getattr(p, "hwid", "")]
    print(f"      完成。当前 IO Dock 串口: {ports}")
    print("DONE")


if __name__ == "__main__":
    main()
