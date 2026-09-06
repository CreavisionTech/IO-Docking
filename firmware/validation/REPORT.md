# 标准版 0.1.4 修复与验证报告

日期：2026-09-06。目标：标准版 6 IO、4 PWM、2 UART、I2C、SPI、3 ADC。
板卡：COM43，UID `d665bc31023b1c16`，RP2040，clk_sys=125 MHz。
连接条件：用户确认仅 USB，无外设、回环线或示波器。
原运行固件为 14 IO / 5 PWM / 1 UART 的 0.3.0；本次按用户要求改烧标准版。

## 交付

- `firmware/io_dock-v0.1.4.uf2`：在 COM43 完成实机验证的标准固件；重新构建输出位于 `firmware/build/`。
- 仓库内的标准源码与实测工作区逐文件一致，并已在仓库路径下重新编译通过。不同构建目录/缓存会影响产物字节，发布文件保留原实测 SHA-256。
- UF2：187,392 字节，256 字节有效载荷/块，RP2040 family ID。
- SHA-256：`ca07e92468a2ade0a0463dfb638d32e429cab2dd2757c89642b3e3a5bb8438cd`。
- ELF、BIN、HEX 在 `firmware/build/`；本目录包含测试源码、JSON 结果和日志。
- 本报告为原开发工作区的验证记录；工作快照及 Web 离线脚本保留在原工作区，未随本次固件/SDK 发布打包。
- 原验证过程未操作在线仓库。本次打包范围为标准固件、SDK、相关文档和测试；报告中的 Web 验证属于原工作区记录。

## 修复内容

| 区域 | 修复与行为 |
| --- | --- |
| ADC 分频 | 用 SDK 的 16.8 定点分频编码，修正设置 1 kHz 却高速采样的问题；校验范围、时长和样本计数溢出 |
| ADC 流 | 精确限制样本数；FIFO IRQ 与软件环形缓冲；分包合并；高优先级定时器终止过载采样；最后一批数据与 DATA_END 顺序正确 |
| ADC 互斥 | 连续采样期间 READ/READALL/TEMP 返回 E_BUSY，暂停阈值轮询；新采样等待前一流的结束通知 |
| 丢样上报 | 软件溢出计数，硬件 FIFO 溢出标记，ADC_OVERRUN 明确报告损失下界；高负载不再无提示延长采样 |
| Flash | SDK flash_safe_execute 协调中断与核心 1；核心 1 安全初始化握手；运行序列/采样期间拒绝 SAVE/LOAD/CLEAR；检查失败返回值 |
| 配置版本 | 标准版布局升级为版本 2，不加载旧定制布局；重新配置 SAVE 后可自动恢复 |
| 序列 | repeat 保留 uint16；末尾/纯 WAIT 有效；启动、停止、完成交接同步；失败定义不覆盖旧定义；严格参数、时序、溢出校验 |
| 序列与核心 1 | 用共享状态和 SEV/WFE 启动，避免与 Flash lockout 抢 FIFO；SAMPLE 交给核心 0；执行失败中止并发 SEQ_ERROR；停止脉冲时恢复电平 |
| GPIO | 真正关闭/切换边沿中断；原子取走事件位；重配、写入和翻转取消旧脉冲恢复；驱动层校验参数 |
| PWM | 共享片频率与状态联动；停止后修改不意外复活；SYNC/PHASE 先停后同步启动；PHADJ 掩码修正及超时；100% 占空比；无限 sweep；低频 tick 的偶数分频 |
| SPI | PL022 原生只支持 MSB，LSB 改为软件按有效字宽反转；9～16 位传输拒绝奇数字节；合法波特率和位序检查；CS AUTO 恢复高电平 |
| UART | 把硬件接收溢出纳入计数；保留环形缓冲溢出报告 |
| 文本协议 | 整行超长拒绝；严格通道、数字、枚举、额外参数及长度检查；不会把 IO10 当 IO1；错误返回一致 |
| 二进制协议 | 固定/变长载荷长度检查；CRC、FLAGS、OP 校验；整帧写入发送队列；半帧超过 1 秒后可恢复接收；版本统一 0.1.4 |
| USB | 整块入队与有界背压；批量复制及 CRC 优化；主循环限制 RX/TX 工作量；发送过载报告 TX_OVERRUN |
| Python SDK | 唯一接收线程、串行事务、独立回调；多行响应；正确识别双词错误；EVT/DATA/DATA_END 分流；修正命令格式与结果解析 |
| C++ SDK | 同样修复事务/事件抢读、多行响应和解析；短写、分片、关闭取消、回调重入；显式设置 DTR/RTS 与流控，修复重启后首次连接失败；公开签名兼容，使用者需要重新编译 |
| 烧录脚本 | 重启板卡前校验全部 UF2 块、family ID、地址及尾标记；仅识别 INFO_UF2.TXT 中的 RPI-RP2；写入刷盘 |

## 测试结果

| 测试 | 结果 | 证据 |
| --- | --- | --- |
| ARM Release 全量编译 | 成功，开启 Wall/Wextra/Wformat=2，零编译警告 | `build.log` |
| 原有固件测试 | 20 通过、0 失败、1 跳过 | `legacy-text-binary.log` |
| 原有二进制专项 | 21 通过、0 失败 | `binary-sweep.log` |
| 新增 USB-only 回归 | 17/17 组通过 | `hardware_results.json` |
| 二进制压力/吞吐 | 10/10 组通过；含 500 条流水请求、CRC、载荷边界、半帧恢复、最大 SPI 请求和高负载恢复 | `binary_stress_results.json` |
| Python SDK 离线 | 18/18 场景通过 | `sdk/python/test_iodock.py` |
| Python SDK 实机 | 4 线程共 200 PING，与 10 kSPS/500ms 数据和 PWM 事件并行；完整收到 5,000 样本 | `sdk_hardware_results.json` |
| C++ SDK 离线 | MinGW GCC 13.1 构建；25/25 模拟场景通过 | `sdk/cpp/VALIDATION.md` |
| C++ SDK 实机 | 板卡重启后首次直连；4 线程共 200 PING、5,000 ADC 样本、PWM 事件、多行响应/错误解析通过 | `sdk_cpp_hardware.cpp`、`cpp-hardware.log` |
| 序列离线 | 320 项断言通过 | `firmware/test_seq_regression.py` |
| PWM 离线 | 寄存器/共享片/分频/sweep 回归通过 | `firmware/test_pwm_offline.py` |
| 协议离线 | 42 个非法文本案例及有效/错误返回检查；二进制固定和变长边界、CRC/FLAGS、超时恢复等通过 | `firmware/tests/test_protocol_parsers.py` |

实机确认：400ms 序列总时长约 0.4 秒；256 次 1ms 序列约 0.256 秒；1Hz tick 事件间隔约 1 秒。
Flash 测试执行了 3 轮 SAVE/修改/LOAD，并软复位验证 IO 和序列恢复，再 CLEAR/LOAD 验证 E_NOTFOUND。
GPIO 事件测试通过同一引脚输出变化触发自身边沿检测，验证 OFF 后不再报告；不等同外部输入电气测试。

## ADC 吞吐边界

最终压力测试使用二进制帧，并逐帧检查 CRC、长度、通道数据范围和 DATA_END 数量。

| 采样请求 | 请求样本 | 收到样本 | 结果 |
| --- | ---: | ---: | --- |
| 1 kSPS，200ms | 200 | 200 | 无丢样 |
| 10 kSPS，200ms | 2,000 | 2,000 | 无丢样 |
| 50 kSPS，200ms | 10,000 | 10,000 | 无丢样 |
| 100 kSPS，200ms | 20,000 | 20,000 | 无丢样 |
| 100 kSPS，5s | 500,000 | 500,000 | 无丢样，约 5.0006s 完成 |
| 250 kSPS，200ms | 50,000 | 44,671 | 过载，报告至少 5,329 丢样，约 0.234s 收完缓冲 |
| 500 kSPS，200ms | 100,000 | 47,131 | 过载，报告至少 52,869 丢样，约 0.233s 收完缓冲 |

这是本机、该板卡、该 USB 连接下的结果；不应把最高可配置采样率宣称为无损回传率。
250/500 kSPS 项通过的是“有界结束、CRC/长度正确、明确丢样报告、随后 PING 正常”的过载处理断言，不是无损采集断言。

## 可复现命令

在项目根目录执行；硬件脚本会改变输出/序列/配置，仅适用于 USB-only 板卡。

```powershell
cmake --build firmware/build
python firmware/uf2conv.py firmware/build/io_dock.elf -o firmware/build/io_dock.uf2
python firmware/flash_auto.py COM43
python firmware/test_iodock.py --port COM43
python firmware/bin_sweep.py COM43
python firmware/validation/regression_hardware.py COM43
python firmware/validation/stress_binary.py COM43
python firmware/validation/sdk_hardware.py COM43
python firmware/test_seq_regression.py
python firmware/test_pwm_offline.py
python firmware/tests/test_protocol_parsers.py
python -m unittest discover -s sdk/python -p test_iodock.py -v
```

C++ 的 MinGW 构建命令见 SDK 内 VALIDATION.md；实机测试编译 `sdk_cpp_hardware.cpp` 与 `iodock.cpp`，添加 SDK include 目录、C++17、pthread、setupapi，运行生成程序并传 COM43。

## 尚未验证的边界

- 没有 UART/SPI 实物回环、I2C 从机、外部 GPIO 激励、电压参考或示波器；不把空总线响应与配置成功当作电气收发/波形验证。
- PWM 的绝对频率、跨片相位、100% 电平、SPI LSB 实际线序需要仪器或外部回环进一步验证；本轮包含驱动寄存器模拟与命令实机验证。
- 没做真实断电/掉电中途擦写、长期老化、模拟输入精度/校准、跨平台串口测试。
- Web 经过生产 JS 的离线模拟，没有通过浏览器真实 Web Serial 连接操作。
- C++ MSVC 的配置受本机 Windows SDK 库路径影响未完成；MinGW 构建及 Windows 实机测试通过。Linux/macOS 分支未实测。
- 外设总线事务仍可能同步等待完成；未知外设异常/长 UART 发送下的控制延迟不在 USB-only 验证范围内。
- 没有证明“所有潜在 bug 都不存在”；本次修复审查确认的问题，并保留上述测试作为后续回归基线。

测试结束清除测试用持久化配置并重启，IO 默认输入、PWM 默认停止、采样/序列停止、文本模式。
