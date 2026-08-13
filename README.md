# IO Dock · 面向电脑的 IO 扩展坞

IO Dock 是一款**即插即用的电脑外设扩展坞**。它通过一根 USB 线接入电脑,变成一个**免驱动的虚拟串口**,让您无需任何硬件或嵌入式开发经验,只用几条简单的文本命令就能:

- 控制数字 IO —— 输入/输出、脉冲、电平翻转、边沿事件上报
- 输出 PWM —— 频率、占空比、相位可调,支持多路同步与渐变
- 收发串口 —— UART 收发与透传桥
- 读写总线外设 —— I2C / SPI
- 采集模拟量 —— ADC 单次/连续采样,外加板载温度
- 编排动作序列 —— 把上述操作按时序组合成脚本执行

所有操作都由电脑发起,板卡实时执行并回传数据与事件。

## 能力一览

| 功能 | 规格 |
| --- | --- |
| 数字 IO | 6 路,可配置输入/输出、上拉/下拉/浮空、边沿中断事件 |
| PWM | 4 路,频率/占空比/相位可调,支持多路同步启动、渐变(呼吸灯) |
| UART | 2 路,波特率/数据位/校验/停止位可配,支持接收透传 |
| I2C | 1 路,主模式,总线扫描 / 寄存器读写(100k / 400k / 1M) |
| SPI | 1 路,全双工,模式/位序/位宽可配 |
| ADC | 3 路,12 位,单次读取或连续采样流(最高约 500 kSPS) |
| 温度 | 板载温度传感器 |
| 事件 | IO 变化、UART 数据、ADC 阈值、序列完成等实时主动上报 |

> 完整命令与参数范围见 [协议手册](#协议)。

## 快速上手

### 1. 连接

把板卡用 USB 线接入电脑。系统会识别出一个虚拟串口(Windows 为 `COMx`,Linux / macOS 为 `/dev/ttyACMx`),**无需安装驱动**。

### 2. 使用(二选一)

**方式 A · Web 控制台(推荐,零安装)**

用 Chrome 或 Edge 直接打开 [web/index.html](web/index.html),点击「连接」选择对应串口,即可图形化操作全部功能。

> 若以 `file://` 方式打开时无法使用串口,可在该目录运行 `python -m http.server` 后访问 `http://localhost:8000`。

**方式 B · 任意串口终端**

用任意串口工具(如 PuTTY、串口助手、`pyserial` 脚本)以 `115200 8N1` 连接,直接输入命令:

```
> PING
OK PING PONG 0.1.3
> IO CFG IO1 OUT
OK IO CFG
> IO WRITE IO1 HIGH
OK IO WRITE
> PWM CFG PWM1 1000 50
OK PWM CFG PWM1 1000Hz 50%
> PWM START PWM1
OK PWM START PWM1
> ADC READ ADC0
OK ADC READ ADC0 2048 1650mV
> SEQ DEF blink "IO IO1 LOW@0; WAIT 200; IO IO1 HIGH@200; WAIT 200"
OK SEQ DEF
> SEQ RUN blink 0
OK SEQ RUN
```

## 协议

板卡提供两种编码,覆盖同一套功能,并都支持事件主动上报:

- **文本模式(默认)**:人类可读的行命令,适合调试与快速验证。完整手册见 [protocol/protocol.html](protocol/protocol.html)。
- **二进制模式(高速)**:紧凑二进制帧 + CRC 校验,适合自动化与大流量场景。发 `BIN ENTER` 切换进入,手册见 [protocol/protocol_bin.html](protocol/protocol_bin.html)。

事件会实时推送给上位机,例如 IO 电平变化(`EVT GPIO`)、UART 收到数据(`EVT UART_RX`)、ADC 越过阈值(`EVT ADC_THRESH`)、序列执行完成(`EVT SEQ_DONE`)等。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| [web/index.html](web/index.html) | Web 控制台(单文件、零依赖,Web Serial + 文本协议) |
| [protocol/protocol.html](protocol/protocol.html) | 文本协议交互式手册 |
| [protocol/protocol_bin.html](protocol/protocol_bin.html) | 二进制协议交互式手册 |
| [demo/demo_mpu6050_iic.html](demo/demo_mpu6050_iic.html) | 示例:通过 I2C 实时读取 MPU6050 六轴数据并绘制曲线 |
| [demo/demo_lvi_handle.html](demo/demo_lvi_handle.html) | 示例:LIV 手持设备的定时同步(多路 PWM 相位对齐) |
| [firmware/io_dock-v0.1.uf2](firmware/io_dock-v0.1.uf2) | 预编译固件 |

## 烧录固件

烧录分两步:先让板卡进入烧写模式(变成一个 U 盘),再把固件文件拖入。

**进入烧写模式(任选其一)**

- **Web 控制台**:打开 [web/index.html](web/index.html),切换到「⚙️ 系统」标签,点击「进入烧写模式」。
- **串口命令**:在串口发送 `BOOTLOADER` 命令。
- **物理方式**:按住板载 BOOT 键上电。

板卡进入烧写模式后会枚举为一个 U 盘。把 [firmware/io_dock-v0.1.uf2](firmware/io_dock-v0.1.uf2) 拖入该 U 盘,板卡自动重启,烧录完成。

## 典型场景

- **自动化测试 / 产测**:脚本控制 IO 电平、PWM 激励、ADC 采集,批量验证电路
- **传感器数据采集**:ADC 连续采样或 I2C 读取传感器,数据流式回传绘制、落盘
- **外设通信桥**:UART / I2C / SPI 作为电脑到目标器件的透传中转
- **教学与原型验证**:不写硬件代码即可操作引脚,快速验证电路与逻辑
- **组合时序**:上电时序、按键脉冲序列、PWM 呼吸灯等定时动作

---

固件版本:v0.1 · [CreavisionTech/IO-Docking](https://github.com/CreavisionTech/IO-Docking)
