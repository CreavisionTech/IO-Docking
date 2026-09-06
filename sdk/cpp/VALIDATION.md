# C++ SDK 离线审查验证报告

日期：2026-09-06。修改范围仅 `IO-Docking/sdk/cpp`；未连接硬件、未运行 demo、未修改 Python 或 firmware。

## 已修复

1. 唯一接收线程，命令响应与 EVT/DATA/DATA_END 分流；监听线程不再读取串口。
2. 删除连接和发命令前的清缓冲操作；未完成的行跨 readLine 超时保存，短写继续发送。
3. INFO、HELP、IO READALL、ADC READALL、I2C SCAN 收齐 END；允许异步数据穿插。
4. 完整多级命令匹配，原始 command 保留，空 ACK 与未知响应区分；错误码始终初始化并正确识别多级 ERR。
5. INFO 同行多键值、ADC 通道/数值、IO 通道、PWM 频率/占空比/运行状态/极性、负温度和 HEX 解析。
6. 回调独立执行、支持同步命令重入、停止及重启监听、隔离回调异常；关闭连接唤醒等待者。
7. 超时/失步后拒绝继续发命令，防止迟到响应复用；拒绝 CR/LF 命令注入。
8. PWM SYNC 空格、显式原始占空比编码、I2C 多字节连续 HEX 编码。
9. 主任务实机复验发现并修复：Windows 显式使能 DTR/RTS、禁用继承的软硬件流控，避免板卡重启后首次 C++ 连接无响应。此前 Python 留下的 DTR 状态会掩盖问题。

## 验证结果

- Windows x64，CMake 4.4 / Ninja / MinGW GCC 13.1.0，C++17 Debug。
- `iodock` 静态库、`demo`、`iodock_offline_tests` 构建成功。
- CTest 1/1 测试程序通过，内部 **25/25 场景通过**，最终耗时 4.69 秒。
- 测试覆盖：130ms/70ms 分片及 CRLF、3 字节短写、EVT/DATA/明确 DATA_END 穿插、回调同步 PING、多行及 END、乱序 ADC 通道、空 I2C 扫描、多级 ERR、未知错误码、空 ACK、PWM/温度/HEX/畸形值、监听前与停止后事件保留、4 线程共 40 条命令、超时及迟到响应隔离、缺 END、非响应行、回调异常/自停/重启、关闭取消等待、读取失败和换行注入。
- `git diff --check -- sdk/cpp` 通过（仅 Git 换行格式提示，无 whitespace 错误）。

复现（项目根目录 PowerShell）：

```powershell
$env:PATH = 'D:\Program Files\JetBrains\CLion 2024.1.2\bin\mingw\bin;' + $env:PATH
cmake -S IO-Docking/sdk/cpp -B IO-Docking/sdk/cpp/build-offline -G Ninja '-DCMAKE_CXX_COMPILER=D:/Program Files/JetBrains/CLion 2024.1.2/bin/mingw/bin/g++.exe' -DCMAKE_BUILD_TYPE=Debug
cmake --build IO-Docking/sdk/cpp/build-offline
ctest --test-dir IO-Docking/sdk/cpp/build-offline -V
```

## 验证边界与兼容性

主任务补充实机验证：板卡软复位后直接使用 C++ SDK（不先由 Python 打开重启后的串口），4 线程共 200 PING，与 5,000 个 ADC 样本和 PWM 事件并行，完整通过；多行和错误解析通过。修复 DTR 后重新构建与 CTest 25 场景全部通过。详见项目根目录 `firmware/validation/cpp-hardware.log`。

- 上述离线测试模拟底层传输，复用生产组行/线程/解析代码；Windows 实机结果见补充记录。Linux/macOS 分支本轮未构建。
- 额外 MSVC 19.29 配置尝试被本机 Windows SDK 路径问题阻断：最初缺 rc/mt 搜索路径，补入工具路径后仍报 `LNK1104 kernel32.lib`。发生在 CMake 最小编译器探测阶段，不计为 SDK 构建通过。
- 公开 API 签名、默认参数和返回结构未改变；私有布局有变化，需重新编译使用者，不保证二进制 ABI。
- 畸形成功响应会抛解析异常；固件 ERR 的高层默认返回语义保留。需要完整错误信息时调用 sendCommand。
- 命令期限维持 1 秒；超时/错配后需关闭重连。无请求 ID 的文本协议不能在旧命令仍执行时保证重连后的响应归属。
- 回调停止期间异步队列继续保留数据，长期高吞吐流需持续消费；连接生命周期应串行管理，不可从自身回调销毁对象。
- 显式原始 PWM duty 的 `#raw` 依赖固件支持；本轮仅核对当前源码并模拟测试。
