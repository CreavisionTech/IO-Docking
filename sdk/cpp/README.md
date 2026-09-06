# IO Docking Board C++ SDK

基于 RP2040 IO 扩展板的 PC 控制 SDK，实现文本模式协议，覆盖全部外设控制。

## 文件结构

```
sdk/cpp/
├── iodock.h        # SDK 头文件（类定义、枚举、接口）
├── iodock.cpp      # SDK 实现（串口通信、协议解析）
├── demo.cpp        # 调用示例（完整演示所有功能）
├── CMakeLists.txt  # CMake 构建脚本
└── README.md       # 本文件
```

## 快速开始

### 编译

**Windows (MSVC):**
```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

**Linux/macOS:**
```bash
mkdir build && cd build
cmake ..
make
```

**MinGW:**
```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
make
```

### 运行示例

```bash
# Windows
./demo COM3

# Linux
./demo /dev/ttyACM0
```

## SDK 使用指南

### 1. 包含头文件

```cpp
#include "iodock.h"
using namespace iodock;
```

### 2. 创建连接

```cpp
IODock dock("COM3");  // Windows
// IODock dock("/dev/ttyACM0");  // Linux

if (!dock.open()) {
    std::cerr << "连接失败" << std::endl;
    return -1;
}

if (!dock.ping()) {
    std::cerr << "板卡无响应" << std::endl;
    return -1;
}
```

### 3. IO 控制

```cpp
// 配置 IO1 为输出
dock.ioConfig(IOChannel::IO1, Direction::OUTPUT);

// 输出低电平（LED 亮）
dock.ioWrite(IOChannel::IO1, Level::LOW);

// 翻转输出
dock.ioToggle(IOChannel::IO1);

// 输出 200ms 脉冲
dock.ioPulse(IOChannel::IO1, Level::LOW, 200);

// 配置 IO2 为上拉输入
dock.ioConfig(IOChannel::IO2, Direction::INPUT, PullMode::PULLUP);

// 读取输入电平
Level level = dock.ioRead(IOChannel::IO2);

// 读取全部 IO
auto levels = dock.ioReadAll();
```

### 4. PWM 控制

```cpp
// 配置 PWM1: 1kHz, 50% 占空比
dock.pwmConfig(PWMChannel::PWM1, 1000, 50);

// 启动 PWM
dock.pwmStart(PWMChannel::PWM1);

// 修改占空比
dock.pwmSetDuty(PWMChannel::PWM1, 75);

// 呼吸灯效果
dock.pwmSweep(PWMChannel::PWM1, 0, 100, 10, 0);  // 无限循环

// 多路同步
dock.pwmConfig(PWMChannel::PWM2, 1000, 50);
dock.pwmSync({PWMChannel::PWM1, PWMChannel::PWM2});

// 相位偏移
dock.pwmSetPhase(PWMChannel::PWM2, PWMChannel::PWM1, 90);  // 滞后90°

// 停止
dock.pwmStop(PWMChannel::PWM1);
```

### 5. UART 控制

```cpp
// 配置 USART1: 9600 8N1
dock.uartConfig(UARTChannel::USART1, 9600);

// 发送文本
dock.uartSendText(UARTChannel::USART1, "Hello");

// 发送十六进制数据
dock.uartSendHex(UARTChannel::USART1, {0x01, 0x02, 0x03});

// 读取接收缓冲
auto data = dock.uartReceive(UARTChannel::USART1);

// 启用透传模式
dock.uartStream(UARTChannel::USART1, true);
```

### 6. I2C 控制

```cpp
// 设置速率
dock.i2cSetSpeed(400);  // 400kHz

// 扫描总线
auto addrs = dock.i2cScan();

// 读取寄存器
auto data = dock.i2cReadReg(0x68, 0x3B, 6);  // MPU6050 加速度

// 写寄存器
dock.i2cWriteReg(0x68, 0x6B, {0x00});  // 唤醒 MPU6050
```

### 7. SPI 控制

```cpp
// 配置 SPI
dock.spiConfig(1000000, 0, "MSB", 8);  // 1MHz, 模式0

// 全双工传输
auto rx = dock.spiTransfer({0xDE, 0xAD, 0xBE, 0xEF});

// 手动控制 CS
dock.spiCS("LOW");
dock.spiWrite({0x01, 0x02});
dock.spiCS("HIGH");
```

### 8. ADC 控制

```cpp
// 单次读取
auto val = dock.adcRead(ADCChannel::ADC0);
std::cout << val.raw << " (" << val.millivolts << " mV)" << std::endl;

// 读取温度
float temp = dock.adcReadTemp();

// 连续采样
dock.adcStartSample(ADCChannel::ADC0, 1000, 1000);  // 1kHz, 1秒

// 阈值事件
dock.adcThreshold(ADCChannel::ADC0, 1000, 2000, true);
```

### 9. 组合时序

```cpp
// 定义序列
dock.seqDefine("blink", "IO IO1 LOW@0; WAIT 200; IO IO1 HIGH@200; WAIT 200");

// 执行序列（循环3次）
dock.seqRun("blink", 3);

// 列出序列
auto seqs = dock.seqList();

// 删除序列
dock.seqDelete("blink");
```

### 10. 事件监听

```cpp
// 注册回调
dock.onEvent([](const std::string& event, const std::string& data) {
    std::cout << "事件: " << event << " - " << data << std::endl;
});

// 启动监听
dock.startEventListener();

// 使能 IO 事件
dock.ioEvent(IOChannel::IO2, true, Edge::FALLING);

// ... 等待事件 ...

// 停止监听
dock.stopEventListener();
```

### 11. 关闭连接

```cpp
dock.close();
```

## 错误处理

SDK 使用 `Response` 结构返回命令结果：

```cpp
Response resp = dock.sendCommand("IO READ IO1");
if (resp.success) {
    std::cout << "成功: " << resp.payload << std::endl;
} else {
    std::cerr << "失败: " << resp.error << " (错误码: " << resp.errorCode << ")" << std::endl;
}
```

## 注意事项

1. **串口权限**：Linux 下可能需要 `sudo` 或添加用户到 `dialout` 组
2. **波特率**：USB CDC 虚拟串口波特率不影响实际通信速率
3. **行缓冲**：单行命令最大 512 字节
4. **线程安全**：SDK 内部使用互斥锁，支持多线程调用
5. **事件监听**：需要单独线程运行，回调在监听线程中执行

## 协议参考

完整协议文档见 `Doc/Host_Protocol.md`

## 接收线程与响应语义（2026-09 修复）

公开方法、参数、默认值和返回结构保持源码兼容；内部对象布局改变，使用者需要重新编译链接。

- `open()` 启动唯一串口接收线程。命令调用串行发送，并等待响应队列；不清空串口缓冲。分片行跨轮询超时保留，短写继续发送。
- `EVT`、`DATA`、`DATA_END` 始终进入异步队列，不参与命令匹配。`onEvent` 的回调分别收到事件名、`DATA`、`DATA_END`；后两个的数据参数含通道及剩余内容。
- `startEventListener()` 仅启动回调分发。回调在独立线程执行，可调用同步命令、替换回调或停止监听；回调异常被隔离。停止监听不停止接收，未分发的数据会保留到再次启动。长时间数据流应保持回调消费，未消费队列会增长。
- `INFO`、`HELP`、`IO READALL`、`ADC READALL`、`I2C SCAN` 一直收集至 `END`。`Response.command` 保留调用者原始命令；`payload` 去掉完整命令前缀和 `END`，多行内容以换行分隔。
- 成功时 `errorCode=0`；固件已知错误仍用原有映射；未知错误、传输错误和超时为 `-1`。多级命令错误能正确提取 `E_*`。
- 命令总等待期限为 1 秒。超时、传输失败或错配会禁止后续命令，需 `close()` 后重新 `open()`。文本协议没有请求 ID，不能安全地把迟到响应当成下一条命令的结果；重新连接应在设备旧命令已结束后进行。
- ADC、IO、PWM、温度及 HEX 的畸形成功响应会抛出解析异常，避免返回部分解析或伪造读数。固件 ERR 时各高层方法的原有默认返回行为仍保留；需要详细失败原因时使用 `sendCommand()`。
- `pwmConfig(..., false)` 使用 `#raw` 明确指定原始占空比（需要支持此语法的固件）；I2C 写数据使用连续 HEX；修复 PWM SYNC 首个通道前缺空格。

连接生命周期（`open/close`）由调用者串行管理；可从多个工作线程调用命令，`close()` 可取消等待中的命令。不要在自身回调内销毁 IODock 对象。连续流无需新增读取 API，使用现有 `onEvent()` 消费。

## 离线回归测试

```bash
cmake -S . -B build-offline -DCMAKE_BUILD_TYPE=Debug
cmake --build build-offline
ctest --test-dir build-offline --output-on-failure
```

测试通过编译期替换底层 open/read/write，使用内存模拟串口；生产组行、接收线程、命令匹配和高层 API 都参与测试。不会枚举或打开真实串口。生产库不启用这个替换；`demo` 仅构建，测试不会运行它。仅构建 SDK 时可传 `-DBUILD_TESTING=OFF`。

详细验证见 [VALIDATION.md](VALIDATION.md)。
