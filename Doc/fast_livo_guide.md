# Fast-LIVO / 雷达·相机时间同步模式 · 操作指南

> 本文档介绍 IO Dock 的 **Fast-LIVO 预设**：由**板卡自主**产生
> 「1 Hz 秒脉冲(PPS) + 多路 10 Hz 触发 + 每秒 GPRMC 时间报文」，供 Fast-LIVO / FAST-LIO、
> 激光雷达、相机或需要多传感器同步的应用使用。


## 1. 输出信号与默认分配

| 信号 | 默认通道 | 引脚 | 参数 |
| --- | --- | --- | --- |
| 1 Hz 秒脉冲 PPS | PWM1 | GPIO7 | 固定 1 Hz，脉宽可配(默认 10000 µs)，极性可反 |
| 10 Hz 触发 | PWM2 / PWM3 / PWM4 | GPIO8 / GPIO9 / GPIO10 | 频率 1–100 Hz 可配(默认 10)，脉宽可配(默认 1000 µs) |
| GPRMC 时间报文 | USART1 TX | GPIO0 | 每秒一条，含 UTC 日期、跨天、XOR 校验和与 CRLF |

要点：

- **PPS 通道永远是 1 Hz**（秒脉冲，没有频率参数）；想要某一路输出 10 Hz，应把它勾成“触发输出
  通道”，并把 PPS 固定到另一路。**PPS 与触发不能是同一路。**
- PWM2/PWM3 共享片计数器，**天然同频同相**；四路输出全部由板卡定时器驱动，与浏览器/上位机断开
  后仍持续运行，重启上电仍可用（见第 5 节“开机自动运行”）。
- 时钟语义：离线上电为 `BUILD`（固件构建 UTC 时刻）；`SYNC <us> UTC` 授时后为 `HOST`；
  旧版单调同步为 `HOST_MONO`。板卡重启后回到 `BUILD`，需要重新授时。

## 2. 接线

| IO Dock 端 | 接到 |
| --- | --- |
| PWM1 / GPIO7（1 Hz PPS） | 雷达 / 相机 / 采集卡的 PPS 或外触发输入 |
| PWM2 · PWM3 · PWM4（GPIO8/9/10，10 Hz 触发） | 各触发通道(相机快门、LiDAR 触发等) |
| USART1 TX / GPIO0（GPRMC） | 接收方串口 RX(如 GNSS 口 / 驱动解析口)，波特率需与对方一致(默认 115200 8N1) |
| USB-C | 电脑(USB 虚拟串口，控制与授时) |

> 若只需给一个设备 10 Hz 触发而不需要 1 Hz 脉冲：把 10 Hz 放你要的那路触发通道，PPS(1 Hz)
> 输出到空闲脚(如 PWM2)即可，不必接线。GPRMC 与秒脉冲的相对延迟可调，用于对齐报文与秒边沿。

## 3. 网页操作（推荐顺序：连接 → 校时 → 配置启动 → 可选开机运行）

打开同步任务页 `web/timing.html`（仓库内可用 `python -m http.server` 后访问
`http://localhost:8000/web/timing.html`；在线版为
`https://creavisiontech.github.io/IO-Docking/timing.html`）。**请用 Chrome/Edge**，不要用
`file://` 直接打开（Web Serial 可能不工作）。

1. **连接**：点击「连接板卡」，选择 IO Dock 串口。连接成功会自动查询状态。
2. **UTC 校时**：点「使用电脑 UTC 校时一次」（发送 `SYNC <现在时刻> UTC`，来源变为 `HOST`）。
   也可点「读取板卡时间」确认 `TIME` 的 `source=HOST`。
3. **配置（保持 Fast-LIVO 默认即可）**：PPS 通道 = PWM1；触发频率 = 10；触发输出通道勾选
   PWM2/PWM3/PWM4；脉宽与极性按需调整；NMEA 串口 = USART1、波特率 115200。
4. **应用并启动**：页面会自动先 `TIMING STOP` 再应用配置并 `TIMING START`，输出边沿对齐下一个
   真实 UTC 整秒。
5. **检查运行**：观察“运行观测”里 `active=1`、`seconds/triggers` 持续递增、`nmea_sent` 增加、
   `max_late_us` 处于低值(板卡内统计)。
6. **（可选）开机自动运行**：点「停止 → 保存开机配置 → 重新启动」；下次上电自动恢复该配置并
   启动同步。要取消则点「关闭开机运行并保存」。

## 4. 命令行 / 脚本方式

串口(115200 8N1)直接发送，或按 Python SDK / pyserial 封装调用同一命令：

```
PING
SYNC <当前Unix微秒> UTC          # 授时;也可不带 UTC 做旧版单调同步
TIMING CFG 1 14 10 10000 1000 0  # PPS=PWM1, 触发 PWM2|3|4=bit1|2|3, 10Hz,
                                 # PPS脉宽10000us, 触发脉宽1000us, 极性正常
TIMING NMEA 1 115200 100000      # USART1 输出 GPRMC, 秒边沿后延迟 100000us
TIMING START                     # 对齐下一个 UTC 整秒后开始
TIMING STAT                      # 查询运行与计数
TIME                             # 查询时间来源/距今
```

- 停止：`TIMING STOP`；**改参数前必须先停止**（运行中 `TIMING CFG` 返回 `E_BUSY`）。
- 需要让边沿重新对齐真实 UTC 整秒：`TIMING STOP` →（重新 `SYNC … UTC`）→ `TIMING START`，
  并把这次重启视为采集会话边界。授时跳变 ≥1 s 时板卡会回 `EVT TIME_JUMP`。
- 保存/开机：`SEQ DEF fastlivo "TIMING START"` → `BOOT SEQ fastlivo` → `SAVE`；
  取消：`BOOT OFF` → `SAVE`；恢复出厂：`TIMING STOP`、`BOOT OFF`、`CFG CLEAR`。

## 5. 校验与常见问题

- **10 Hz 触发间隔不对 / 改参数后仍是旧频率**：先看是否该路被设成了 PPS 通道（PPS 永远 1 Hz）；
  再看是否改配置时任务仍在运行（先 `TIMING STOP` 再改，`TIMING CFG` 运行中会 `E_BUSY`）。
- **`TIMING START` 报 E_BUSY**：目标通道已被其他活动占用——运行中的普通 PWM、`PWM TICK`、
  渐变或序列正在使用 PWM1–4/USART1，先停止它们再启动。
- **同一路被勾成 PPS 又勾成触发**：页面会自动拦截并提示；命令行会返回 `E_PARAM`。
- **断连/关闭页面后输出停止？** 不会。输出由板卡自主调度，USB 只用于控制与授时；
  板卡 `RESET`/断电后才停止（并回到 `BUILD` 时间，需重新授时）。
- **GPRMC 收不到**：确认 USART1 TX(GPIO0) 接到对方 RX、双方波特率一致；在网页把 NMEA 串口
  关/开或改用逻辑分析仪查看 GPIO0。
- **需要微秒级边沿精度时**：以逻辑分析仪/示波器实测为准（板卡 `TIMING STAT` 的 `max_late_us`
  只是板内软件统计）。授时与触发的“对齐整秒”受 USB/PC 时钟误差影响，推荐用 GPS/网络授时源。
- **重启后时间回到 BUILD**：属设计行为，板卡不知道断电时长；Fast-LIVO 上位机流程仍为
  「连接 → 授时 → 启动采集」。


