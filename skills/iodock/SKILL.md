---
name: iodock
description: 控制 IO Dock 板卡(RP2040,USB 虚拟串口)。覆盖全部功能:数字 IO(读/写/翻转/脉冲/边沿事件)、PWM(频率/占空比/极性/渐变/同步/相位/TICK)、UART(配置/收发/透传)、I2C(速率/扫描/读写)、参数持久化(保存/恢复/清除)。当任务涉及操作这块硬件时使用。
---

# IO Dock 板卡控制

IO Dock 是一块通过 USB 虚拟串口(免驱动)控制的 RP2040 扩展板。用本 skill 的 `iodock.py` 命令行工具即可操作全部功能——不需要自己拼串口命令。

## 工具入口

```bash
# 在本仓库根目录下:
python skills/iodock/iodock.py <命令> [参数]
# 若已安装到用户级(见 README),可直接:
iodock <命令> [参数]
```

> 端口自动识别(VID:PID=1209:8888)。识别不到时:`IO_DOCK_PORT=COMxx`。

## 板卡能力(以 `iodock info` 实际返回为准)

- **IO**:`IO1..IO<n>`。定制固件 14 路映射:IO1~6=GPIO20~25,IO7~8=GPIO4/5,IO9~11=GPIO26/27/28,IO12~14=GPIO6/16/19。原版固件仅 6 路(GPIO20~25)。
- **PWM**:`PWM1..PWM<m>`。定制固件 5 路:PWM1=GPIO7,PWM2=GPIO8,PWM3=GPIO9,PWM4=GPIO10,PWM5=GPIO18。原版 4 路。
- **UART**:USART1(GPIO0 TX / GPIO1 RX)。
- **I2C**:IIC0(GPIO12 SDA / GPIO13 SCL)。
- **参数持久化**:`config save/load/clear`。

先用 `iodock info` 看当前固件的 IO/PWM/UART 数量,再按实际能力调用,不要超范围。

---

## 全部命令(详细)

### 系统

| 命令 | 说明 |
| --- | --- |
| `iodock ping` | 检测板卡,返回固件版本 |
| `iodock info` | 板卡信息与能力(IO/PWM/UART/I2C 数量、时钟) |
| `iodock reset` | 软复位(串口短暂断开,需重连) |
| `iodock bootloader` | 进入 UF2 烧写模式(枚举为 U 盘) |
| `iodock led on\|off` | 控制板载状态指示灯 |

### IO

| 命令 | 说明 |
| --- | --- |
| `iodock io config <ch> out` | 配置为输出(写之前通常需要) |
| `iodock io config <ch> in [pull]` | 配置为输入;`pull`: 0=浮空 1=上拉 2=下拉 |
| `iodock io write <ch> high\|low` | 写高/低电平(若未配置输出会自动先配置) |
| `iodock io read <ch>` | 读取电平(HIGH/LOW) |
| `iodock io readall` | 读取全部 IO |
| `iodock io toggle <ch>` | 翻转输出电平 |
| `iodock io pulse <ch> high\|low <ms>` | 输出指定宽度脉冲(1..65535 ms,非阻塞) |
| `iodock io event <ch> off\|rise\|fall\|both` | 边沿中断事件上报(off 关闭/rise 上升/fall 下降/both 双边) |

例:`iodock io write 1 high`、`iodock io config 3 in 1`、`iodock io pulse 2 low 500`。

### PWM

| 命令 | 说明 |
| --- | --- |
| `iodock pwm cfg <ch> <freq_hz> [duty_pct]` | 配置频率+占空比(默认 50%) |
| `iodock pwm freq <ch> <freq_hz>` | 仅改频率(同片通道频率联动) |
| `iodock pwm duty <ch> <duty_pct>` | 在线改占空比 |
| `iodock pwm start <ch>` / `iodock pwm stop <ch>` | 启动/停止 |
| `iodock pwm read <ch>` | 读取频率/占空比/运行状态/极性 |
| `iodock pwm pol <ch> normal\|invert` | 极性(反转=等效 180° 移相) |
| `iodock pwm sweep <ch> <from%> <to%> <step_ms>` | 渐变(呼吸灯效果) |
| `iodock pwm sync <ch1,ch2,...>` | 多路同步启动(需同频,相位锁定) |
| `iodock pwm phase <ch> <ref_ch> <deg>` | 跨片相位设置(0..360°,同片不可) |
| `iodock pwm phadj <ch> <ticks>` | 在线微调相位(分频需>1) |
| `iodock pwm tick <ch> <freq_hz>` | TICK:低频真实方波输出 + 每周期事件回调 |
| `iodock pwm tick off` | 关闭 TICK |

参数:
- 频率约 7.5Hz ~ 62.5MHz。
- **占空比 0~100%,支持小数(0.1% 精度)**,如 50.5。
- PWM2/PWM3 共享片 4 → 同频、相位天然对齐;PWM5 独立片 1。
- `pwm sync` 的通道之间必须同频,否则返回 E_CFG。

例:`iodock pwm cfg 1 1000 50`(PWM1 1kHz 50%)、`iodock pwm cfg 5 12345 33.5`、`iodock pwm sweep 1 0 100 10`。

### UART(USART1)

| 命令 | 说明 |
| --- | --- |
| `iodock uart cfg <baud> [databits] [parity] [stopbits]` | 配置;`parity`: N/O/E;数据位 5..8;停止位 1/2 |
| `iodock uart tx <text>` | 发送;**纯偶数字符串自动按 HEX 发送**(如 `AA55`),其余按文本 |
| `iodock uart rx [n]` | 读取接收缓冲(返回 HEX;n 默认 64) |
| `iodock uart flush` | 清空接收缓冲 |
| `iodock uart stream on\|off` | 接收透传开关(开启后数据实时上报 EVT UART_RX) |
| `iodock uart stat` | 查询收发状态(rx 缓冲/tx/错误计数) |

- 波特率范围:1200 ~ 7800000。
- `uart tx` 如需强制按文本发送纯数字串,可在文本里加个非 hex 字符;如需强制 HEX,传偶数字符串。

例:`iodock uart cfg 115200 8 N 1`、`iodock uart tx "hello"`、`iodock uart tx AABBCC`(按 HEX)。

### I2C(IIC0)

| 命令 | 说明 |
| --- | --- |
| `iodock i2c cfg <khz>` | 速率:100/400/1000 |
| `iodock i2c scan` | 扫描总线,枚举从机地址 |
| `iodock i2c read <addr> [reg] [n]` | 读寄存器 N 字节(默认 n=1) |
| `iodock i2c ronly <addr> [n]` | 无寄存器直读 N 字节 |
| `iodock i2c write <addr> <reg> <hexdata>` | 写寄存器 |
| `iodock i2c wronly <addr> <hexdata>` | 无寄存器直写 |

- 地址/寄存器可用 `0x68` 或十进制;数据为 HEX(如 `6B`)。
- 无应答返回 `E_NACK`,超时返回 `E_TIMEOUT`。

例:`iodock i2c scan`、`iodock i2c read 0x68 0x1D 4`、`iodock i2c write 0x68 0x1D 6B`。

### 参数持久化

| 命令 | 说明 |
| --- | --- |
| `iodock config save` | 把当前参数(PWM/IO/UART/I2C/序列)写入 flash,掉电重启自动恢复 |
| `iodock config load` | 从 flash 恢复并应用 |
| `iodock config clear` | 清除保存的参数(下次上电恢复默认) |

---

## 规则与安全

1. **写 IO 前先确认是输出**:`iodock io config <ch> out`;`io write` 失败会自动尝试配置输出后重试。
2. **PWM 占空比 0~100%**,0.1% 精度(内部换算 16 位)。
3. **PWM2/PWM3 同片同频**;PWM5 独立。`pwm sync` 通道必须同频。
4. **I2C 地址/寄存器**支持 `0x68` 与十进制。
5. **UART 波特率** 1200..7800000;`parity` 用 N/O/E。
6. **`config save` 会写 flash**,掉电自动恢复;`config clear` 恢复默认。
7. 命令报 `ERR` 时,把错误原文反馈(如 `E_CFG` 表示需先配置、`E_NACK` 表示 I2C 无应答)。
8. 若 `iodock info` 显示的外设数量与上面不同(不同固件),按实际能力使用,不要硬套。

## 常见流程

- **让某路 IO 输出高电平**:`iodock io config 1 out` → `iodock io write 1 high`。
- **呼吸灯**:`iodock pwm cfg 1 1000 50` → `iodock pwm start 1` → `iodock pwm sweep 1 0 100 10`。
- **读传感器(I2C)**:`iodock i2c scan` → `iodock i2c read 0x68 0x1D 4`。
- **保存配置**:设置完参数后 `iodock config save`,掉电后自动恢复。

## 完成后

- 如果只是查询,直接返回结果。
- 如果设置了 GPIO/PWM 并希望保留,提醒用户是否 `iodock config save`。
