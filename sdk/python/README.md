# IO Docking Board Python SDK

基于 RP2040 IO 扩展板的 PC 控制库，实现文本模式协议，覆盖全部外设控制。

## 文件结构

```
sdk/python/
├── iodock.py           # SDK 主模块（类定义、接口实现）
├── demo.py             # 调用示例（完整演示所有功能）
├── requirements.txt    # 依赖列表
└── README.md           # 本文件
```

## 安装依赖

```bash
pip install -r requirements.txt
```

## 快速开始

### 运行示例

```bash
# Windows
python demo.py COM3

# Linux
python demo.py /dev/ttyACM0
```

## SDK 使用指南

### 1. 导入模块

```python
from iodock import IODock, Level, Direction, PullMode, Edge, Polarity
```

### 2. 创建连接

```python
dock = IODock("COM3")  # Windows
# dock = IODock("/dev/ttyACM0")  # Linux

if not dock.open():
    print("连接失败")
    exit(1)

if not dock.ping():
    print("板卡无响应")
    exit(1)
```

### 3. IO 控制

```python
# 配置 IO1 为输出
dock.io_config("IO1", Direction.OUTPUT)

# 输出低电平（LED 亮）
dock.io_write("IO1", Level.LOW)

# 翻转输出
dock.io_toggle("IO1")

# 输出 200ms 脉冲
dock.io_pulse("IO1", Level.LOW, 200)

# 配置 IO2 为上拉输入
dock.io_config("IO2", Direction.INPUT, PullMode.PULLUP)

# 读取输入电平
level = dock.io_read("IO2")

# 读取全部 IO
levels = dock.io_read_all()
```

### 4. PWM 控制

```python
# 配置 PWM1: 1kHz, 50% 占空比
dock.pwm_config("PWM1", 1000, 50)

# 启动 PWM
dock.pwm_start("PWM1")

# 修改占空比
dock.pwm_set_duty("PWM1", 75)

# 呼吸灯效果
dock.pwm_sweep("PWM1", 0, 100, 10, 0)  # 无限循环

# 多路同步
dock.pwm_config("PWM2", 1000, 50)
dock.pwm_sync(["PWM1", "PWM2"])

# 相位偏移
dock.pwm_set_phase("PWM2", "PWM1", 90)  # 滞后90°

# 停止
dock.pwm_stop("PWM1")
```

### 5. UART 控制

```python
# 配置 USART1: 9600 8N1
dock.uart_config("USART1", 9600)

# 发送文本
dock.uart_send_text("USART1", "Hello")

# 发送十六进制数据
dock.uart_send_hex("USART1", b'\x01\x02\x03')

# 读取接收缓冲
data = dock.uart_receive("USART1")

# 启用透传模式
dock.uart_stream("USART1", True)
```

### 6. I2C 控制

```python
# 设置速率
dock.i2c_set_speed(400)  # 400kHz

# 扫描总线
addrs = dock.i2c_scan()

# 读取寄存器
data = dock.i2c_read_reg(0x68, 0x3B, 6)  # MPU6050 加速度

# 写寄存器
dock.i2c_write_reg(0x68, 0x6B, b'\x00')  # 唤醒 MPU6050
```

### 7. SPI 控制

```python
# 配置 SPI
dock.spi_config(1000000, 0, "MSB", 8)  # 1MHz, 模式0

# 全双工传输
rx = dock.spi_transfer(b'\xDE\xAD\xBE\xEF')

# 手动控制 CS
dock.spi_cs("LOW")
dock.spi_write(b'\x01\x02')
dock.spi_cs("HIGH")
```

### 8. ADC 控制

```python
# 单次读取
val = dock.adc_read("ADC0")
print(f"{val.raw} ({val.millivolts} mV)")

# 读取温度
temp = dock.adc_read_temp()

# 连续采样
dock.adc_start_sample("ADC0", 1000, 1000)  # 1kHz, 1秒

# 阈值事件
dock.adc_threshold("ADC0", 1000, 2000, True)
```

### 9. 组合时序

```python
# 定义序列
dock.seq_define("blink", "IO IO1 LOW@0; WAIT 200; IO IO1 HIGH@200; WAIT 200")

# 执行序列（循环3次）
dock.seq_run("blink", 3)

# 列出序列
seqs = dock.seq_list()

# 删除序列
dock.seq_delete("blink")
```

### 10. 事件监听

```python
# 注册回调
dock.on_event(lambda event, data: print(f"事件: {event} - {data}"))

# 启动监听
dock.start_event_listener()

# 使能 IO 事件
dock.io_event("IO2", True, Edge.FALLING)

# ... 等待事件 ...

# 停止监听
dock.stop_event_listener()
```

### 11. 关闭连接

```python
dock.close()
```

## 错误处理

SDK 使用 `Response` 数据类返回命令结果：

```python
resp = dock.send_command("IO READ IO1")
if resp.success:
    print(f"成功: {resp.payload}")
else:
    print(f"失败: {resp.error} (错误码: {resp.error_code})")
```

## 注意事项

1. **串口权限**：Linux 下可能需要 `sudo` 或添加用户到 `dialout` 组
2. **波特率**：USB CDC 虚拟串口波特率不影响实际通信速率
3. **行缓冲**：单行命令最大 512 字节
4. **线程安全**：SDK 内部使用线程锁，支持多线程调用
5. **事件监听**：需要单独线程运行，回调在监听线程中执行

## 协议参考

完整协议文档见 `Doc/Host_Protocol.md`
