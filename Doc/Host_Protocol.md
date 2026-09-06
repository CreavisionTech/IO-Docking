# PC 控制协议设计(Host Protocol)

> **标准固件 0.1.4 实现补充（2026-09-06）**：文本与二进制版本号统一；请求参数严格校验。
> ADC 连续采样为 FIFO 中断加软件缓冲（1～500 kSPS 请求范围），最高采样率不等同 USB 无损吞吐。
> 采样期间其他 ADC 读取返回 `E_BUSY`；序列或采样期间 Flash 保存/加载/清除返回 `E_BUSY`。
> 标准版配置布局版本 2 不加载旧布局。完整实测边界见 `firmware/validation/REPORT.md`。

新增事件（原有事件格式保持兼容）：

| 文本事件 | 二进制 EVT OP | 载荷 |
| --- | --- | --- |
| `EVT ADC_OVERRUN ADCx N` | `0x66` | `[ch:u8][lost_at_least:u32]`，丢样数下界；硬件 FIFO 溢出时无法精确计数 |
| `EVT TX_OVERRUN N` | `0x07` | `[dropped_writes:u32]`，USB 发送队列过载后整块丢弃次数 |
| `EVT SEQ_ERROR name E_xxx` | `0x78` | `[error:u8][name_len:u8][name]`，驱动执行失败，中止后续步骤 |

二进制 `DATA_END` 的 `0x62` 载荷为实际交给发送通道的样本数 `u32`。发生 `TX_OVERRUN` 时接收数量可能更少。
`SEQ_DONE` 表示正常完成或主动停止；驱动失败改发 `SEQ_ERROR`。
SPI LSB 由软件实现；9～16 位模式按小端两字节一个字传输，拒绝奇数字节数量。

> 版本:v0.1(设计稿,待固件实现)
> 定位:定义 PC 与 IO Docking Board 之间用于控制与数据采集的通信协议。

## 1. 概述

板卡以 RP2040 为主控,通过 USB 与 PC 通信。PC 端将板卡识别为一个**虚拟串口**(USB CDC-ACM),Windows/macOS/Linux 均免驱动。

设计目标:

- 不需要专业知识,用任意串口终端即可控制;
- 命令人类可读、可脚本化、可自动化测试;
- 覆盖板卡全部外设的配置、控制、读取与连续数据采集;
- 支持异步事件上报(IO 变化、ADC 阈值、UART 数据、序列完成),便于实时应用;
- 同一套功能提供**两种编码**:文本行协议(开发者友好、可读可调试)与二进制协议(紧凑、高速,面向自动化与大流量,见 [第 11 节](#11-二进制协议高速模式))。

## 2. 物理与链路层

| 项目 | 值 |
| --- | --- |
| USB 标准 | USB 1.1 Full-Speed,12 Mbps |
| 设备类 | CDC-ACM(虚拟串口) |
| 系统识别 | Windows:`COMx`;macOS/Linux:`/dev/ttyACM*` |
| 驱动 | 系统内置,免安装 |
| 推荐串口设置 | 115200 8N1(USB CDC 下波特率仅作记录,不影响实际速率) |

> USB CDC 的实际吞吐受 TinyUSB 端点缓冲限制,稳定速率约 100~900 KB/s。高速大流量场景(如连续采样、UART 透传)在固件侧用 DMA + 双缓冲保证,协议对数据流做了分帧(见第 4.4 节)。

## 3. 帧格式(文本行协议)

- 每条命令一行,以 `\n` 或 `\r\n` 结尾;板卡返回一行(或多行)响应;
- 命令与参数以空白分隔;**参数内不允许裸空格**——需要携带任意字节时用 `TXT:` / `HEX:` 前缀(见 4.4、第 6 节);
- 命令名不区分大小写;参数中通道名、`ON/OFF`、`HIGH/LOW` 等关键字不区分大小写;
- 单行最大长度 512 字节,超长丢弃并回 `ERR <CMD> E_LONG`;
- 空行忽略;`#` 开头为注释行。

通用形式:`<CMD> <ARG1> <ARG2> ...`

> 本节描述的是**文本模式**帧格式。板卡默认运行在文本模式;二进制(高速)模式的帧格式与命令集见 [第 11 节](#11-二进制协议高速模式)。

## 4. 响应与事件

### 4.1 成功响应

```
OK <CMD> <payload>
```

### 4.2 错误响应

```
ERR <CMD> <ERRCODE> <message>
```

### 4.3 异步事件(板卡主动上报)

```
EVT <NAME> <payload>
```

### 4.4 数据流

连续采样/透传数据使用独立 TAG 行,便于上位机区分命令响应与流数据:

```
DATA <STREAM> <payload>
DATA_END <STREAM>
```

凡需要携带任意字节的 payload,一律用 **HEX 编码**(16 进制字节对),避免与行结束符、控制字符冲突。例如 UART 收到的原始字节以 `HEX:0A0D01FF` 形式上报。

## 5. 错误码表

| 码 | 含义 |
| --- | --- |
| `E_BADCMD` | 未知命令 |
| `E_PARAM` | 参数数量或格式错误 |
| `E_NOTFOUND` | 通道/对象不存在 |
| `E_RANGE` | 数值超出范围 |
| `E_CFG` | 未配置或配置冲突 |
| `E_BUSY` | 上一条异步操作仍在进行 |
| `E_TIMEOUT` | 总线操作超时(如 I2C/SPI 无应答) |
| `E_NACK` | I2C 从机无应答(收到 NACK) |
| `E_OVERRUN` | 缓冲溢出 / 采样丢失 |
| `E_IO` | 硬件错误 |
| `E_LONG` | 命令行过长 |
| `E_DENIED` | 当前状态不允许该操作 |

## 6. 命令集

通道命名与引脚映射见 `Doc/RP2040_IO.md`。通道别名:IO1~IO6、PWM1~PWM4、USART1/USART2、IIC0、SPI0、ADC0~ADC2。

### 6.1 系统

| 命令 | 说明 | 响应示例 |
| --- | --- | --- |
| `HELP` | 列出全部命令与用法 | `OK HELP`(后跟命令列表) |
| `INFO` | 板卡与固件信息(多行) | `OK INFO` + 键值行 |
| `PING` | 在线检查 | `OK PING PONG` |
| `SYNC <host_us>` | **时间同步(单向)**:主机发送自身时间,从机记录本地接收时刻并设 `offset=host_us-local`,返回偏移 | `OK SYNC offset=.. local=.. synced=..` |
| `TIME` | 查询本地/偏移/同步后时间 | `OK TIME local=.. offset=.. synced=..` |
| `RESET` | 软复位板卡 | `OK RESET` |
| `BOOTLOADER` | 进入 UF2 下载模式(USB 枚举为存储盘) | `OK BOOTLOADER` |
| `LED <ON\|OFF>` | 板载控制通道指示灯 | `OK LED` |

`INFO` 响应格式(键值对,每行一项):

```
OK INFO
 board=IO-Dock
 mcu=RP2040
 fw=0.1.0
 uid=<8位HEX序列号>
 clk=125000000
 io=6 pwm=4 uart=2 i2c=1 spi=1 adc=3
END
```

### 6.2 IO(6 路)

| 命令 | 说明 |
| --- | --- |
| `IO CFG <IOx> <IN\|OUT> [PULLUP\|PULLDOWN\|FLOAT]` | 配置方向与输入上下拉。示例:`IO CFG IO1 IN PULLUP` |
| `IO WRITE <IOx> <HIGH\|LOW>` | 输出电平 |
| `IO READ <IOx>` | 读输入电平 |
| `IO READALL` | 读全部 6 路,每路一行 |
| `IO TOGGLE <IOx>` | 翻转输出 |
| `IO PULSE <IOx> <HIGH\|LOW> <ms>` | 输出指定宽度的脉冲(非阻塞,先置电平,到时还原) |
| `IO EVENT <IOx> <ON\|OFF> [RISING\|FALLING\|CHANGE]` | 使能/关闭输入变化事件上报 |

响应示例:

```
> IO READ IO1
OK IO READ IO1 HIGH
> IO READALL
OK IO READALL
 IO1 HIGH IO2 LOW IO3 LOW IO4 HIGH IO5 HIGH IO6 LOW
END
```

事件(输入变化时上报):

```
EVT GPIO IO1 LOW
EVT GPIO IO3 HIGH
```

实现备注:RP2040 每路 GPIO 均可产生中断(RISING/FALLING/CHANGE);电平触发(RISING/FALLING)对应边沿中断,`CHANGE` 为双边沿。事件带 50ms 消抖,防抖时间作为固件可配置参数(后续可加 `IO DEBOUNCE <IOx> <ms>`)。

### 6.3 PWM(4 路)

| 命令 | 说明 |
| --- | --- |
| `PWM CFG <PWMx> <freq_Hz> <duty>` | 配置频率与占空比并停止。`duty`:0~100 为百分比;`#N`(0~65535)为 16 位原始值 |
| `PWM FREQ <PWMx> <freq_Hz>` | 修改频率(运行中生效) |
| `PWM DUTY <PWMx> <duty>` | 修改占空比(运行中生效) |
| `PWM START <PWMx>` | 启动输出 |
| `PWM STOP <PWMx>` | 停止(输出保持最后电平或归零,由配置决定) |
| `PWM SWEEP <PWMx> <from_duty> <to_duty> <step_ms> [repeat]` | 渐变:从起始占空比按步进间隔扫到目标,`repeat` 为重复次数(0=无限) |
| `PWM READ <PWMx>` | 查询当前频率/占空比/极性/运行状态 |
| `PWM SYNC <PWMx>,<PWMx>,...` | 同步重启列出的通道:停止 → 对齐计数器初值 → 经全局使能 `PWM_EN` 同时启动,此后相位锁定。各通道须同频,否则 `E_CFG` |
| `PWM PHASE <PWMx> <ref> <deg>` | 设置 `PWMx` 相对 `ref` 的相位偏移(0~360°),自动按当前频率换算为计数器 ticks、写入初值并重新同步两者。要求同频;同片 A/B(本板 PWM2/PWM3)不支持任意相位,仅 0°/180° |
| `PWM PHADJ <PWMx> <delta>` | 运行中在线微调相位,`delta` 为计数器 ticks(可负),1 tick = 360/(TOP+1) 度。要求该片分频器 > 1,否则 `E_DENIED` |
| `PWM POL <PWMx> <NORMAL\|INVERT>` | 输出极性;`INVERT` 等效把该通道相位翻转 180° |
| `PWM TICK <PWMx> <freq> \| OFF` | **1Hz 回调 + 真实输出**:把该路 PWM 配为基频并开启计数器翻转中断,每周期上报 `EVT PWM_TICK <计数>`。freq 低于硬件 ~7.45Hz 下限时,选中通道的引脚改由 ISR 软件翻转,输出**真实 `freq`-Hz 方波**(50% 占空比;如 `PWM TICK PWM3 1` 让 GPIO9 真正输出 1Hz)。`OFF` 关闭并恢复引脚为 PWM 功能 |

示例:

```
> PWM CFG PWM1 1000 50
OK PWM CFG PWM1 1000Hz 50%
> PWM START PWM1
OK PWM START PWM1
> PWM SWEEP PWM1 0 100 10 0      # 呼吸灯,无限循环
OK PWM SWEEP PWM1
> PWM PHASE PWM2 PWM1 90        # PWM2 相对 PWM1 相位滞后 90°
OK PWM PHASE PWM2 90° (ref PWM1)
```

实现备注:RP2040 PWM 为 8 片 × 2 通道,16 位计数器 + 8.4 分频。本板 4 路:PWM1=GPIO7(片3B)、PWM2=GPIO8(片4A)、PWM3=GPIO9(片4B)、PWM4=GPIO10(片5A)。**注意 PWM2 与 PWM3 同属片 4**:共享计数器,频率必须相同、相位天然对齐(不可任意移相,仅可经极性反转取 180°);PWM1/PWM4 各自独立片。频率范围约 7.5Hz~62.5MHz(clk_sys=125MHz),占空比 16 位精度。`PWM SWEEP` 由定时器逐步写 CC 寄存器实现,步进期间 CPU 开销低。

**相位控制(跨片)**:`PWM SYNC` 经全局使能寄存器 `PWM_EN` 同时启动多片进入 lockstep,相位由各片计数器初值决定(写 `CH_CTR` 设初值);`PWM PHASE` 完成换算 + 写初值 + 重新同步;`PWM PHADJ` 在运行中用 `CSR_PH_ADV`/`CSR_PH_RET` 按计数在线推进/后退(每写一次恰好 1 个计数,要求分频器 > 1)。所有片共用 clk_sys,相位一旦建立即稳定不漂移。

### 6.4 UART(2 路)

| 命令 | 说明 |
| --- | --- |
| `UART CFG <USARTx> <baud> [databits] [parity] [stopbits]` | 配置。`parity`:N/O/E;`databits` 5~8;`stopbits` 1/2。示例:`UART CFG USART1 9600 8 N 1` |
| `UART TX <USARTx> <TXT:...\|HEX:...>` | 发送。文本或十六进制 |
| `UART RX <USARTx> [n]` | 读取接收缓冲(最多 n 字节,默认全部),以 `HEX:` 返回 |
| `UART FLUSH <USARTx>` | 清空接收缓冲 |
| `UART STREAM <USARTx> <ON\|OFF>` | 打开/关闭接收透传:打开后收到数据实时以 `EVT UART_RX` 上报 |
| `UART STAT <USARTx>` | 查询缓冲字节数、错误计数 |

示例:

```
> UART TX USART1 TXT:Hello
OK UART TX USART1 5
> UART RX USART1
OK UART RX USART1 HEX:48656C6C6F
> UART STREAM USART1 ON
OK UART STREAM USART1 ON
```

透传事件:

```
EVT UART_RX USART1 HEX:0A0D41
```

实现备注:RP2040 集成 PL011 UART,硬件 32 字节 TX/RX FIFO;固件用中断 + 软件环形缓冲,流控命令 UART/CTS 本板未引出。最大波特率 UARTCLK/16 ≈ 7.8Mbps(clk_sys=125MHz)。透传时若 USB 拥塞,固件以 `E_OVERRUN` 事件提示丢包:`EVT UART_OVERRUN USART1 <丢弃字节数>`。

### 6.5 I2C(1 路)

所有 `addr`、`reg`、`data` 均为十六进制(不带 `0x` 或带均可)。

| 命令 | 说明 |
| --- | --- |
| `I2C CFG <rate>` | 设置速率,`rate` 为 100 / 400 / 1000(kHz) |
| `I2C SCAN` | 扫描总线,列出应答的 7 位地址 |
| `I2C WRITE <addr> <reg> <data...>` | 写寄存器地址后跟数据字节(寄存器寻址) |
| `I2C WRONLY <addr> <data...>` | 无寄存器地址直写 |
| `I2C READ <addr> <reg> <n>` | 写寄存器地址后读 n 字节 |
| `I2C RONLY <addr> <n>` | 无寄存器地址直读 n 字节 |
| `I2C WR <addr> <reg> <n>` | 先写后读(同 READ,保留接口以明确语义) |

示例:

```
> I2C CFG 400
OK I2C CFG 400kHz
> I2C SCAN
OK I2C SCAN
 0x3C 0x50 0x68
END
> I2C READ 0x3C 0x00 4
OK I2C READ HEX:01234567
> I2C WRITE 0x50 0x01 0AA 0BB
OK I2C WRITE
```

无应答或超时:

```
ERR I2C WRITE E_NACK addr=0x3C
```

实现备注:RP2040 I2C0 支持标准/快速/快速+(100k/400k/1M),16 级收发缓冲,含 DMA。板载上拉,长总线建议外接上拉。地址统一用 7 位格式。固件需处理 NACK、时钟拉伸超时,并支持重启条件(restart)。

### 6.6 SPI(1 路)

| 命令 | 说明 |
| --- | --- |
| `SPI CFG <baud> [mode] [bitorder] [bits]` | 配置。`mode` 0~3(CPOL/CPHA);`bitorder` MSB/LSB;`bits` 4~16。示例:`SPI CFG 1000000 0 MSB 8` |
| `SPI XFR <HEX:...>` | 全双工传输:同时发送并接收,返回收到的数据 |
| `SPI WRITE <HEX:...>` | 只发送(丢弃接收) |
| `SPI READ <n>` | 只接收 n 字节(发送 0x00) |
| `SPI CS <AUTO\|HIGH\|LOW>` | CS 控制:`AUTO` 传输期间自动拉低;手动模式可显式拉高/拉低 |

示例:

```
> SPI XFR HEX:DEADBEEF
OK SPI XFR HEX:00000000
> SPI CS LOW
OK SPI CS LOW
```

实现备注:RP2040 SPI0 为 PL022 SSP,主模式最高约 clk_peri/2(66.5MHz@133MHz)。本板 CS 使用 GPIO6 软件控制。模式位序等见 PL022 协议。全双工 XFR 一次传输长度上限受 USB 行缓冲(512B)约束,大块数据可分多次。

### 6.7 ADC(3 路 + 温度)

| 命令 | 说明 |
| --- | --- |
| `ADC READ <ADCx>` | 单次读取,返回原始 12 位值 + 电压(mV) |
| `ADC READALL` | 读取全部 3 路 |
| `ADC SAMPLE <ADCx> <rate_Hz> <ms> [ON]` | 连续采样指定时长,实时以 `DATA ADCx` 流上报。省略 `ms` 可先开始、后用 `ADC SAMPLE <ADCx> STOP` 停止 |
| `ADC TEMP` | 读取片上温度传感器(ADC 通道 4) |
| `ADC THRESH <ADCx> <low_mV> <high_mV> <ON\|OFF>` | 阈值事件。采样值跨出 [low,high] 时上报;`0 0` 表示仅用单侧 |

示例:

```
> ADC READ ADC0
OK ADC READ ADC0 2048 1650mV
> ADC SAMPLE ADC0 1000 100
OK ADC SAMPLE ADC0
DATA ADC0 2048,2050,2047,2051,2049,...
DATA ADC0 ...
DATA_END ADC0
> ADC TEMP
OK ADC TEMP 27.3C
```

阈值事件:

```
EVT ADC_THRESH ADC0 ABOVE 1700mV
EVT ADC_THRESH ADC0 BELOW 1600mV
```

实现备注:RP2040 ADC 为 12 位、500ksps(独立 48MHz 时钟)、8 级 FIFO + 可编程定速器,支持 round-robin 多通道。`DATA` 行每次打包若干样本(如 32 个)用逗号分隔的原始 12 位值;上位机自行换算电压:`mV = raw × 3300 / 4095`(参考 IOVDD≈3.3V)。温度换算公式:`T = 27 − (V − 0.706) / 0.001721`,V 为通道 4 电压。

### 6.8 组合时序动作(SEQ)

一个**序列**是一组按时序执行的操作。序列定义后存入固件 RAM,可重复执行。

**序列语法**

```
SEQ DEF <name> "<step>;<step>;..."
```

每条 `step` 的通用形式:`<操作> [@绝对毫秒] | [+相对毫秒]`,缺省为紧随上一步(相对 +0)。操作类型:

| 操作 | 说明 |
| --- | --- |
| `IO <IOx> <HIGH\|LOW>` | 置 IO 电平 |
| `PULSE <IOx> <HIGH\|LOW> <ms>` | IO 脉冲(置电平、延时、还原) |
| `PWM <PWMx> <duty>` | 设置 PWM 占空比 |
| `SWEEP <PWMx> <from> <to> <step_ms>` | PWM 渐变 |
| `TX <USARTx> <TXT:..\|HEX:..>` | UART 发送 |
| `SAMPLE <ADCx> <rate_Hz> <ms>` | ADC 采样(数据流正常上报) |
| `WAIT <ms>` | 等待 |

**序列命令**

| 命令 | 说明 |
| --- | --- |
| `SEQ DEF <name> "<steps>"` | 定义/覆盖序列 |
| `SEQ LIST` | 列出已定义序列 |
| `SEQ SHOW <name>` | 打印序列内容 |
| `SEQ RUN <name> [n]` | 执行,`n` 为循环次数(0=无限),默认 1 |
| `SEQ STOP` | 停止当前序列 |
| `SEQ STAT` | 查询执行状态(空闲/运行/剩余次数/当前步骤) |
| `SEQ DEL <name>` | 删除序列 |

示例:

```
# 上电时序:IO1 拉高→50ms 后 PWM1 50%→500ms 后同步还原
> SEQ DEF boot_seq "IO IO1 HIGH@0; WAIT 50; PWM PWM1 50@50; WAIT 500; IO IO1 LOW@550; PWM PWM1 0@550"
OK SEQ DEF boot_seq
> SEQ RUN boot_seq
OK SEQ RUN boot_seq
```

序列执行完成上报:

```
EVT SEQ_DONE boot_seq
```

实现备注:时序由 RP2040 双核中的辅助核 + 定时器驱动,主核仍可处理其余命令。执行中的序列若被新 `SEQ RUN` 覆盖,先停止再启动。

### 6.9 扩展与预留

- `PIN REASSIGN <IOx> <FUNC>`(预留):将某路 IO 重映射到 PIO 自定义协议(如 WS2812、1-Wire、软串口、DHT),需固件按 PIO 程序注入;
- `GPIO <n> <...>`(预留):直控 GPIO20~25 的底层命令,供调试/高级用户使用;
- 二进制高速模式:已设计完成,见 [第 11 节](#11-二进制协议高速模式)。

## 7. 快速上手示例

一段完整会话(交互终端):

```
> PING
OK PING PONG
> INFO
OK INFO
 board=IO-Dock
 mcu=RP2040
 fw=0.1.0
 uid=1234ABCD
 clk=125000000
 io=6 pwm=4 uart=2 i2c=1 spi=1 adc=3
END
> IO WRITE IO1 LOW          # IO1 的 LED 低电平点亮
OK IO WRITE IO1
> PWM CFG PWM1 1000 50
OK PWM CFG PWM1 1000Hz 50%
> PWM START PWM1
OK PWM START PWM1
> UART TX USART1 TXT:Hello
OK UART TX USART1 5
> I2C READ 0x3C 0x00 4
OK I2C READ HEX:01234567
> ADC READ ADC0
OK ADC READ ADC0 2048 1650mV
```

## 8. 上位机使用建议

**Python + pyserial 最小示例:**

```python
import serial, time

s = serial.Serial("COM3", 115200, timeout=1)   # Linux: "/dev/ttyACM0"

def cmd(c: str) -> str:
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    time.sleep(0.05)
    return s.read_all().decode()

print(cmd("PING"))
print(cmd("IO WRITE IO1 LOW"))
print(cmd("ADC READ ADC0"))
```

要点:

- 命令写入后等待 `\n` 结尾的响应行;读取用 `read_until(b"\n")` 而非固定长度;
- 事件与数据流与命令响应共享同一通道,上位机按行前缀(`OK`/`ERR`/`EVT`/`DATA`/`DATA_END`)分派;
- 上位机库(规划中):`iodock` Python 包,封装本协议为对象式 API(`dock.io.write("IO1", False)`),并提供 CLI 工具与终端监视器。

## 9. 性能与设计边界

- **USB CDC 吞吐**:稳定约 100~900 KB/s。ADC 连续采样、UART 透传等高流量场景需注意固件侧环形缓冲水位,超限上报 `E_OVERRUN` 而非静默丢弃;
- **实时性**:GPIO 事件、PWM 渐变、SEQ 时序由中断/定时器/辅助核保证;文本命令解析在轮询循环完成,不应阻塞硬件数据路径;
- **行缓冲**:单行 512B,单次 SPI/I2C/UART 传输受此限制,大块数据分段;
- **掉电保持**:序列与配置存于 RAM,复位后清空;如需持久化,后续增加 flash 存储命令(`CFG SAVE` / `CFG LOAD`)。

## 10. 后续扩展(Backlog)

1. `CFG SAVE/LOAD`:配置与序列持久化到片外 flash;
2. PIO 自定义协议注入(`PIN REASSIGN` + PIO 程序下载);
3. 双缓冲流控与流暂停(`STREAM PAUSE/RESUME`);
4. 上位机 `iodock` Python 库与 CLI(提供文本与二进制两套后端)。

---

## 11. 二进制协议(高速模式)

> 与文本协议**共享同一功能集**,但以紧凑的二进制帧传输。面向:自动化测试、大数据量传输(ADC 连续采样、UART 透传、大块 SPI/I2C),以及需要低延迟多步执行的复杂任务。

### 11.1 与文本模式的关系

| | 文本模式 | 二进制模式 |
| --- | --- | --- |
| 适用对象 | 开发者 / 交互调试 | 自动化 / 高性能程序 |
| 帧 | 一行 ASCII 命令 | 二进制帧(SOF + LEN + CRC) |
| 数据 | HEX 文本,人可读 | 原始字节,紧凑 |
| 吞吐 | 受行缓冲与解析开销限制 | 大帧 + 原生解析,更接近 USB 上限 |
| 上手成本 | 低 | 中(需参考本节) |

**模式切换**:上电默认**文本模式**。发送文本命令 `BIN ENTER` 进入二进制模式——固件先回 `OK BIN`(文本),随后切换到二进制帧。在二进制模式下用系统命令 `MODE_EXIT`(0x0F)返回文本模式(先回二进制 RESP 再切换)。复位(RESET / 文本 `RESET`)回到文本模式。

> 模式**不自动混用**:避免解析歧义。二进制模式下若主机收到无法识别的帧,应发送 `RESET` 或重新插拔;固件在空闲超时后(默认 30s,可配置)自动回到文本模式,避免"卡死在二进制模式"。

### 11.2 帧格式

```
 偏移  大小  字段
 ──────────────────────────────────────────────
 0     1     SOF  = 0xCB,固定帧起始标记
 1     1     TYPE 帧类型(见 11.3)
 2     1     OP   操作码(命令编号;流/事件中为流标识)
 3     1     FLAGS bit0=1 表示后续还有帧(分片);其余保留=0
 4-5   2     LEN  载荷长度,u16 小端(最大 65535;默认上限 4096)
 6-7   2     SEQ  请求序号,u16 小端;响应原样回显;事件/流=0
 8..    LEN   PAYLOAD(载荷,小端编码)
 ..+LEN  2   CRC16-CCITT(0x1021,初值 0xFFFF),对偏移 0~(7+LEN) 全部字节计算,小端存放
```

- 默认最大载荷 4096 字节(可配置);超长传输用分片(FLAGS.bit0)或拆成多次请求。
- **重同步**:USB 传输可靠、帧为长度定界,无需转义。若主机丢步,扫描 `0xCB` 后再按 LEN + CRC 校验,不合法则跳过一字节继续找。

### 11.3 帧类型(TYPE)

| 值 | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| 0x01 | CMD | 主机→固件 | 命令请求 |
| 0x02 | RESP | 固件→主机 | 成功响应(与请求同 OP/SEQ) |
| 0x03 | ERR | 固件→主机 | 错误响应(与请求同 OP/SEQ) |
| 0x04 | EVT | 固件→主机 | 异步事件 |
| 0x05 | DATA | 固件→主机 | 数据流帧 |
| 0x06 | DATA_END | 固件→主机 | 数据流结束 |

### 11.4 错误响应载荷

```
[code:u8][msg_len:u8][msg:msg_len]
```

`code` 对应第 5 节错误码表(0 起编号):E_BADCMD=0, E_PARAM=1, E_NOTFOUND=2, E_RANGE=3, E_CFG=4, E_BUSY=5, E_TIMEOUT=6, E_NACK=7, E_OVERRUN=8, E_IO=9, E_LONG=10, E_DENIED=11。

### 11.5 通道与枚举编码

- 通道以 u8 索引表示:IO1~IO6 → 0~5;PWM1~PWM4 → 0~3;USART1/2 → 0/1;ADC0~2 → 0~2。
- 电平:HIGH=1, LOW=0;方向:OUT=0, IN=1;上拉:FLOAT=0, PULLUP=1, PULLDOWN=2;校验:PARITY_NONE=0, ODD=1, EVEN=2。
- 数值一律**小端**。频率/波特率为 u32(Hz);占空比为 u16(0~65535 原始值,不用百分比);时间 ms 为 u16/u32。
- 二进制数据直接原样传输,**无需 HEX 编码**。

### 11.6 命令与载荷规格

#### 系统(0x00~0x0F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x00 | PING | - | `[magic:u8=0x4F][fw:u8 u8 u8]` |
| 0x01 | INFO | - | 键值对,见下 |
| 0x02 | RESET | - | `[]`(随后复位) |
| 0x03 | BOOTLOADER | - | `[]`(随后进 UF2) |
| 0x0C | SYNC | `[host_us:u64]` | `[local:u64][offset:i64][synced:u64]`(时间同步) |
| 0x0D | TIME | - | `[local:u64][offset:i64][synced:u64]` |
| 0x04 | LED | `[on:u8]` | `[]` |
| 0x0F | MODE_EXIT | - | `[]`(随后切回文本模式) |

INFO 响应载荷:一系列 `[tag_len:u8][tag][val_len:u8][val]`,以 `tag_len=0` 结束。

#### IO(0x10~0x1F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x10 | IO_CFG | `[ch][dir][pull]` | `[]` |
| 0x11 | IO_WRITE | `[ch][level]` | `[]` |
| 0x12 | IO_READ | `[ch]` | `[ch][level]` |
| 0x13 | IO_READALL | - | `[levels:u8]`(bit0=IO1..bit5=IO6,1=HIGH) |
| 0x14 | IO_TOGGLE | `[ch]` | `[]` |
| 0x15 | IO_PULSE | `[ch][level][ms:u16]` | `[]` |
| 0x16 | IO_EVENT | `[ch][mode:u8]`(0=OFF,1=RISING,2=FALLING,3=CHANGE) | `[]` |

IO 事件:`TYPE=EVT, OP=0x16, [ch][level]`。

#### PWM(0x20~0x2F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x20 | PWM_CFG | `[ch][freq:u32][duty:u16]` | `[]` |
| 0x21 | PWM_FREQ | `[ch][freq:u32]` | `[]` |
| 0x22 | PWM_DUTY | `[ch][duty:u16]` | `[]` |
| 0x23 | PWM_START | `[ch]` | `[]` |
| 0x24 | PWM_STOP | `[ch]` | `[]` |
| 0x25 | PWM_SWEEP | `[ch][from:u16][to:u16][step_ms:u16][repeat:u16]` | `[]` |
| 0x26 | PWM_READ | `[ch]` | `[ch][freq:u32][duty:u16][running:u8][inv:u8]` |
| 0x27 | PWM_SYNC | `[n:u8][ch × n]` | `[]`(经 `PWM_EN` 同时启动,相位锁定) |
| 0x28 | PWM_PHASE | `[ch][ref][unit:u8](0=度,1=ticks)[val:u16]` | `[]` |
| 0x29 | PWM_PHADJ | `[ch][delta:i16]`(ticks,负=后退) | `[]` |
| 0x2A | PWM_POL | `[ch][inv:u8]` | `[]` |
| 0x2B | PWM_TICK | `[ch][freq:u32]`(freq=0 关闭) | `[]`(随后每周期 `EVT` 帧 op=0x2B `[count:u32]`) |

#### UART(0x30~0x3F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x30 | UART_CFG | `[ch][baud:u32][databits:u8][parity:u8][stopbits:u8]` | `[]` |
| 0x31 | UART_TX | `[ch][len:u16][data]` | `[sent:u16]` |
| 0x32 | UART_RX | `[ch][max:u16]` | `[ch][len:u16][data]` |
| 0x33 | UART_FLUSH | `[ch]` | `[]` |
| 0x34 | UART_STREAM | `[ch][on:u8]` | `[]` |
| 0x35 | UART_STAT | `[ch]` | `[rx_len:u16][tx_len:u16][errs:u32]` |

UART 透传事件:`TYPE=EVT, OP=0x34, [ch][len:u16][data]`;溢出:`TYPE=EVT, OP=0x37, [ch][dropped:u32]`。

#### I2C(0x40~0x4F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x40 | I2C_CFG | `[rate_khz:u32]` | `[]` |
| 0x41 | I2C_SCAN | - | `[count:u8][addr...]` |
| 0x42 | I2C_WRITE | `[addr:u8][reg:u8][len:u16][data]` | `[]` |
| 0x43 | I2C_WRONLY | `[addr:u8][len:u16][data]` | `[]` |
| 0x44 | I2C_READ | `[addr:u8][reg:u8][n:u16]` | `[len:u16][data]` |
| 0x45 | I2C_RONLY | `[addr:u8][n:u16]` | `[len:u16][data]` |
| 0x46 | I2C_WR | `[addr:u8][reg:u8][n:u16]` | `[len:u16][data]` |

#### SPI(0x50~0x5F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x50 | SPI_CFG | `[baud:u32][mode:u8][bitorder:u8][bits:u8]` | `[]` |
| 0x51 | SPI_XFR | `[len:u16][tx:len]` | `[len:u16][rx:len]` |
| 0x52 | SPI_WRITE | `[len:u16][tx:len]` | `[]` |
| 0x53 | SPI_READ | `[n:u16]` | `[len:u16][rx:len]` |
| 0x54 | SPI_CS | `[mode:u8]`(0=AUTO,1=LOW,2=HIGH) | `[]` |

#### ADC(0x60~0x6F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x60 | ADC_READ | `[ch]` | `[ch][raw:u16][mv:u16]` |
| 0x61 | ADC_READALL | - | `[raw0][mv0] [raw1][mv1] [raw2][mv2]`(各 u16) |
| 0x62 | ADC_SAMPLE_START | `[ch][rate:u32][ms:u32]` | `[]`,随后 DATA 流 |
| 0x63 | ADC_SAMPLE_STOP | `[ch]` | 先 DATA 余量,再 DATA_END |
| 0x64 | ADC_TEMP | - | `[m°C:i32]`(如 27300 = 27.3°C) |
| 0x65 | ADC_THRESH | `[ch][low_mv:u16][high_mv:u16][on:u8]` | `[]` |

ADC 数据流:`TYPE=DATA, OP=0x62, [count:u16][raw...:u16×count]`;结束:`TYPE=DATA_END, OP=0x62, [total:u32]`。阈值事件:`TYPE=EVT, OP=0x65, [ch][dir:u8](0=BELOW,1=ABOVE)[mv:u16]`。

#### SEQ(0x70~0x7F)

| OP | 命令 | 请求载荷 | 响应载荷 |
| --- | --- | --- | --- |
| 0x70 | SEQ_DEF | `[name_len:u8][name][script_len:u16][script]` | `[]` |
| 0x71 | SEQ_LIST | - | `[count:u8][name_len:u8][name]...` |
| 0x72 | SEQ_SHOW | `[name_len:u8][name]` | `[script_len:u16][script]` |
| 0x73 | SEQ_RUN | `[name_len:u8][name][repeat:u16]` | `[]` |
| 0x74 | SEQ_STOP | - | `[]` |
| 0x75 | SEQ_STAT | - | `[state:u8][name_len:u8][name][remaining:u32][step:u32]` |
| 0x76 | SEQ_DEL | `[name_len:u8][name]` | `[]` |

序列脚本复用 6.8 节的 mini 语言(UTF-8 字节),同一定义跨模式通用。序列由**辅助核**执行,主机无需等待序列结束即可发下一条命令——"一条命令执行复杂任务"。完成事件:`TYPE=EVT, OP=0x77, [name_len:u8][name]`。

#### 高级:BATCH(0x05,可选)

在一个帧内打包多条子命令,减少往返延迟:

```
载荷:[n:u8] ( [op:u8][len:u16][payload] ) × n
响应:逐条 RESP(共用 SEQ);任一步出错即终止并回 ERR
```

典型用途:批量写多路 IO、一次配置多个 PWM、启动+赋值组合。

### 11.7 高速流与吞吐边界

- 二进制模式用 4KB 大帧 + DMA,摊薄 CDC 开销,持续吞吐可逼近 ~900KB/s;
- **ADC 全速 500ksps × 2B ≈ 1MB/s,仍超出 CDC 实际上限**。对策:降采样率、用 8 位右移模式(FCS.SHIFT)或分通道轮询;以 ≤300ksps 单通道(≈600KB/s)为安全线;
- UART 透传 115200bps(≈11.5KB/s)乃至 921600bps(≈92KB/s)均远低于上限,无压力;
- 分片帧(FLAGS.bit0)用于单帧超上限的传输,接收方按 SEQ + 片序重组。

### 11.8 上位机封装示例(Python)

```python
import serial, struct

SOF, CMD, RESP = 0xCB, 0x01, 0x02
INIT = 0xFFFF

def crc16(data):
    c = INIT
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

class Dock:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.seq = 0
        self.s.write(b"BIN ENTER\n")                 # 切二进制模式
        assert self.s.readline().startswith(b"OK BIN")
    def _frame(self, op, payload=b""):
        self.seq = (self.seq + 1) & 0xFFFF
        body = bytes([SOF, CMD, op, 0]) + struct.pack("<HH", len(payload), self.seq) + payload
        return body + struct.pack("<H", crc16(body))
    def _read(self):
        self.s.read(1)                               # SOF(简化:跳过校验)
        t, op, fl, ln, sq = struct.unpack("<BBBBH", self.s.read(6))
        data = self.s.read(ln); self.s.read(2)       # 载荷 + CRC
        return t, op, data
    def adc_read(self, ch):
        self.s.write(self._frame(0x60, bytes([ch])))
        return struct.unpack("<BHH", self._read()[2])[1:]
    def exit_bin(self):
        self.s.write(self._frame(0x0F)); self._read()
```

(以上为概念示例;正式上位机库规划为 `iodock` 包,同时提供文本与二进制两套后端。)
