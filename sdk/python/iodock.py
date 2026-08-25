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
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial: Optional[serial.Serial] = None
        
        # 事件监听
        self._event_callback: Optional[Callable] = None
        self._listening = False
        self._listen_thread: Optional[threading.Thread] = None
    
    # ========================================================================
    # 连接管理
    # ========================================================================
    
    def open(self) -> bool:
        """
        打开串口连接
        
        Returns:
            是否成功
        """
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE
            )
            # 清空缓冲区
            self.serial.reset_input_buffer()
            self.serial.reset_output_buffer()
            return True
        except Exception as e:
            print(f"打开串口失败: {e}")
            return False
    
    def close(self):
        """关闭连接"""
        self.stop_event_listener()
        if self.serial and self.serial.is_open:
            self.serial.close()
            self.serial = None
    
    def is_open(self) -> bool:
        """检查连接状态"""
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
        
        for line in resp.payload.split('\n'):
            line = line.strip()
            if '=' in line:
                key, val = line.split('=', 1)
                info[key.strip()] = val.strip()
        
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
        if resp.success and "HIGH" in resp.payload:
            return Level.HIGH
        return Level.LOW
    
    def io_read_all(self) -> List[Level]:
        """
        读取全部 IO 状态
        
        Returns:
            6 个通道的电平列表
        """
        levels = [Level.LOW] * 6
        resp = self.send_command("IO READALL")
        
        if not resp.success:
            return levels
        
        # 解析 "IO1 HIGH IO2 LOW ..." 格式
        for i in range(6):
            key = f"IO{i+1}"
            if key in resp.payload:
                pos = resp.payload.find(key)
                if "HIGH" in resp.payload[pos:]:
                    levels[i] = Level.HIGH
        
        return levels
    
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
        # 简化处理，实际应解析响应
        return PWMConfig(0, 0, False, False)
    
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
        关闭 PWM 定时回调
        
        Args:
            ch: 通道名称
        """
        self.send_command(f"PWM TICK {ch} OFF")
    
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
        
        # 解析 HEX:xxxx 格式
        if "HEX:" in resp.payload:
            hex_str = resp.payload.split("HEX:")[1].strip()
            return bytes.fromhex(hex_str)
        return b""
    
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
        hex_data = " ".join([f"0x{b:02X}" for b in data])
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
        hex_data = " ".join([f"0x{b:02X}" for b in data])
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
            return ADCReading(0, 0)
        
        # 解析 "2048 1650mV" 格式
        parts = resp.payload.split()
        if len(parts) >= 2:
            raw = int(parts[0])
            mv_str = parts[1].replace("mV", "")
            mv = int(mv_str)
            return ADCReading(raw, mv)
        
        return ADCReading(0, 0)
    
    def adc_read_all(self) -> List[ADCReading]:
        """
        读取全部 ADC 通道
        
        Returns:
            3 个通道的读数列表
        """
        readings = [ADCReading(0, 0)] * 3
        resp = self.send_command("ADC READALL")
        
        if not resp.success:
            return readings
        
        # 解析多行响应
        lines = resp.payload.split('\n')
        for i, line in enumerate(lines[:3]):
            parts = line.strip().split()
            if len(parts) >= 2:
                try:
                    raw = int(parts[0])
                    mv_str = parts[1].replace("mV", "")
                    mv = int(mv_str)
                    readings[i] = ADCReading(raw, mv)
                except (ValueError, IndexError):
                    pass
        
        return readings
    
    def adc_start_sample(self, ch: str, rate_hz: int, duration_ms: int = 0):
        """
        启动连续采样
        
        Args:
            ch: 通道名称
            rate_hz: 采样率（Hz）
            duration_ms: 持续时间（毫秒，0=持续直到手动停止）
        """
        cmd = f"ADC SAMPLE {ch} {rate_hz}"
        if duration_ms > 0:
            cmd += f" {duration_ms}"
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
        """
        注册事件回调
        
        Args:
            callback: 回调函数，参数为 (event_name, data)
        """
        self._event_callback = callback
    
    def start_event_listener(self):
        """启动事件监听线程"""
        if self._listening:
            return
        
        self._listening = True
        self._listen_thread = threading.Thread(target=self._listen_loop, daemon=True)
        self._listen_thread.start()
    
    def stop_event_listener(self):
        """停止事件监听"""
        self._listening = False
        if self._listen_thread and self._listen_thread.is_alive():
            self._listen_thread.join(timeout=1.0)
            self._listen_thread = None
    
    def _listen_loop(self):
        """事件监听循环"""
        while self._listening and self.is_open():
            try:
                line = self.serial.readline().decode('utf-8', errors='ignore').strip()
                
                if line and line.startswith("EVT "):
                    # 解析 "EVT NAME data" 格式
                    parts = line[4:].split(" ", 1)
                    if len(parts) == 2:
                        event, data = parts
                        if self._event_callback:
                            self._event_callback(event, data)
            except Exception:
                pass
    
    # ========================================================================
    # 工具函数
    # ========================================================================
    
    def send_command(self, cmd: str) -> Response:
        """
        发送命令并获取响应
        
        Args:
            cmd: 命令字符串
        
        Returns:
            响应对象
        """
        resp = Response(success=False, command=cmd, payload="")
        
        if not self.is_open():
            resp.error = "串口未打开"
            return resp
        
        try:
            # 清空接收缓冲
            self.serial.reset_input_buffer()
            
            # 发送命令
            full_cmd = cmd + "\n"
            self.serial.write(full_cmd.encode('utf-8'))
            
            # 读取响应
            line = self.serial.readline().decode('utf-8', errors='ignore').strip()
            
            if not line:
                resp.error = "响应超时"
                return resp
            
            return self._parse_response(line)
            
        except Exception as e:
            resp.error = str(e)
            return resp
    
    def _parse_response(self, line: str) -> Response:
        """解析响应行"""
        resp = Response(success=False, command="", payload="")
        
        if line.startswith("OK "):
            resp.success = True
            resp.payload = line[3:]
            
            # 提取命令名
            parts = resp.payload.split(" ", 1)
            if len(parts) > 1:
                resp.command = parts[0]
                resp.payload = parts[1]
            else:
                resp.command = resp.payload
                resp.payload = ""
                
        elif line.startswith("ERR "):
            resp.success = False
            content = line[4:]
            
            # 解析 "CMD E_CODE message" 格式
            parts = content.split(" ", 2)
            if len(parts) >= 2:
                resp.command = parts[0]
                code_str = parts[1]
                resp.error_code = self.ERROR_CODES.get(code_str, -1)
                resp.error = parts[2] if len(parts) > 2 else code_str
            else:
                resp.error = content
        else:
            # 可能是多行响应
            resp.success = True
            resp.payload = line
        
        return resp


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
