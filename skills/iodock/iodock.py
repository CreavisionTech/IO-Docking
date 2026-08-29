#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iodock — 命令行控制 IO Dock 板卡(通过 USB 串口文本协议)。

用法:
    iodock ping | info | reset | bootloader | led <on|off>

    # IO(IO1..N,数量见 info)
    iodock io config <ch> <out|in> [pull]     # pull: 0|1|2 (in 时)
    iodock io write <ch> <high|low>
    iodock io read <ch>
    iodock io readall
    iodock io toggle <ch>
    iodock io pulse <ch> <high|low> <ms>
    iodock io event <ch> <off|rise|fall|both>

    # PWM(PWM1..M,数量见 info)
    iodock pwm cfg <ch> <freq_hz> [duty_pct]  # 占空比 0..100,可小数(0.1% 精度)
    iodock pwm freq <ch> <freq_hz>
    iodock pwm duty <ch> <duty_pct>
    iodock pwm start <ch>
    iodock pwm stop <ch>
    iodock pwm read <ch>
    iodock pwm pol <ch> <normal|invert>
    iodock pwm sweep <ch> <from_pct> <to_pct> <step_ms>
    iodock pwm sync <ch1,ch2,...>
    iodock pwm phase <ch> <ref_ch> <deg>
    iodock pwm phadj <ch> <ticks>
    iodock pwm tick <ch> <freq_hz>            # 低频方波 + 事件回调
    iodock pwm tick off

    # UART(USART1)
    iodock uart cfg <baud> [databits] [parity] [stopbits]   # parity N|O|E
    iodock uart tx <text>                       # 纯偶数字符串按 HEX,其余按文本
    iodock uart rx [n]
    iodock uart flush
    iodock uart stream <on|off>
    iodock uart stat

    # I2C
    iodock i2c cfg <khz>                        # 100|400|1000
    iodock i2c scan
    iodock i2c read <addr> [reg] [n]
    iodock i2c ronly <addr> [n]
    iodock i2c write <addr> <reg> <hexdata>
    iodock i2c wronly <addr> <hexdata>

    # 参数持久化
    iodock config save | load | clear

端口自动识别(VID:PID 1209:8888),可用 IO_DOCK_PORT=COMx 覆盖。
"""
import os
import re
import sys
import time

import serial
import serial.tools.list_ports


def find_dock_port():
    for p in serial.tools.list_ports.comports():
        h = getattr(p, "hwid", "") or ""
        if "1209" in h and "8888" in h:
            return p.device
    return None


class Dock:
    def __init__(self):
        port = os.environ.get("IO_DOCK_PORT") or find_dock_port()
        if not port:
            sys.stderr.write("未找到 IO Dock 串口(1209:8888)。请插上板卡或设置 IO_DOCK_PORT=COMx\n")
            sys.exit(2)
        self.ser = serial.Serial(port, 115200, timeout=1)
        self.ser.reset_input_buffer()

    def send(self, cmd, multiline=False, timeout=4.0):
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        lines = []
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            if line.startswith("EVT "):
                continue
            lines.append(line)
            if multiline:
                if line == "END":
                    break
            elif line.startswith("OK ") or line.startswith("ERR "):
                break
        if not lines:
            sys.stderr.write("命令超时: %s\n" % cmd)
            sys.exit(1)
        if not lines[0].startswith("OK "):
            sys.stderr.write("%s\n" % lines[0])
            sys.exit(1)
        return "\n".join(lines)


def duty_raw(pct):
    d = float(pct)
    if not (0 <= d <= 100):
        sys.exit("占空比需 0..100")
    return max(0, min(65535, round(d * 655.35)))


def onoff(v):
    return v.lower() in ("high", "1", "on")


def main(argv):
    dock = Dock()
    if not argv:
        print(__doc__)
        return
    a = argv
    c = a[0]

    if c == "ping":
        print(dock.send("PING"))
    elif c == "info":
        print(dock.send("INFO", multiline=True))
    elif c == "reset":
        print(dock.send("RESET"))
    elif c == "bootloader":
        print(dock.send("BOOTLOADER"))
    elif c == "led":
        print(dock.send("LED " + ("ON" if onoff(a[1]) else "OFF")))

    elif c == "io":
        sub = a[1] if len(a) > 1 else ""
        ch = a[2] if len(a) > 2 else ""
        if sub == "config":
            mode = a[3].upper()
            pull = a[4] if len(a) > 4 else "0"
            cmd = f"IO CFG IO{ch} {mode}"
            if mode == "IN" and pull in ("1", "2"):
                cmd += " PULLUP" if pull == "1" else " PULLDOWN"
            print(dock.send(cmd))
        elif sub == "write":
            print(dock.send(f"IO WRITE IO{ch} {'HIGH' if onoff(a[3]) else 'LOW'}"))
        elif sub == "read":
            print(dock.send(f"IO READ IO{ch}"))
        elif sub == "readall":
            print(dock.send("IO READALL", multiline=True))
        elif sub == "toggle":
            print(dock.send(f"IO TOGGLE IO{ch}"))
        elif sub == "pulse":
            lvl = "HIGH" if onoff(a[3]) else "LOW"
            print(dock.send(f"IO PULSE IO{ch} {lvl} {a[4]}"))
        elif sub == "event":
            m = {"off": "OFF", "rise": "ON RISING", "fall": "ON FALLING", "both": "ON CHANGE"}
            if a[3] not in m:
                sys.exit("io event: off|rise|fall|both")
            print(dock.send(f"IO EVENT IO{ch} {m[a[3]]}"))
        else:
            sys.exit("io 子命令: config|write|read|readall|toggle|pulse|event")

    elif c == "pwm":
        sub = a[1] if len(a) > 1 else ""
        ch = a[2] if len(a) > 2 else ""
        if sub == "cfg":
            freq = a[3]
            duty = a[4] if len(a) > 4 else "50"
            print(dock.send(f"PWM CFG PWM{ch} {freq} #{duty_raw(duty)}"))
        elif sub == "freq":
            print(dock.send(f"PWM FREQ PWM{ch} {a[3]}"))
        elif sub == "duty":
            print(dock.send(f"PWM DUTY PWM{ch} #{duty_raw(a[3])}"))
        elif sub == "start":
            print(dock.send(f"PWM START PWM{ch}"))
        elif sub == "stop":
            print(dock.send(f"PWM STOP PWM{ch}"))
        elif sub == "read":
            print(dock.send(f"PWM READ PWM{ch}"))
        elif sub == "pol":
            inv = "INVERT" if a[3].lower() in ("invert", "1") else "NORMAL"
            print(dock.send(f"PWM POL PWM{ch} {inv}"))
        elif sub == "sweep":
            fr = duty_raw(a[3])
            to = duty_raw(a[4])
            print(dock.send(f"PWM SWEEP PWM{ch} #{fr} #{to} {a[5]} 0"))
        elif sub == "sync":
            chs = a[2]
            print(dock.send(f"PWM SYNC {chs}"))
        elif sub == "phase":
            print(dock.send(f"PWM PHASE PWM{ch} PWM{a[3]} {a[4]}"))
        elif sub == "phadj":
            print(dock.send(f"PWM PHADJ PWM{ch} {a[3]}"))
        elif sub == "tick":
            if a[2].lower() in ("off",):
                print(dock.send("PWM TICK OFF"))
            else:
                print(dock.send(f"PWM TICK PWM{ch} {a[3]}"))
        else:
            sys.exit("pwm 子命令: cfg|freq|duty|start|stop|read|pol|sweep|sync|phase|phadj|tick")

    elif c == "uart":
        sub = a[1] if len(a) > 1 else ""
        if sub == "cfg":
            baud = a[2]
            dbits = a[3] if len(a) > 3 else "8"
            parity = a[4] if len(a) > 4 else "N"
            sbits = a[5] if len(a) > 5 else "1"
            print(dock.send(f"UART CFG USART1 {baud} {dbits} {parity} {sbits}"))
        elif sub == "tx":
            data = a[2]
            if re.fullmatch(r"[0-9a-fA-F]+", data) and len(data) % 2 == 0 and len(data) > 1:
                print(dock.send(f"UART TX USART1 HEX:{data}"))
            else:
                print(dock.send(f"UART TX USART1 TXT:{data}"))
        elif sub == "rx":
            n = a[2] if len(a) > 2 else "64"
            print(dock.send(f"UART RX USART1 {n}"))
        elif sub == "flush":
            print(dock.send("UART FLUSH USART1"))
        elif sub == "stream":
            print(dock.send(f"UART STREAM USART1 {'ON' if onoff(a[2]) else 'OFF'}"))
        elif sub == "stat":
            print(dock.send("UART STAT USART1"))
        else:
            sys.exit("uart 子命令: cfg|tx|rx|flush|stream|stat")

    elif c == "i2c":
        sub = a[1] if len(a) > 1 else ""
        if sub == "cfg":
            print(dock.send(f"I2C CFG {a[2]}"))
        elif sub == "scan":
            print(dock.send("I2C SCAN", multiline=True))
        elif sub == "read":
            addr = a[2]
            reg = a[3] if len(a) > 3 else "0"
            n = a[4] if len(a) > 4 else "1"
            print(dock.send(f"I2C READ {addr} {reg} {n}"))
        elif sub == "ronly":
            addr = a[2]
            n = a[3] if len(a) > 3 else "1"
            print(dock.send(f"I2C RONLY {addr} {n}"))
        elif sub == "write":
            print(dock.send(f"I2C WRITE {a[2]} {a[3]} {a[4]}"))
        elif sub == "wronly":
            print(dock.send(f"I2C WRONLY {a[2]} {a[3]}"))
        else:
            sys.exit("i2c 子命令: cfg|scan|read|ronly|write|wronly")

    elif c == "config":
        sub = a[1] if len(a) > 1 else ""
        if sub == "save":
            print(dock.send("SAVE"))
        elif sub == "load":
            print(dock.send("LOAD"))
        elif sub == "clear":
            print(dock.send("CFG CLEAR"))
        else:
            sys.exit("config 子命令: save|load|clear")

    else:
        sys.exit("未知命令: %s\n%s" % (c, __doc__))


if __name__ == "__main__":
    main(sys.argv[1:])
