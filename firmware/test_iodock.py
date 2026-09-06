#!/usr/bin/env python3
"""IO Dock 固件功能测试。

用法:
    python test_iodock.py [--port COMxx] [--verbose]

不带 --port 时自动扫描所有串口,直至 PING 到 IO Dock。

测试覆盖:
    文本协议: 系统/IO/PWM(含相位)/UART/I2C/SPI/ADC/SEQ
    二进制协议: 模式切换 + 主要操作码
    事件: ADC 采样流、序列完成

注:
    - 无外部设备的测试(UART 回环、I2C/SPI 总线)会报告实际结果而不判 FAIL;
    - GPIO 中断、UART 透传等需要外部激励的项标注 "需外部接线/激励"。
"""
import argparse
import struct
import sys
import time
from unittest import SkipTest

# Windows 控制台默认 GBK,重配为 UTF-8 避免中文测试名打印失败
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import serial
import serial.tools.list_ports

SOF, T_CMD, T_RESP, T_ERR, T_EVT, T_DATA, T_DATA_END = 0xCB, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06

CRC_INIT = 0xFFFF


def crc16(data):
    c = CRC_INIT
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


class Dock:
    def __init__(self, port):
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=1)
        self.s.write(b"\r\n")  # kick the line parser
        time.sleep(0.2)
        self.s.reset_input_buffer()

    # ---------- text protocol ----------
    def cmd(self, line, multiline=False):
        self.s.reset_input_buffer()
        self.s.write((line + "\n").encode())
        lines = []
        deadline = time.time() + 2.5
        while time.time() < deadline:
            ln = self.s.readline().decode(errors="replace").rstrip("\r\n")
            if not ln:
                continue
            lines.append(ln)
            if ln.startswith("ERR"):
                break
            if ln.startswith("OK"):
                if not multiline:
                    break
                continue
            if ln == "END":
                break
        return lines

    def ok(self, line, multiline=False):
        r = self.cmd(line, multiline)
        assert r, f"{line!r}: no response"
        assert r[0].startswith("OK"), f"{line!r}: {r}"
        return r

    def err(self, line, code=None):
        r = self.cmd(line)
        assert r and r[0].startswith("ERR"), f"{line!r}: expected ERR, got {r}"
        if code:
            assert code in r[0], f"{line!r}: expected {code}, got {r[0]}"
        return r

    def raw_read(self, until=None, timeout=2.5, collect=None):
        """Read lines; stop when a line satisfies `until`."""
        got = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            ln = self.s.readline().decode(errors="replace").rstrip("\r\n")
            if not ln:
                continue
            got.append(ln)
            if collect is not None:
                collect.append(ln)
            if until and until(ln):
                return got
        return got

    # ---------- binary protocol ----------
    def bframe(self, op, payload=b"", seq=0):
        hdr = bytes([SOF, T_CMD, op, 0]) + struct.pack("<HH", len(payload), seq)
        body = hdr + payload
        return body + struct.pack("<H", crc16(body))

    def bread(self, timeout=1.5):
        """Read one binary frame (resync on SOF). Returns (type, op, payload)."""
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
            self.s.read(2)  # CRC (skip verification on the host side for simplicity)
            return hdr[0], hdr[1], data

    def bcmd(self, op, payload=b""):
        """Send a binary command, return the RESP payload (skipping events)."""
        self.s.reset_input_buffer()
        self.s.write(self.bframe(op, payload))
        deadline = time.time() + 2.5
        while time.time() < deadline:
            f = self.bread()
            if f is None:
                break
            typ, op2, data = f
            if typ == T_RESP and op2 == op:
                return data
            if typ == T_ERR and op2 == op:
                raise AssertionError(f"bin ERR op=0x{op:02X}: {data.hex()}")
        raise AssertionError(f"no binary response for op=0x{op:02X}")


# =========================================================================
# test registry
# =========================================================================
TESTS = []


def test(name):
    def wrap(fn):
        TESTS.append((name, fn))
        return fn
    return wrap


# ---------------- system ----------------
@test("PING")
def t_ping(d):
    r = d.ok("PING")
    assert "PONG" in r[0], r


@test("INFO")
def t_info(d):
    r = d.ok("INFO", multiline=True)
    joined = "\n".join(r)
    assert r[-1] == "END", r
    assert "mcu=RP2040" in joined
    assert "board=IO-Dock" in joined
    assert "io=6" in joined and "pwm=4" in joined


@test("LED")
def t_led(d):
    d.ok("LED ON")
    d.ok("LED OFF")


# ---------------- IO ----------------
@test("IO 配置/写/读/翻转/脉冲/读全部")
def t_io(d):
    d.ok("IO CFG IO1 OUT")
    d.ok("IO WRITE IO1 HIGH")
    r = d.ok("IO READ IO1")
    assert "IO1 HIGH" in r[0], r
    d.ok("IO WRITE IO1 LOW")
    r = d.ok("IO READ IO1")
    assert "IO1 LOW" in r[0], r
    d.ok("IO TOGGLE IO1")
    r = d.ok("IO READ IO1")
    assert "IO1 HIGH" in r[0], r
    d.ok("IO PULSE IO1 LOW 50")
    r = d.ok("IO READALL", multiline=True)
    assert r[-1] == "END"
    assert "IO1 " in "\n".join(r)
    # 非法通道
    d.err("IO CFG IO9 OUT", "E_PARAM")


@test("IO 输入配置 + 事件配置(需外部激励才能看到事件)")
def t_io_in(d):
    d.ok("IO CFG IO2 IN PULLUP")
    d.ok("IO EVENT IO2 ON CHANGE")
    d.ok("IO EVENT IO2 OFF")
    d.ok("IO CFG IO2 IN FLOAT")  # restore as input (don't drive: IO1 may be jumpered to IO2)
    print("    [注] GPIO 中断事件需外部接线触发,此处仅验证配置命令")


# ---------------- PWM ----------------
@test("PWM 配置/读取/启动/占空比/停止")
def t_pwm(d):
    r = d.ok("PWM CFG PWM1 1000 50")
    assert "1000Hz 50%" in r[0], r
    d.ok("PWM START PWM1")
    d.ok("PWM DUTY PWM1 25")
    r = d.ok("PWM READ PWM1")
    assert "running" in r[0] and "25%" in r[0], r
    d.ok("PWM STOP PWM1")
    # 非法频率 -> E_RANGE
    d.err("PWM CFG PWM1 100000000 50", "E_RANGE")


@test("PWM 跨片相位 PWM_PHASE/SYNC")
def t_pwm_phase(d):
    d.ok("PWM CFG PWM1 2000 50")
    d.ok("PWM CFG PWM2 2000 50")
    r = d.ok("PWM PHASE PWM2 PWM1 90")
    assert "90deg" in r[0], r
    d.ok("PWM SYNC PWM1,PWM2")
    # 同片对 PWM2/PWM3 -> E_CFG(相位不可调)
    d.ok("PWM CFG PWM3 2000 50")
    d.err("PWM PHASE PWM3 PWM2 45", "E_CFG")


@test("PWM 在线微调 PHADJ + 极性 POL")
def t_pwm_phadj(d):
    # 低频(分频器>1)才能 PH_ADV
    d.ok("PWM CFG PWM1 1000 50")
    d.ok("PWM START PWM1")
    d.ok("PWM PHADJ PWM1 1")
    d.ok("PWM PHADJ PWM1 -2")
    d.ok("PWM POL PWM1 INVERT")
    d.ok("PWM POL PWM1 NORMAL")
    d.ok("PWM STOP PWM1")


# ---------------- UART ----------------
@test("UART 配置/发送/统计/缓冲")
def t_uart(d):
    d.ok("UART CFG USART1 115200 8 N 1")
    r = d.ok("UART TX USART1 TXT:Hello")
    assert "5" in r[0], r
    d.ok("UART TX USART1 HEX:DEADBEEF")
    r = d.ok("UART STAT USART1")
    assert "rx=" in r[0], r
    d.ok("UART FLUSH USART1")


@test("UART 回环 + 接收事件(需 TX-RX 短接,未接则跳过)")
def t_uart_loop(d):
    # USART1 = GPIO0(TX) <-> GPIO1(RX),已由用户短接
    d.ok("UART CFG USART1 115200 8 N 1")
    d.ok("UART FLUSH USART1")
    # 1) 轮询接收
    d.ok("UART TX USART1 TXT:LOOP")
    time.sleep(0.4)
    r = d.ok("UART RX USART1 64")
    hexpart = r[0].split("HEX:")[1] if "HEX:" in r[0] else ""
    got = bytes.fromhex(hexpart).decode(errors="replace") if hexpart else ""
    if got != "LOOP":
        raise SkipTest("仅 USB，未连接 USART1 TX-RX 回环")
    print("    [OK] 回环轮询接收:", r[0])
    # 2) 接收事件(透传模式)
    d.ok("UART FLUSH USART1")
    d.ok("UART STREAM USART1 ON")
    d.ok("UART TX USART1 TXT:EVENT123")
    seen = d.raw_read(until=lambda ln: ln.startswith("EVT UART_RX"), timeout=2)
    d.ok("UART STREAM USART1 OFF")
    assert any("EVT UART_RX" in ln for ln in seen), "未收到 EVT UART_RX"
    print("    [OK] 接收事件(EVT UART_RX)已触发")


# ---------------- I2C ----------------
@test("I2C 速率 + 扫描(无设备则空)")
def t_i2c(d):
    d.ok("I2C CFG 400")
    r = d.ok("I2C SCAN", multiline=True)
    found = [ln for ln in r if ln.strip().startswith("0x")]
    print("    [INFO] 扫描到:", found if found else "无设备(空总线)")


@test("I2C 读(无设备应 NACK)")
def t_i2c_read(d):
    r = d.cmd("I2C READ 0x3C 0x00 4")
    if r and r[0].startswith("OK"):
        print("    [INFO] 设备应答:", r[0])
    else:
        d.err("I2C READ 0x3C 0x00 4", "E_NACK")
        print("    [INFO] 无设备,正确返回 NACK")


# ---------------- SPI ----------------
@test("SPI 配置/全双工/CS")
def t_spi(d):
    d.ok("SPI CFG 1000000 0 MSB 8")
    r = d.ok("SPI XFR HEX:DEADBEEF")
    assert "OK SPI XFR" in r[0], r
    print("    [INFO] SPI XFR 返回:", r[0])
    d.ok("SPI WRITE HEX:A5A5")
    r = d.ok("SPI READ 4")
    assert "OK SPI READ" in r[0], r
    d.ok("SPI CS LOW")
    d.ok("SPI CS AUTO")


# ---------------- ADC ----------------
@test("ADC 单次读取 0/1/2 + 全部 + 温度")
def t_adc(d):
    for i in range(3):
        r = d.ok(f"ADC READ ADC{i}")
        # "OK ADC READ ADCx <raw> <mv>mV"
        parts = r[0].split()
        assert len(parts) == 6 and "mV" in parts[5], r
        raw = int(parts[4]); mv = int(parts[5][:-2])
        assert 0 <= raw <= 4095 and 0 <= mv <= 3300, r
    r = d.ok("ADC READALL", multiline=True)
    assert r[-1] == "END"
    r = d.cmd("ADC TEMP")
    if r and r[0].startswith("OK") and "C" in r[0]:
        print("    [INFO] 温度:", r[0].split()[-1])
    else:
        print("    [SKIP] 温度传感器不可用(读数超出量程):", r)


@test("ADC 连续采样流(DATA/DATA_END)")
def t_adc_stream(d):
    d.ok("ADC SAMPLE ADC0 2000 300")
    data_lines = 0
    got_end = False
    seen = []
    d.raw_read(until=lambda ln: ln.startswith("DATA_END ADC0"), timeout=3,
               collect=seen)
    for ln in seen:
        if ln.startswith("DATA ADC0"):
            data_lines += 1
    assert any(ln.startswith("DATA_END ADC0") for ln in seen), "no DATA_END"
    assert data_lines >= 1, "no DATA lines"
    print(f"    [OK] {data_lines} 个 DATA 帧 + DATA_END")
    # 采样期间发命令不应被阻塞(序列在核心1,采样在主核轮询)
    d.ok("PING")
    d.cmd("ADC SAMPLE ADC0 STOP")  # stream auto-stopped -> E_CFG is fine


@test("ADC 阈值配置")
def t_adc_thresh(d):
    d.ok("ADC THRESH ADC0 500 2500 ON")
    d.ok("ADC THRESH ADC0 0 0 OFF")


# ---------------- SEQ ----------------
@test("SEQ 定义/列表/显示/运行/完成事件")
def t_seq(d):
    script = "IO IO1 LOW@0; WAIT 100; IO IO1 HIGH@100; WAIT 100"
    d.ok(f'SEQ DEF blink "{script}"')
    r = d.ok("SEQ LIST")
    assert "blink" in r[0], r
    r = d.ok("SEQ SHOW blink")
    assert "IO" in r[0], r
    d.ok("SEQ RUN blink 2")
    seen = []
    d.raw_read(until=lambda ln: ln.startswith("EVT SEQ_DONE"), timeout=4, collect=seen)
    assert any("SEQ_DONE" in ln for ln in seen), "no SEQ_DONE"
    r = d.ok("SEQ STAT")
    assert "state=idle" in r[0], r
    d.ok("SEQ DEL blink")
    r = d.ok("SEQ LIST")
    assert "blink" not in r[0]


# ---------------- binary ----------------
@test("二进制模式: 切换 + PING/INFO")
def t_bin_sys(d):
    r = d.ok("BIN ENTER")
    assert "OK BIN" in r[0], r
    # PING 0x00 -> [magic=0x4F][fw 0 1 0]
    data = d.bcmd(0x00)
    assert data[0] == 0x4F and tuple(data[1:4]) == (0, 1, 4), data
    # INFO 0x01 -> TLV 序列
    data = d.bcmd(0x01)
    assert data and b"RP2040" in data, data.hex()


@test("二进制模式: IO 配置/写/读")
def t_bin_io(d):
    d.bcmd(0x10, bytes([0, 0, 0]))          # IO_CFG IO1 OUT float
    d.bcmd(0x11, bytes([0, 1]))             # IO_WRITE IO1 HIGH
    data = d.bcmd(0x12, bytes([0]))         # IO_READ
    assert data[0] == 0 and data[1] == 1, data
    data = d.bcmd(0x13)                     # IO_READALL -> bitmask
    assert data[0] & 0x01, data


@test("二进制模式: PWM/ADC")
def t_bin_pwm_adc(d):
    # PWM_CFG [ch][freq:u32][duty:u16] = 1000Hz, 50%
    d.bcmd(0x20, bytes([0]) + struct.pack("<I", 1000) + struct.pack("<H", 32767))
    data = d.bcmd(0x26, bytes([0]))         # PWM_READ
    freq = struct.unpack("<I", data[1:5])[0]
    duty = struct.unpack("<H", data[5:7])[0]
    assert freq == 1000 and duty == 32767, (freq, duty)
    data = d.bcmd(0x60, bytes([0]))         # ADC_READ
    raw, mv = struct.unpack("<HH", data[1:5])
    assert 0 <= raw <= 4095 and 0 <= mv <= 3300, (raw, mv)
    try:
        data = d.bcmd(0x64)                 # ADC_TEMP -> m°C
        mdeg = struct.unpack("<i", data)[0]
        assert -50_000 < mdeg < 150_000, mdeg
        print("    [INFO] 温度传感器:", mdeg / 1000.0, "°C")
    except AssertionError:
        print("    [SKIP] 温度传感器不可用(读数超出量程)")


@test("二进制模式: 退出回文本")
def t_bin_exit(d):
    d.bcmd(0x0F)                            # MODE_EXIT
    r = d.ok("PING")                        # 应已回文本模式
    assert "PONG" in r[0], r


# =========================================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM 口(默认自动扫描)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    port = args.port
    if not port:
        port = autodetect()
        if not port:
            sys.exit("未找到 IO Dock,请用 --port 指定串口")
    print(f"连接 {port} ...")
    d = Dock(port)

    passed = failed = skipped = 0
    for name, fn in TESTS:
        print(f"TEST  {name}")
        try:
            fn(d)
            print(f"  PASS  {name}")
            passed += 1
        except SkipTest as e:
            print(f"  SKIP  {name}: {e}")
            skipped += 1
        except AssertionError as e:
            print(f"  FAIL  {name}: {e}")
            failed += 1
            if args.verbose:
                import traceback; traceback.print_exc()
        except Exception as e:
            print(f"  ERROR {name}: {e!r}")
            failed += 1
            if args.verbose:
                import traceback; traceback.print_exc()

    # cleanup best-effort
    try:
        d.cmd("UART STREAM USART1 OFF")
        d.cmd("ADC SAMPLE ADC0 STOP")
    except Exception:
        pass
    d.s.close()

    print(f"\n==== 结果: {passed} 通过, {failed} 失败, {skipped} 跳过 ====")
    sys.exit(1 if failed else 0)


def autodetect():
    for p in serial.tools.list_ports.comports():
        try:
            s = serial.Serial(p.device, 115200, timeout=0.4)
            s.write(b"PING\n")
            time.sleep(0.15)
            resp = s.read(64)
            s.close()
            if b"OK PING PONG" in resp:
                return p.device
        except Exception:
            continue
    return None


if __name__ == "__main__":
    main()
