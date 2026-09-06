#!/usr/bin/env python3
"""二进制协议专项验证:覆盖全部 OP 码,重点 UART 校验位 + SPI 大包往返。"""
import struct
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SOF, T_CMD, T_RESP, T_ERR, T_EVT, T_DATA, T_DATA_END = 0xCB, 1, 2, 3, 4, 5, 6


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


class B:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=1)
        time.sleep(0.1)
        self.s.reset_input_buffer()
        self.s.write(b"BIN ENTER\n")
        time.sleep(0.3)
        # 确认进入二进制模式(读掉 "OK BIN")
        self.s.reset_input_buffer()

    def frame(self, op, payload=b"", seq=0):
        hdr = bytes([SOF, T_CMD, op, 0]) + struct.pack("<HH", len(payload), seq)
        body = hdr + payload
        return body + struct.pack("<H", crc16(body))

    def read(self, timeout=2.0):
        deadline = time.time() + timeout
        while True:
            if time.time() > deadline:
                return None
            if self.s.read(1) != b"\xcb":
                continue
            hdr = self.s.read(7)
            if len(hdr) != 7:
                return None
            ln = struct.unpack("<H", hdr[3:5])[0]
            data = self.s.read(ln)
            crc = self.s.read(2)
            got = crc16(hdr[:3] + struct.pack("<H", ln) + hdr[5:7] + data)
            # 重新计算帧 CRC(SOF+TYPE+OP+FLAGS+LEN+SEQ+payload)
            full = bytes([SOF]) + hdr[0:3] + struct.pack("<H", ln) + hdr[5:7] + data
            assert crc16(full) == struct.unpack("<H", crc)[0], "CRC mismatch!"
            return hdr[0], hdr[1], data

    def cmd(self, op, payload=b""):
        self.s.reset_input_buffer()
        self.s.write(self.frame(op, payload))
        deadline = time.time() + 2.5
        while time.time() < deadline:
            f = self.read()
            if f is None:
                break
            typ, op2, data = f
            if typ == T_RESP and op2 == op:
                return data
            if typ == T_ERR and op2 == op:
                raise AssertionError(f"bin ERR op=0x{op:02X}: {data.hex()}")
        raise AssertionError(f"no binary response for op=0x{op:02X}")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM91"
    b = B(port)
    ok = fail = 0

    def chk(name, cond, extra=""):
        nonlocal ok, fail
        if cond:
            ok += 1
            print(f"  PASS  {name} {extra}")
        else:
            fail += 1
            print(f"  FAIL  {name} {extra}")

    # --- 系统 ---
    d = b.cmd(0x00)  # PING
    chk("PING", d[0] == 0x4F and tuple(d[1:4]) == (0, 1, 4), d.hex())
    d = b.cmd(0x01)  # INFO
    chk("INFO", b"RP2040" in d, "")
    b.cmd(0x04, b"\x01"); b.cmd(0x04, b"\x00")  # LED ON/OFF

    # --- IO 全操作 ---
    b.cmd(0x10, bytes([0, 0, 0]))      # CFG IO1 OUT
    b.cmd(0x11, bytes([0, 1]))         # WRITE HIGH
    d = b.cmd(0x12, bytes([0]))        # READ
    chk("IO_READ", d[0] == 0 and d[1] == 1, d.hex())
    b.cmd(0x14, bytes([0]))            # TOGGLE
    d = b.cmd(0x12, bytes([0]))
    chk("IO_TOGGLE", d[1] == 0, d.hex())
    b.cmd(0x15, bytes([0, 1]) + struct.pack("<H", 20))  # PULSE ch0 HIGH 20ms
    d = b.cmd(0x13)                    # READALL bitmask
    chk("IO_READALL", isinstance(d, bytes) and len(d) >= 1, "")

    # --- PWM 全操作 ---
    b.cmd(0x20, bytes([0]) + struct.pack("<I", 1000) + struct.pack("<H", 32767))
    d = b.cmd(0x26, bytes([0]))
    chk("PWM_READ", struct.unpack("<I", d[1:5])[0] == 1000, d.hex())
    b.cmd(0x21, bytes([0]) + struct.pack("<I", 2000))  # FREQ
    b.cmd(0x22, bytes([0]) + struct.pack("<H", 16384))  # DUTY
    b.cmd(0x23, bytes([0]))  # START
    b.cmd(0x24, bytes([0]))  # STOP
    b.cmd(0x25, struct.pack("<BHHHH", 0, 0, 65535, 5, 0))  # SWEEP
    b.cmd(0x24, bytes([0]))  # STOP

    # --- UART 全操作 + 校验位(重点) ---
    for parity in (0, 1, 2):  # NONE / ODD / EVEN
        d = b.cmd(0x30, bytes([0]) + struct.pack("<I", 115200) + bytes([8, parity, 1]))
        chk(f"UART_CFG parity={parity}", len(d) == 0, "")
    d = b.cmd(0x31, bytes([0]) + struct.pack("<H", 5) + b"Hello")  # TX
    chk("UART_TX", struct.unpack("<H", d)[0] == 5, d.hex())
    d = b.cmd(0x32, bytes([0]) + struct.pack("<H", 64))  # RX
    n = struct.unpack("<H", d[1:3])[0]
    chk("UART_RX", d[0] == 0 and len(d) == 3 + n, f"n={n} (RX 悬空可能读到噪声字节)")
    b.cmd(0x33, bytes([0]))  # FLUSH
    b.cmd(0x34, bytes([0, 1])); b.cmd(0x34, bytes([0, 0]))  # STREAM on/off
    d = b.cmd(0x35, bytes([0]))  # STAT
    chk("UART_STAT", len(d) == 8, d.hex())

    # --- SPI 全操作 + 大包往返(重点) ---
    b.cmd(0x50, struct.pack("<IBBB", 1000000, 0, 1, 8))  # CFG 1MHz mode0 MSB 8bit
    big = bytes(range(256)) * 2  # 512 字节
    d = b.cmd(0x51, struct.pack("<H", len(big)) + big)  # XFR
    chk("SPI_XFR 512B", len(d) == 2 + 512, f"len={len(d)} (MISO 悬空,内容不校验)")
    b.cmd(0x52, struct.pack("<H", 4) + b"\xde\xad\xbe\xef")  # WRITE
    d = b.cmd(0x53, struct.pack("<H", 256))  # READ 256
    chk("SPI_READ 256B", len(d) == 2 + 256, f"len={len(d)}")
    b.cmd(0x54, bytes([1])); b.cmd(0x54, bytes([0]))  # CS low / auto

    # --- I2C ---
    b.cmd(0x40, struct.pack("<I", 400))  # CFG
    d = b.cmd(0x41)  # SCAN
    chk("I2C_SCAN", len(d) >= 1 and d[0] >= 0, f"found={d.hex()}")

    # --- ADC ---
    d = b.cmd(0x60, bytes([0]))  # READ
    raw, mv = struct.unpack("<HH", d[1:5])
    chk("ADC_READ", 0 <= raw <= 4095 and 0 <= mv <= 3300, f"raw={raw} mv={mv}")
    d = b.cmd(0x61)  # READALL
    chk("ADC_READALL", len(d) == 12, f"len={len(d)}")
    d = b.cmd(0x64)  # TEMP
    chk("ADC_TEMP", -50000 < struct.unpack("<i", d)[0] < 150000,
        f"{struct.unpack('<i', d)[0] / 1000.0:.2f}°C")
    b.cmd(0x65, struct.pack("<BHHB", 0, 500, 2500, 1))  # THRESH on
    b.cmd(0x65, struct.pack("<BHHB", 0, 0, 0, 0))       # THRESH off

    # --- SEQ ---
    script = b"IO IO1 LOW@0; WAIT 50"
    b.cmd(0x70, bytes([5]) + b"blink" + struct.pack("<H", len(script)) + script)  # DEF
    d = b.cmd(0x72, bytes([5]) + b"blink")  # SHOW
    chk("SEQ_SHOW", len(d) >= 2 and d[2:] == script, d.hex())
    b.cmd(0x73, bytes([5]) + b"blink" + struct.pack("<H", 1))  # RUN once
    time.sleep(0.5)
    d = b.cmd(0x75)  # STAT
    chk("SEQ_STAT", d[0] == 0, f"state={d[0]}")
    b.cmd(0x76, bytes([5]) + b"blink")  # DEL

    # --- 退出 ---
    b.cmd(0x0F)  # MODE_EXIT
    b.s.reset_input_buffer()
    b.s.write(b"PING\n")
    time.sleep(0.4)
    r = b.s.read(64)
    chk("MODE_EXIT->文本", b"PONG" in r, r.decode(errors="replace").strip())
    b.s.close()

    print(f"\n==== 二进制专项: {ok} 通过, {fail} 失败 ====")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
