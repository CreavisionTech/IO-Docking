# IO Dock 固件

当前标准版：**0.1.4**（6 IO / 4 PWM / 2 UART / I2C / SPI / 3 ADC）。
本轮修复和可复现测试记录见 [validation/REPORT.md](validation/REPORT.md)。
开发工作区另有不同引脚布局的定制固件；本仓库此目录为标准版，不要混用定制版固件或持久化参数。

RP2040 端固件,实现 [Host_Protocol.md](../Doc/Host_Protocol.md) 的 PC 控制协议:

- **USB CDC 虚拟串口**(TinyUSB),默认**文本协议**,`BIN ENTER` 切换**二进制协议**
- 全部外设驱动:6×IO、4×PWM(含跨片相位控制)、2×UART(透传)、I2C、SPI、3×ADC + 温度、阈值事件、ADC 连续采样流(FIFO IRQ + 软件环形缓冲)
- **序列引擎运行在核心 1**,主核专司 USB 命令处理,序列执行不阻塞控制通道
- 事件上报:GPIO 变化(带防抖)、UART 数据/溢出、ADC 阈值、序列完成

## 目录结构

```
firmware/
  CMakeLists.txt
  pico_sdk_import.cmake
  src/
    main.c           # USB 主循环、事件分派
    board.h          # 引脚映射(见 Doc/RP2040_IO.md)
    protocol.c/.h    # 错误码、TX 环形缓冲
    drivers.c/.h     # 外设驱动 + 扫描/渐变/脉冲/采样后台任务
    text_cmd.c/.h    # 文本协议解析
    bin_proto.c/.h   # 二进制协议(帧 + 操作码)
    seq.c/.h         # 序列引擎(核心 1)
    tusb_config.h    # TinyUSB 配置(单 CDC)
    usb_descriptors.c
```

## 构建

前置:Pico SDK、ARM GCC(≥10)、CMake、Ninja。已在本机验证:

```bash
# 一次性:克隆 SDK(含 tinyusb 子模块)
git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk

# 配置 + 编译
cmake -S firmware -B firmware/build -G Ninja -DPICO_SDK_PATH=~/pico-sdk
cmake --build firmware/build

# 生成 .bin/.hex/.uf2(去掉了 pico_add_extra_outputs,因本机 host gcc 过旧编不过 picotool)
arm-none-eabi-objcopy -O binary firmware/build/io_dock.elf firmware/build/io_dock.bin
arm-none-eabi-objcopy -O ihex    firmware/build/io_dock.elf firmware/build/io_dock.hex
python firmware/uf2conv.py firmware/build/io_dock.elf -o firmware/build/io_dock.uf2
```

> 本轮构建、烧录与验证产物均在 `firmware/build/`。

## 烧录

- **全自动(推荐)**:`python flash_auto.py [COMxx]` —— 自动识别端口(VID:PID=1209:8888)、发 `BOOTLOADER` 进入烧写模式(板卡遗留二进制模式时自动先发 `MODE_EXIT` 退回)、等 `RPI-RP2` 盘出现、写入 uf2 并刷盘、等待重启。等价手工流程:发 `BOOTLOADER` → 等 `RPI-RP2` 盘出现 → 把 `io_dock.uf2` 拷入(bootrom 消费文件后自动重启)。
- **UF2**:按住板载 BOOT 键上电进入引导,把 `io_dock.uf2` 拖入 U 盘。
- **SWD**:`picotool load -x io_dock.uf2` 或直接用调试器烧 `io_dock.elf`。

> ⚠️ 本板 bootrom v3 **只接受 256 字节负载/块的官方 UF2 格式**(`uf2conv.py` 已按此生成)。**476 字节负载/块的 uf2 会被静默拒绝**——文件看似写进去了,实际没烧进 flash,重启后仍是旧固件。Windows 上写盘后必须确保刷盘(`Flush(true)` 或安全弹出),否则缓存未落盘 bootrom 收不到。

## 使用

插入 USB,PC 出现虚拟串口。打开任意串口终端(115200 8N1):

```
> PING
OK PING PONG 0.1.0
> IO CFG IO1 OUT
OK IO CFG
> IO WRITE IO1 LOW        # IO1 的 LED 低电平点亮
OK IO WRITE
> PWM CFG PWM1 1000 50
OK PWM CFG PWM1 1000Hz 50%
> PWM START PWM1
OK PWM START PWM1
> ADC READ ADC0
OK ADC READ ADC0 2048 1650mV
> SEQ DEF blink "IO IO1 LOW@0; WAIT 200; IO IO1 HIGH@200; WAIT 200"
OK SEQ DEF
> SEQ RUN blink 0         # 无限闪烁(核心 1 执行)
OK SEQ RUN
```

完整命令集与二进制帧格式见 [Doc/Host_Protocol.md](../Doc/Host_Protocol.md)。

## 测试

```bash
python firmware/test_iodock.py [--port COMxx]   # 未指定端口则自动扫描(文本 + 二进制基础项)
python firmware/bin_sweep.py [COMxx]            # 二进制协议专项:覆盖全部 OP 码 + 逐帧 CRC 校验
```

已在本板实测 **22 项全部通过**(文本 + 二进制协议全覆盖)。需要外部条件才能完整验证的项会打印 `[SKIP]`/`[INFO]` 而不判失败:

- UART 透传(需外部激励)

> 注:若把某两个 IO 口短接在一起,测试前请确认它们不会互相驱动(例如 IO1↔IO2 短接时,IO2 需保持输入,否则会与 IO1 输出对拉)。

## 已知限制与兼容性(v0.1.4)

- PWM2 与 PWM3 共享片 4:同频、相位对齐,仅可极性反转取 180°;跨片相位可用 `PWM PHASE`/`PWM SYNC`/`PWM PHADJ`
- `PWM PHADJ` 在线微调要求该片分频器 > 1(数据手册限制),否则返回 `E_DENIED`
- 序列脚本中的 TX 数据暂不支持含空格(用 `HEX:`)
- `SAVE` / `LOAD` / `CFG CLEAR` 已实现，启动自动恢复；Flash 操作在序列或 ADC 采样期间返回 `E_BUSY`。持久化布局升级为版本 2，旧配置不自动加载，需重新配置并 SAVE。
- ADC 支持 1～500 kSPS 的采样请求，但不保证 USB 全速链路能无损传回最高采样率。本机二进制 100 kSPS、200ms 测试完整收到 20,000 样本；250/500 kSPS 会过载并报告 `EVT ADC_OVERRUN`，采样按规定时间截止。
- ADC 采样期间单次读取、读全部、温度读取返回 `E_BUSY`；阈值轮询暂缓。连续采样结束标记发出前，新采样也可能返回 `E_BUSY`。
- SPI LSB 通过软件逐字反转实现；9～16 位 SPI 传输要求偶数字节长度，字按小端字节序编码。
- 文本命令超过 511 字节整行拒绝，不执行截断前缀。非法数字、通道尾缀、额外参数和无效二进制载荷被拒绝。
- 序列驱动错误发 `EVT SEQ_ERROR`，正常完成或主动 STOP 发 `EVT SEQ_DONE`；没有事件触发调度、多序列并行功能。
- BATCH(0x05)二进制批量命令未实现(标记可选)
- 烧录必须使用**官方 UF2 格式**(256 字节/块 + family ID);bootrom v3 不接受自定义格式的 uf2
