"""
IO Docking Board Python SDK

基于 RP2040 IO 扩展板的 PC 控制库
支持文本模式协议，覆盖全部外设：IO/PWM/UART/I2C/SPI/ADC

使用示例:
    from iodock import IODock
    
    dock = IODock("COM3")
    dock.open()
    dock.ping()
    dock.io_write("IO1", "LOW")
    dock.close()
"""

import serial
import time
import queue
import re
import logging
import math
import threading
from typing import List, Dict, Tuple, Optional, Callable
from dataclasses import dataclass
from enum import Enum


# ============================================================================
# 枚举定义
# ============================================================================

class Level(Enum):
    """电平枚举"""
    LOW = 0
    HIGH = 1


class Direction(Enum):
    """IO 方向枚举"""
    INPUT = 1
    OUTPUT = 0


class PullMode(Enum):
    """上拉模式枚举"""
    FLOAT = 0
    PULLUP = 1
    PULLDOWN = 2


class Edge(Enum):
    """边沿触发枚举"""
    RISING = 0
    FALLING = 1
    CHANGE = 2


class Polarity(Enum):
    """PWM 极性枚举"""
    NORMAL = 0
    INVERT = 1


# ============================================================================
# 数据结构
# ============================================================================

@dataclass
class Response:
    """命令响应"""
    success: bool           # 是否成功
    command: str            # 原始命令
    payload: str            # 响应内容
    error: str = ""         # 错误信息
    error_code: int = 0     # 错误码


@dataclass
class ADCReading:
    """ADC 读数"""
    raw: int                # 原始 12 位值
    millivolts: int         # 电压（mV）


@dataclass
class PWMConfig:
    """PWM 配置"""
    frequency: int          # 频率（Hz）
    duty: int               # 占空比（0-65535）
    running: bool           # 是否运行中
    inverted: bool          # 是否反转极性


# ============================================================================
# 主控制类
# ============================================================================

class _UtcCommand(str):
    """Internal send-time UTC marker; never sent literally."""


class IODock:
    """
    IO Docking Board 主控制类
    
    通过 USB 虚拟串口与 RP2040 板卡通信
    支持全部外设的配置、控制与数据采集
    """
    
    # 错误码映射
    ERROR_CODES = {
        "E_BADCMD": 0,
        "E_PARAM": 1,
        "E_NOTFOUND": 2,
        "E_RANGE": 3,
        "E_CFG": 4,
        "E_BUSY": 5,
        "E_TIMEOUT": 6,
        "E_NACK": 7,
        "E_OVERRUN": 8,
        "E_IO": 9,
        "E_LONG": 10,
        "E_DENIED": 11
    }
    
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 1.0):
        """
        初始化 IODock
        
        Args:
            port: 串口名称（Windows: COM3, Linux: /dev/ttyACM0）
            baudrate: 波特率（默认 115200，CDC 下不影响实际速率）
            timeout: 读取超时（秒）
        """
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout 必须为有限正数")
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial: Optional[serial.Serial] = None
        
        # 事件监听
        self._event_callback: Optional[Callable] = None
        self._listening = False
        self._listen_thread: Optional[threading.Thread] = None
        self._command_lock = threading.Lock()
        self._state_lock = threading.RLock()
        self._lifecycle_lock = threading.Lock()
        self._pending = None
        self._failure = None
        self._stop = threading.Event()
        self._callbacks = queue.Queue()
        self._data_callback = None
        self._reader_thread = None
    
    # ========================================================================
    # 连接管理
    # ========================================================================
    
    def open(self) -> bool:
        """
        打开串口连接
        
        Returns:
            是否成功
        """
        with self._command_lock, self._lifecycle_lock, self._state_lock:
            if self.is_open():
                return self._failure is None
            try:
                self.serial = serial.Serial(
                    port=self.port, baudrate=self.baudrate,
                    timeout=0.05, write_timeout=self.timeout,
                    bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE)
                self.serial.reset_input_buffer()
                self.serial.reset_output_buffer()
                self._failure = None
                self._stop = threading.Event()
                self._callbacks = queue.Queue()
                self._reader_thread = threading.Thread(
                    target=self._receive_loop,
                    args=(self.serial, self._stop), daemon=True)
                self._listen_thread = threading.Thread(
                    target=self._callback_loop,
                    args=(self._callbacks, self._stop), daemon=True)
                self._reader_thread.start()
                self._listen_thread.start()
                return True
            except Exception:
                if self.serial is not None:
                    self.serial.close()
                self.serial = None
                return False

    def close(self):
        """关闭连接并唤醒正在等待响应的调用。"""
        with self._lifecycle_lock:
            with self._state_lock:
                self._stop.set()
                self._fail("串口已关闭")
                connection = self.serial
                self.serial = None
                self._listening = False
                self._callbacks.put(None)
            if connection is not None:
                connection.close()
            if self._reader_thread is not None:
                self._reader_thread.join(timeout=1.0)
            # 不等待用户回调：回调可以安全地调用 close/open/send_command。

    def is_open(self) -> bool:
        """检查连接状态"""
        with self._state_lock:
            return self.serial is not None and self.serial.is_open
    
    def ping(self) -> bool:
        """
        测试连接（发送 PING）
        
        Returns:
            是否在线
        """
        resp = self.send_command("PING")
        return resp.success and "PONG" in resp.payload
    
    # ========================================================================
    # 系统命令
    # ========================================================================
    
    def get_info(self) -> Dict[str, str]:
        """
        获取板卡信息
        
        Returns:
            信息键值对
        """
        info = {}
        resp = self.send_command("INFO")
        
        if not resp.success:
            return info
        
        for key, val in re.findall(r'(\w+)=([^\s]+)', resp.payload):
            info[key] = val
        
        return info
    
    def reset(self):
        """软复位板卡"""
        self.send_command("RESET")
    
    def enter_bootloader(self):
        """进入 UF2 下载模式"""
        self.send_command("BOOTLOADER")
    
    def set_led(self, on: bool):
        """
        控制板载 LED
        
        Args:
            on: True=亮, False=灭
        """
        self.send_command(f"LED {'ON' if on else 'OFF'}")
    
    def sync_time(self, host_us: int) -> int:
        """
        时间同步
        
        Args:
            host_us: 主机时间戳（微秒）
        
        Returns:
            偏移量
        """
        resp = self.send_command(f"SYNC {host_us}")
        if not resp.success:
            return 0
        
        # 解析 offset=xxx
        if "offset=" in resp.payload:
            start = resp.payload.find("offset=") + 7
            end = resp.payload.find(" ", start)
            if end == -1:
                end = len(resp.payload)
            return int(resp.payload[start:end])
        return 0
    
    def sync_utc(self, unix_us: Optional[int] = None) -> Response:
        """同步 UTC；省略时间时在取得发送锁后读取系统 Unix 微秒。"""
        if unix_us is None:
            return self.send_command(_UtcCommand("SYNC 0 UTC"))
        self._timing_integer(unix_us, 0, 2**64 - 1)
        return self.send_command(f"SYNC {unix_us} UTC")

    syncUtc = sync_utc

    def time_status(self) -> Response:
        return self.send_command("TIME")

    @staticmethod
    def _timing_integer(value, low, high):
        if not isinstance(value, int) or not low <= value <= high:
            raise ValueError(f"整数参数须在 {low}..{high} 范围内")

    def timing_configure(self, pps: int, trigger_mask: int, hz: int,
                         pps_width_us: int, trigger_width_us: int,
                         invert: bool = False) -> Response:
        self._timing_integer(hz, 1, 100)
        for value, low, high in ((pps, 1, 4), (trigger_mask, 1, 15),
                                 (hz, 1, 100), (pps_width_us, 100, 999999),
                                 (trigger_width_us, 100, 1000000 // hz - 1), (invert, 0, 1)):
            self._timing_integer(value, low, high)
        return self.send_command(f"TIMING CFG {pps} {trigger_mask} {hz} "
                                 f"{pps_width_us} {trigger_width_us} {int(invert)}")

    def timing_nmea(self, uart: int = 0, baud: int = 115200,
                    delay_us: int = 0) -> Response:
        for value, low, high in ((uart, 0, 2), (baud, 1200, 7800000),
                                 (delay_us, 0, 899999)):
            self._timing_integer(value, low, high)
        return self.send_command(f"TIMING NMEA {uart} {baud} {delay_us}")

    def timing_start(self) -> Response:
        return self.send_command("TIMING START")

    def timing_stop(self) -> Response:
        return self.send_command("TIMING STOP")

    def timing_status(self) -> Response:
        return self.send_command("TIMING STAT")

    def boot_sequence(self, name: Optional[str] = None) -> Response:
        """选择已定义序列；None 关闭。显式 send_command('SAVE') 持久化。"""
        if name is not None and (not re.fullmatch(r"[A-Za-z0-9_]{1,15}", name)
                                 or name.upper() in {"OFF", "STAT"}):
            raise ValueError("序列名须为字母、数字或下划线，且不能为 OFF/STAT")
        return self.send_command("BOOT SEQ " + (name if name is not None else "OFF"))

    def boot_sequence_status(self) -> Response:
        return self.send_command("BOOT SEQ STAT")

    # ========================================================================
    # IO 控制（6路）
    # ========================================================================
    
    def io_config(self, ch: str, direction: Direction, pull: PullMode = PullMode.FLOAT):
        """
        配置 IO 方向和上拉
        
        Args:
            ch: 通道名称（IO1-IO6）
            direction: 方向（INPUT/OUTPUT）
            pull: 上拉模式（FLOAT/PULLUP/PULLDOWN）
        """
        cmd = f"IO CFG {ch} {'IN' if direction == Direction.INPUT else 'OUT'}"
        
        if pull == PullMode.PULLUP:
            cmd += " PULLUP"
        elif pull == PullMode.PULLDOWN:
            cmd += " PULLDOWN"
        else:
            cmd += " FLOAT"
        
        self.send_command(cmd)
    
    def io_write(self, ch: str, level):
        """
        设置 IO 输出电平
        
        Args:
            ch: 通道名称（IO1-IO6）
            level: 电平（HIGH/LOW 或 Level 枚举）
        """
        if isinstance(level, Level):
            level_str = "HIGH" if level == Level.HIGH else "LOW"
        else:
            level_str = str(level).upper()
        
        self.send_command(f"IO WRITE {ch} {level_str}")
    
    def io_read(self, ch: str) -> Level:
        """
        读取 IO 输入电平
        
        Args:
            ch: 通道名称（IO1-IO6）
        
        Returns:
            电平状态
        """
        resp = self.send_command(f"IO READ {ch}")
        if not resp.success:
            raise RuntimeError(resp.error)
        match = re.fullmatch(re.escape(ch.upper()) + r" (HIGH|LOW)", resp.payload)
        if not match:
            raise ValueError("无效 IO 响应: " + resp.payload)
        return Level[match[1]]

    def io_read_all(self) -> List[Level]:
        """按 IO1..IO6 顺序读取全部通道。"""
        resp = self.send_command("IO READALL")
        if not resp.success:
            raise RuntimeError(resp.error)
        values = dict(re.findall(r"\b(IO[1-6]) (HIGH|LOW)\b", resp.payload))
        if len(values) != 6:
            raise ValueError("无效 IO READALL 响应: " + resp.payload)
        return [Level[values[f"IO{i}"]] for i in range(1, 7)]

    def io_toggle(self, ch: str):
        """
        翻转 IO 输出
        
        Args:
            ch: 通道名称（IO1-IO6）
        """
        self.send_command(f"IO TOGGLE {ch}")
    
    def io_pulse(self, ch: str, level, ms: int):
        """
        输出脉冲
        
        Args:
            ch: 通道名称（IO1-IO6）
            level: 脉冲电平（HIGH/LOW）
            ms: 持续时间（毫秒）
        """
        if isinstance(level, Level):
            level_str = "HIGH" if level == Level.HIGH else "LOW"
        else:
            level_str = str(level).upper()
        
        self.send_command(f"IO PULSE {ch} {level_str} {ms}")
    
    def io_event(self, ch: str, enable: bool, edge: Edge = Edge.CHANGE):
        """
        使能 IO 事件上报
        
        Args:
            ch: 通道名称（IO1-IO6）
            enable: 是否使能
            edge: 触发边沿
        """
        cmd = f"IO EVENT {ch} {'ON' if enable else 'OFF'}"
        
        if enable:
            if edge == Edge.RISING:
                cmd += " RISING"
            elif edge == Edge.FALLING:
                cmd += " FALLING"
            else:
                cmd += " CHANGE"
        
        self.send_command(cmd)
    
    # ========================================================================
    # PWM 控制（4路）
    # ========================================================================
    
    def pwm_config(self, ch: str, freq_hz: int, duty: int):
        """
        配置 PWM
        
        Args:
            ch: 通道名称（PWM1-PWM4）
            freq_hz: 频率（Hz）
            duty: 占空比（0-100 百分比 或 0-65535 原始值）
        """
        self.send_command(f"PWM CFG {ch} {freq_hz} {duty}")
    
    def pwm_set_freq(self, ch: str, freq_hz: int):
        """
        设置 PWM 频率
        
        Args:
            ch: 通道名称
            freq_hz: 频率（Hz）
        """
        self.send_command(f"PWM FREQ {ch} {freq_hz}")
    
    def pwm_set_duty(self, ch: str, duty: int):
        """
        设置 PWM 占空比
        
        Args:
            ch: 通道名称
            duty: 占空比（0-100）
        """
        self.send_command(f"PWM DUTY {ch} {duty}")
    
    def pwm_start(self, ch: str):
        """
        启动 PWM 输出
        
        Args:
            ch: 通道名称
        """
        self.send_command(f"PWM START {ch}")
    
    def pwm_stop(self, ch: str):
        """
        停止 PWM 输出
        
        Args:
            ch: 通道名称
        """
        self.send_command(f"PWM STOP {ch}")
    
    def pwm_sweep(self, ch: str, from_duty: int, to_duty: int, 
                  step_ms: int, repeat: int = 1):
        """
        PWM 渐变（呼吸灯效果）
        
        Args:
            ch: 通道名称
            from_duty: 起始占空比
            to_duty: 目标占空比
            step_ms: 步进间隔（毫秒）
            repeat: 重复次数（0=无限）
        """
        self.send_command(f"PWM SWEEP {ch} {from_duty} {to_duty} {step_ms} {repeat}")
    
    def pwm_read(self, ch: str) -> PWMConfig:
        """
        读取 PWM 配置
        
        Args:
            ch: 通道名称
        
        Returns:
            配置信息
        """
        resp = self.send_command(f"PWM READ {ch}")
        if not resp.success:
            raise RuntimeError(resp.error)
        match = re.fullmatch(re.escape(ch.upper()) +
                             r" (\d+)Hz (\d+)% (running|stopped) pol=(invert|normal)",
                             resp.payload)
        if not match:
            raise ValueError("无效 PWM 响应: " + resp.payload)
        # 文本固件仅返回取整后的百分比；换算为近似 16 位值。
        return PWMConfig(int(match[1]), (int(match[2]) * 65535 + 50) // 100,
                         match[3] == "running", match[4] == "invert")
    
    def pwm_sync(self, channels: List[str]):
        """
        同步启动多路 PWM（相位锁定）
        
        Args:
            channels: 通道列表（如 ["PWM1", "PWM2"]）
        """
        self.send_command(f"PWM SYNC {','.join(channels)}")
    
    def pwm_set_phase(self, ch: str, ref: str, degrees: int):
        """
        设置 PWM 相位偏移
        
        Args:
            ch: 目标通道
            ref: 参考通道
            degrees: 相位度数（0-360）
        """
        self.send_command(f"PWM PHASE {ch} {ref} {degrees}")
    
    def pwm_set_polarity(self, ch: str, polarity: Polarity):
        """
        设置 PWM 极性
        
        Args:
            ch: 通道名称
            polarity: 极性（NORMAL/INVERT）
        """
        self.send_command(f"PWM POL {ch} {'INVERT' if polarity == Polarity.INVERT else 'NORMAL'}")
    
    def pwm_tick(self, ch: str, freq_hz: int):
        """
        启用 PWM 定时回调（低频输出）
        
        Args:
            ch: 通道名称
            freq_hz: 频率（低于 7.45Hz 时软件模拟输出）
        """
        self.send_command(f"PWM TICK {ch} {freq_hz}")
    
    def pwm_tick_off(self, ch: str):
        """
        关闭全局 PWM 定时回调（ch 参数保留用于兼容）
        
        Args:
            ch: 通道名称
        """
        self.send_command("PWM TICK OFF")
    
    # ========================================================================
    # UART 控制（2路）
    # ========================================================================
    
    def uart_config(self, ch: str, baud: int, data_bits: int = 8,
                    parity: str = 'N', stop_bits: int = 1):
        """
        配置 UART
        
        Args:
            ch: 通道名称（USART1/USART2）
            baud: 波特率
            data_bits: 数据位（5-8）
            parity: 校验（N/O/E）
            stop_bits: 停止位（1/2）
        """
        self.send_command(f"UART CFG {ch} {baud} {data_bits} {parity} {stop_bits}")
    
    def uart_send_text(self, ch: str, text: str):
        """
        UART 发送文本
        
        Args:
            ch: 通道名称
            text: 文本内容
        """
        self.send_command(f"UART TX {ch} TXT:{text}")
    
    def uart_send_hex(self, ch: str, data: bytes):
        """
        UART 发送十六进制数据
        
        Args:
            ch: 通道名称
            data: 数据字节
        """
        hex_str = data.hex().upper()
        self.send_command(f"UART TX {ch} HEX:{hex_str}")
    
    def uart_receive(self, ch: str, max_bytes: int = 0) -> bytes:
        """
        读取 UART 接收缓冲
        
        Args:
            ch: 通道名称
            max_bytes: 最大读取字节数（0=全部）
        
        Returns:
            接收到的数据
        """
        cmd = f"UART RX {ch}"
        if max_bytes > 0:
            cmd += f" {max_bytes}"
        
        resp = self.send_command(cmd)
        if not resp.success:
            return b""
        
        match = re.fullmatch(re.escape(ch.upper()) + r" HEX:([0-9A-Fa-f]*)", resp.payload)
        if not match:
            raise ValueError("无效 UART 响应: " + resp.payload)
        return bytes.fromhex(match[1])

    def uart_flush(self, ch: str):
        """
        清空 UART 接收缓冲
        
        Args:
            ch: 通道名称
        """
        self.send_command(f"UART FLUSH {ch}")
    
    def uart_stream(self, ch: str, enable: bool):
        """
        启用/关闭 UART 透传模式
        
        Args:
            ch: 通道名称
            enable: 是否启用
        """
        self.send_command(f"UART STREAM {ch} {'ON' if enable else 'OFF'}")
    
    # ========================================================================
    # I2C 控制（1路）
    # ========================================================================
    
    def i2c_set_speed(self, khz: int):
        """
        设置 I2C 速率
        
        Args:
            khz: 速率（100/400/1000）
        """
        self.send_command(f"I2C CFG {khz}")
    
    def i2c_scan(self) -> List[int]:
        """
        扫描 I2C 总线
        
        Returns:
            应答的从机地址列表
        """
        addrs = []
        resp = self.send_command("I2C SCAN")
        
        if not resp.success:
            return addrs
        
        # 解析 "0x3C 0x50 ..." 格式
        for token in resp.payload.split():
            if token.startswith("0x") or token.startswith("0X"):
                try:
                    addrs.append(int(token, 16))
                except ValueError:
                    pass
        
        return addrs
    
    def i2c_write_reg(self, addr: int, reg: int, data: bytes):
        """
        I2C 写寄存器
        
        Args:
            addr: 从机地址（7位）
            reg: 寄存器地址
            data: 数据字节
        """
        hex_data = data.hex().upper()
        self.send_command(f"I2C WRITE 0x{addr:02X} 0x{reg:02X} {hex_data}")
    
    def i2c_read_reg(self, addr: int, reg: int, count: int) -> bytes:
        """
        I2C 读寄存器
        
        Args:
            addr: 从机地址
            reg: 寄存器地址
            count: 读取字节数
        
        Returns:
            数据
        """
        resp = self.send_command(f"I2C READ 0x{addr:02X} 0x{reg:02X} {count}")
        
        if not resp.success:
            return b""
        
        if "HEX:" in resp.payload:
            hex_str = resp.payload.split("HEX:")[1].strip()
            return bytes.fromhex(hex_str)
        return b""
    
    def i2c_write(self, addr: int, data: bytes):
        """
        I2C 直写（无寄存器地址）
        
        Args:
            addr: 从机地址
            data: 数据
        """
        hex_data = data.hex().upper()
        self.send_command(f"I2C WRONLY 0x{addr:02X} {hex_data}")
    
    def i2c_read(self, addr: int, count: int) -> bytes:
        """
        I2C 直读（无寄存器地址）
        
        Args:
            addr: 从机地址
            count: 字节数
        
        Returns:
            数据
        """
        resp = self.send_command(f"I2C RONLY 0x{addr:02X} {count}")
        
        if not resp.success:
            return b""
        
        if "HEX:" in resp.payload:
            hex_str = resp.payload.split("HEX:")[1].strip()
            return bytes.fromhex(hex_str)
        return b""
    
    # ========================================================================
    # SPI 控制（1路）
    # ========================================================================
    
    def spi_config(self, baud: int, mode: int = 0, 
                   bit_order: str = "MSB", bits: int = 8):
        """
        配置 SPI
        
        Args:
            baud: 速率（Hz）
            mode: 模式（0-3）
            bit_order: 位序（MSB/LSB）
            bits: 数据位宽（4-16）
        """
        self.send_command(f"SPI CFG {baud} {mode} {bit_order} {bits}")
    
    def spi_transfer(self, tx_data: bytes) -> bytes:
        """
        SPI 全双工传输
        
        Args:
            tx_data: 发送数据
        
        Returns:
            接收数据
        """
        hex_str = tx_data.hex().upper()
        resp = self.send_command(f"SPI XFR HEX:{hex_str}")
        
        if not resp.success:
            return b""
        
        if "HEX:" in resp.payload:
            hex_str = resp.payload.split("HEX:")[1].strip()
            return bytes.fromhex(hex_str)
        return b""
    
    def spi_write(self, tx_data: bytes):
        """
        SPI 只发送
        
        Args:
            tx_data: 发送数据
        """
        hex_str = tx_data.hex().upper()
        self.send_command(f"SPI WRITE HEX:{hex_str}")
    
    def spi_read(self, count: int) -> bytes:
        """
        SPI 只接收
        
        Args:
            count: 字节数
        
        Returns:
            接收数据
        """
        resp = self.send_command(f"SPI READ {count}")
        
        if not resp.success:
            return b""
        
        if "HEX:" in resp.payload:
            hex_str = resp.payload.split("HEX:")[1].strip()
            return bytes.fromhex(hex_str)
        return b""
    
    def spi_cs(self, mode: str):
        """
        控制 SPI CS 引脚
        
        Args:
            mode: AUTO/HIGH/LOW
        """
        self.send_command(f"SPI CS {mode}")
    
    # ========================================================================
    # ADC 控制（3路+温度）
    # ========================================================================
    
    def adc_read(self, ch: str) -> ADCReading:
        """
        单次读取 ADC
        
        Args:
            ch: 通道名称（ADC0-ADC2）
        
        Returns:
            读数（原始值 + 电压）
        """
        resp = self.send_command(f"ADC READ {ch}")
        
        if not resp.success:
            raise RuntimeError(resp.error)
        values = self._adc_values(resp.payload)
        if ch.upper() not in values:
            raise ValueError("无效 ADC 响应: " + resp.payload)
        return values[ch.upper()]

    @staticmethod
    def _adc_values(payload):
        return {ch: ADCReading(int(raw), int(mv)) for ch, raw, mv in
                re.findall(r"\b(ADC[0-2]) (\d+) (\d+)mV\b", payload)}

    def adc_read_all(self) -> List[ADCReading]:
        """按 ADC0..ADC2 顺序读取全部通道。"""
        resp = self.send_command("ADC READALL")
        if not resp.success:
            raise RuntimeError(resp.error)
        values = self._adc_values(resp.payload)
        if len(values) != 3:
            raise ValueError("无效 ADC READALL 响应: " + resp.payload)
        return [values[f"ADC{i}"] for i in range(3)]

    def adc_start_sample(self, ch: str, rate_hz: int, duration_ms: int = 1000):
        """
        启动连续采样
        
        Args:
            ch: 通道名称
            rate_hz: 采样率（Hz）
            duration_ms: 持续时间（毫秒，1..1000000，默认1000）
        """
        if not 1 <= duration_ms <= 1000000 or not 1 <= rate_hz <= 500000:
            raise ValueError("采样率须为1..500000，时长须为1..1000000毫秒")
        cmd = f"ADC SAMPLE {ch} {rate_hz} {duration_ms}"
        self.send_command(cmd)
    
    def adc_stop_sample(self, ch: str):
        """
        停止连续采样
        
        Args:
            ch: 通道名称
        """
        self.send_command(f"ADC SAMPLE {ch} STOP")
    
    def adc_read_temp(self) -> float:
        """
        读取片上温度
        
        Returns:
            温度（摄氏度）
        """
        resp = self.send_command("ADC TEMP")
        
        if not resp.success:
            return 0.0
        
        # 解析 "27.3C" 格式
        temp_str = resp.payload.replace("C", "").strip()
        try:
            return float(temp_str)
        except ValueError:
            return 0.0
    
    def adc_threshold(self, ch: str, low_mv: int, high_mv: int, enable: bool):
        """
        设置 ADC 阈值事件
        
        Args:
            ch: 通道名称
            low_mv: 低阈值（mV）
            high_mv: 高阈值（mV）
            enable: 是否启用
        """
        self.send_command(f"ADC THRESH {ch} {low_mv} {high_mv} {'ON' if enable else 'OFF'}")
    
    # ========================================================================
    # 组合时序（SEQ）
    # ========================================================================
    
    def seq_define(self, name: str, script: str):
        """
        定义序列
        
        Args:
            name: 序列名称
            script: 脚本内容
        """
        self.send_command(f'SEQ DEF {name} "{script}"')
    
    def seq_list(self) -> List[str]:
        """
        列出已定义序列
        
        Returns:
            序列名称列表
        """
        resp = self.send_command("SEQ LIST")
        
        if not resp.success:
            return []
        
        names = []
        for name in resp.payload.split():
            if name and name != "END":
                names.append(name)
        return names
    
    def seq_run(self, name: str, repeat: int = 1):
        """
        执行序列
        
        Args:
            name: 序列名称
            repeat: 循环次数（0=无限）
        """
        self.send_command(f"SEQ RUN {name} {repeat}")
    
    def seq_stop(self):
        """停止当前序列"""
        self.send_command("SEQ STOP")
    
    def seq_delete(self, name: str):
        """
        删除序列
        
        Args:
            name: 序列名称
        """
        self.send_command(f"SEQ DEL {name}")
    
    # ========================================================================
    # 事件处理
    # ========================================================================
    
    def on_event(self, callback: Callable[[str, str], None]):
        """注册 EVT 回调，使用 start_event_listener 启用。"""
        with self._state_lock:
            self._event_callback = callback

    def on_data(self, callback: Callable[[str, str], None]):
        """注册数据回调，不受事件监听开关影响。

        DATA ADC0 ... -> callback("ADC0", "...")；
        DATA_END ADC0 -> callback("DATA_END", "ADC0")。
        DATA_END 的第二参数保留标记后的完整文本。
        """
        with self._state_lock:
            self._data_callback = callback

    def start_event_listener(self):
        with self._state_lock:
            self._listening = True

    def stop_event_listener(self):
        """停止投递新事件；已经开始执行的回调不会被中断。"""
        with self._state_lock:
            self._listening = False

    @staticmethod
    def _callback_loop(callbacks, stop):
        while True:
            item = callbacks.get()
            if item is None or stop.is_set():
                return
            callback, name, data = item
            try:
                callback(name, data)
            except Exception:
                logging.getLogger(__name__).exception("IODock callback failed")

    def _fail(self, message):
        # 调用方持有 _state_lock。协议没有请求 ID，故失步后必须重连。
        self._failure = message
        if self._pending is not None:
            self._pending.put(("failure", message))

    def _receive_loop(self, connection, stop):
        buffer = bytearray()
        try:
            while not stop.is_set():
                chunk = connection.read(max(1, min(connection.in_waiting, 4096)))
                if not chunk:
                    continue
                buffer.extend(chunk)
                while b"\n" in buffer:
                    raw, _, rest = buffer.partition(b"\n")
                    buffer = bytearray(rest)
                    line = raw.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    with self._state_lock:
                        if stop.is_set():
                            return
                        if line == "DATA_END" or line.startswith("DATA_END "):
                            if self._data_callback:
                                self._callbacks.put((self._data_callback, "DATA_END",
                                                     line.partition(" ")[2]))
                        elif line.startswith(("EVT ", "DATA ")):
                            kind, _, content = line.partition(" ")
                            name, _, data = content.partition(" ")
                            callback = (self._event_callback if self._listening else None) \
                                if kind == "EVT" else self._data_callback
                            if callback:
                                self._callbacks.put((callback, name, data))
                        elif self._pending is not None:
                            self._pending.put(("line", line))
        except Exception as exc:
            with self._state_lock:
                if not stop.is_set():
                    self._fail("串口读取失败: " + str(exc))

    _MULTILINE = {"INFO", "HELP", "IO READALL", "ADC READALL", "I2C SCAN"}
    _GROUPS = {"IO", "PWM", "UART", "I2C", "SPI", "ADC", "SEQ", "CFG", "BIN", "TIMING", "BOOT"}

    @classmethod
    def _command_name(cls, cmd):
        words = cmd.upper().split()
        return " ".join(words[:2] if words[0] in cls._GROUPS else words[:1])

    def send_command(self, cmd: str) -> Response:
        """串行执行完整事务。失败/超时后须 close/open 才能继续。"""
        if not cmd.strip() or any(c in cmd for c in "\r\n\0"):
            raise ValueError("命令必须是非空单行文本")
        if len(cmd.encode("utf-8")) > 511:
            raise ValueError("命令超过511字节")
        name = self._command_name(cmd)
        if name == "BIN ENTER":
            raise ValueError("本 SDK 仅支持文本协议")
        with self._command_lock:
            pending = queue.Queue()
            with self._state_lock:
                if not self.is_open() or self._failure:
                    return Response(False, cmd, "", self._failure or "串口未打开", 9)
                self._pending = pending
                connection = self.serial
            deadline = time.monotonic() + self.timeout
            try:
                if isinstance(cmd, _UtcCommand):
                    cmd = f"SYNC {time.time_ns() // 1000} UTC"
                wire = (cmd + "\n").encode("utf-8")
                if connection.write(wire) != len(wire):
                    raise OSError("串口写入不完整")
                response = None
                lines = []
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise queue.Empty
                    kind, line = pending.get(timeout=remaining)
                    if kind == "failure":
                        return Response(False, cmd, "", line, 9)
                    if response is None:
                        response = self._parse_response(line)
                        # unknown command 错误只回显第一个命令词。
                        if response.command != name and not (
                                not response.success and response.error_code == 0
                                and response.command == name.split()[0]):
                            raise ValueError("响应命令不匹配: " + line)
                        response.command = cmd
                        if not response.success or name not in self._MULTILINE:
                            return response
                        if response.payload:
                            lines.append(response.payload)
                    elif line == "END":
                        response.payload = "\n".join(lines)
                        return response
                    elif line.startswith(("OK ", "ERR ")):
                        raise ValueError("多行响应缺少 END")
                    else:
                        lines.append(line)
            except queue.Empty:
                with self._state_lock:
                    self._fail("响应超时；请关闭并重新打开连接")
                return Response(False, cmd, "", self._failure, 6)
            except Exception as exc:
                with self._state_lock:
                    self._fail(str(exc) + "；请关闭并重新打开连接")
                return Response(False, cmd, "", self._failure, 9)
            finally:
                with self._state_lock:
                    self._pending = None

    def _parse_response(self, line: str) -> Response:
        """解析标准版单/双词命令头与符号错误码。"""
        if line.startswith("ERR "):
            match = re.fullmatch(r"ERR (.+?) (E_[A-Z_]+)(?: (.*))?", line)
            if match:
                return Response(False, match[1], "", match[3] or match[2],
                                self.ERROR_CODES.get(match[2], -1))
        elif line.startswith("OK "):
            content = line[3:]
            name = self._command_name(content)
            return Response(True, name, content[len(name):].strip())
        raise ValueError("无效响应: " + line)


# ============================================================================
# 快捷函数
# ============================================================================

def create(port: str) -> IODock:
    """
    创建并连接 IODock
    
    Args:
        port: 串口名称
    
    Returns:
        IODock 实例
    """
    dock = IODock(port)
    dock.open()
    return dock
