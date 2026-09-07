/**
 * @file iodock.h
 * @brief IO Docking Board C++ SDK 头文件
 * @version 1.0.0
 * 
 * 基于 RP2040 IO 扩展板的 PC 控制协议
 * 支持文本模式命令，覆盖全部外设：IO/PWM/UART/I2C/SPI/ADC
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>
#include <cstdint>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>

namespace iodock {

// 前向声明
class SerialPort;

/**
 * @brief 通道枚举定义
 */
enum class IOChannel : uint8_t { IO1=0, IO2, IO3, IO4, IO5, IO6 };
enum class PWMChannel : uint8_t { PWM1=0, PWM2, PWM3, PWM4 };
enum class UARTChannel : uint8_t { USART1=0, USART2 };
enum class ADCChannel : uint8_t { ADC0=0, ADC1, ADC2 };

/**
 * @brief 电平/方向/极性等枚举
 */
enum class Level : uint8_t { LOW=0, HIGH=1 };
enum class Direction : uint8_t { INPUT=0, OUTPUT=1 };
enum class PullMode : uint8_t { FLOAT=0, PULLUP=1, PULLDOWN=2 };
enum class Edge : uint8_t { RISING=0, FALLING=1, CHANGE=2 };
enum class Polarity : uint8_t { NORMAL=0, INVERT=1 };

/**
 * @brief 命令响应结构
 */
struct Response {
    bool success;           // 是否成功
    std::string command;    // 原始命令
    std::string payload;    // 响应内容
    std::string error;      // 错误信息（失败时）
    int errorCode;          // 错误码（失败时）
};

/**
 * @brief ADC 读数结构
 */
struct ADCReading {
    uint16_t raw;           // 原始 12 位值
    uint16_t millivolts;    // 电压（mV）
};

/**
 * @brief PWM 配置结构
 */
struct PWMConfig {
    uint32_t frequency;     // 频率（Hz）
    uint16_t duty;          // 占空比（0-65535）
    bool running;           // 是否运行中
    bool inverted;          // 是否反转极性
};

/**
 * @brief 事件回调类型
 */
using EventCallback = std::function<void(const std::string& event, const std::string& data)>;

/**
 * @brief IO Docking Board 主控制类
 */
class IODock {
public:
    /**
     * @brief 构造函数
     * @param port 串口名称（Windows: COM3, Linux: /dev/ttyACM0）
     * @param baud 波特率（默认 115200，CDC 下不影响实际速率）
     */
    IODock(const std::string& port, uint32_t baud = 115200);
    ~IODock();

    // ========== 连接管理 ==========
    
    /**
     * @brief 打开串口连接
     * @return 是否成功
     */
    bool open();
    
    /**
     * @brief 关闭连接
     */
    void close();
    
    /**
     * @brief 检查连接状态
     */
    bool isOpen() const;
    
    /**
     * @brief 测试连接（发送 PING）
     * @return 是否在线
     */
    bool ping();

    // ========== 系统命令 ==========
    
    /**
     * @brief 获取板卡信息
     * @return 信息键值对
     */
    std::map<std::string, std::string> getInfo();
    
    /**
     * @brief 软复位板卡
     */
    void reset();
    
    /**
     * @brief 进入 UF2 下载模式
     */
    void enterBootloader();
    
    /**
     * @brief 控制板载 LED
     * @param on true=亮, false=灭
     */
    void setLED(bool on);
    
    /**
     * @brief 时间同步
     * @param hostUs 主机时间戳（微秒）
     * @return 偏移量
     */
    int64_t syncTime(uint64_t hostUs);

    // New timing APIs return full responses; status payloads are kept verbatim.
    Response syncUtc(); // System Unix microseconds sampled after acquiring send locks.
    Response syncUtc(uint64_t unixUs);
    Response timeStatus();
    Response timingConfigure(int pps, int triggerMask, int hz,
                             uint32_t ppsWidthUs, uint32_t triggerWidthUs, bool invert = false);
    Response timingNmea(int uart = 0, uint32_t baud = 115200, uint32_t delayUs = 0);
    Response timingStart();
    Response timingStop();
    Response timingStatus();
    Response bootSequence(const std::string& name); // Existing sequence; SAVE separately.
    Response bootSequenceOff();
    Response bootSequenceStatus();


    // ========== IO 控制（6路）==========
    
    /**
     * @brief 配置 IO 方向和上拉
     * @param ch 通道
     * @param dir 方向
     * @param pull 上拉模式
     */
    void ioConfig(IOChannel ch, Direction dir, PullMode pull = PullMode::FLOAT);
    
    /**
     * @brief 设置 IO 输出电平
     * @param ch 通道
     * @param level 电平
     */
    void ioWrite(IOChannel ch, Level level);
    
    /**
     * @brief 读取 IO 输入电平
     * @param ch 通道
     * @return 电平状态
     */
    Level ioRead(IOChannel ch);
    
    /**
     * @brief 读取全部 IO 状态
     * @return 6 个通道的电平数组
     */
    std::vector<Level> ioReadAll();
    
    /**
     * @brief 翻转 IO 输出
     * @param ch 通道
     */
    void ioToggle(IOChannel ch);
    
    /**
     * @brief 输出脉冲
     * @param ch 通道
     * @param level 脉冲电平
     * @param ms 持续时间（毫秒）
     */
    void ioPulse(IOChannel ch, Level level, uint16_t ms);
    
    /**
     * @brief 使能 IO 事件上报
     * @param ch 通道
     * @param enable 是否使能
     * @param edge 触发边沿
     */
    void ioEvent(IOChannel ch, bool enable, Edge edge = Edge::CHANGE);

    // ========== PWM 控制（4路）==========
    
    /**
     * @brief 配置 PWM
     * @param ch 通道
     * @param freqHz 频率（Hz）
     * @param duty 占空比（0-100 百分比 或 0-65535 原始值）
     * @param isPercent duty 是否为百分比
     */
    void pwmConfig(PWMChannel ch, uint32_t freqHz, uint16_t duty, bool isPercent = true);
    
    /**
     * @brief 设置 PWM 频率
     * @param ch 通道
     * @param freqHz 频率（Hz）
     */
    void pwmSetFreq(PWMChannel ch, uint32_t freqHz);
    
    /**
     * @brief 设置 PWM 占空比
     * @param ch 通道
     * @param duty 占空比（0-100）
     */
    void pwmSetDuty(PWMChannel ch, uint16_t duty);
    
    /**
     * @brief 启动 PWM 输出
     * @param ch 通道
     */
    void pwmStart(PWMChannel ch);
    
    /**
     * @brief 停止 PWM 输出
     * @param ch 通道
     */
    void pwmStop(PWMChannel ch);
    
    /**
     * @brief PWM 渐变（呼吸灯效果）
     * @param ch 通道
     * @param from 起始占空比
     * @param to 目标占空比
     * @param stepMs 步进间隔（毫秒）
     * @param repeat 重复次数（0=无限）
     */
    void pwmSweep(PWMChannel ch, uint16_t from, uint16_t to, uint16_t stepMs, uint16_t repeat = 1);
    
    /**
     * @brief 读取 PWM 配置
     * @param ch 通道
     * @return 配置信息
     */
    PWMConfig pwmRead(PWMChannel ch);
    
    /**
     * @brief 同步启动多路 PWM（相位锁定）
     * @param channels 通道列表
     */
    void pwmSync(const std::vector<PWMChannel>& channels);
    
    /**
     * @brief 设置 PWM 相位偏移
     * @param ch 目标通道
     * @param ref 参考通道
     * @param degrees 相位度数（0-360）
     */
    void pwmSetPhase(PWMChannel ch, PWMChannel ref, uint16_t degrees);
    
    /**
     * @brief 设置 PWM 极性
     * @param ch 通道
     * @param pol 极性
     */
    void pwmSetPolarity(PWMChannel ch, Polarity pol);
    
    /**
     * @brief 启用 PWM 定时回调（1Hz 等低频输出）
     * @param ch 通道
     * @param freqHz 频率（低于 7.45Hz 时软件模拟输出）
     */
    void pwmTick(PWMChannel ch, uint32_t freqHz);
    
    /**
     * @brief 关闭 PWM 定时回调
     * @param ch 通道
     */
    void pwmTickOff(PWMChannel ch);

    // ========== UART 控制（2路）==========
    
    /**
     * @brief 配置 UART
     * @param ch 通道
     * @param baud 波特率
     * @param dataBits 数据位（5-8）
     * @param parity 校验（N/O/E）
     * @param stopBits 停止位（1/2）
     */
    void uartConfig(UARTChannel ch, uint32_t baud, uint8_t dataBits = 8, 
                    char parity = 'N', uint8_t stopBits = 1);
    
    /**
     * @brief UART 发送文本
     * @param ch 通道
     * @param text 文本内容
     */
    void uartSendText(UARTChannel ch, const std::string& text);
    
    /**
     * @brief UART 发送十六进制数据
     * @param ch 通道
     * @param data 数据字节
     */
    void uartSendHex(UARTChannel ch, const std::vector<uint8_t>& data);
    
    /**
     * @brief 读取 UART 接收缓冲
     * @param ch 通道
     * @param maxBytes 最大读取字节数（0=全部）
     * @return 接收到的数据
     */
    std::vector<uint8_t> uartReceive(UARTChannel ch, uint16_t maxBytes = 0);
    
    /**
     * @brief 清空 UART 接收缓冲
     * @param ch 通道
     */
    void uartFlush(UARTChannel ch);
    
    /**
     * @brief 启用/关闭 UART 透传模式
     * @param ch 通道
     * @param enable 是否启用
     */
    void uartStream(UARTChannel ch, bool enable);

    // ========== I2C 控制（1路）==========
    
    /**
     * @brief 设置 I2C 速率
     * @param khz 速率（100/400/1000）
     */
    void i2cSetSpeed(uint32_t khz);
    
    /**
     * @brief 扫描 I2C 总线
     * @return 应答的从机地址列表
     */
    std::vector<uint8_t> i2cScan();
    
    /**
     * @brief I2C 写寄存器
     * @param addr 从机地址（7位）
     * @param reg 寄存器地址
     * @param data 数据字节
     */
    void i2cWriteReg(uint8_t addr, uint8_t reg, const std::vector<uint8_t>& data);
    
    /**
     * @brief I2C 读寄存器
     * @param addr 从机地址
     * @param reg 寄存器地址
     * @param count 读取字节数
     * @return 数据
     */
    std::vector<uint8_t> i2cReadReg(uint8_t addr, uint8_t reg, uint16_t count);
    
    /**
     * @brief I2C 直写（无寄存器地址）
     * @param addr 从机地址
     * @param data 数据
     */
    void i2cWrite(uint8_t addr, const std::vector<uint8_t>& data);
    
    /**
     * @brief I2C 直读（无寄存器地址）
     * @param addr 从机地址
     * @param count 字节数
     * @return 数据
     */
    std::vector<uint8_t> i2cRead(uint8_t addr, uint16_t count);

    // ========== SPI 控制（1路）==========
    
    /**
     * @brief 配置 SPI
     * @param baud 速率（Hz）
     * @param mode 模式（0-3）
     * @param bitOrder 位序（MSB/LSB）
     * @param bits 数据位宽（4-16）
     */
    void spiConfig(uint32_t baud, uint8_t mode = 0, const std::string& bitOrder = "MSB", uint8_t bits = 8);
    
    /**
     * @brief SPI 全双工传输
     * @param txData 发送数据
     * @return 接收数据
     */
    std::vector<uint8_t> spiTransfer(const std::vector<uint8_t>& txData);
    
    /**
     * @brief SPI 只发送
     * @param txData 发送数据
     */
    void spiWrite(const std::vector<uint8_t>& txData);
    
    /**
     * @brief SPI 只接收
     * @param count 字节数
     * @return 接收数据
     */
    std::vector<uint8_t> spiRead(uint16_t count);
    
    /**
     * @brief 控制 SPI CS 引脚
     * @param mode AUTO/HIGH/LOW
     */
    void spiCS(const std::string& mode);

    // ========== ADC 控制（3路+温度）==========
    
    /**
     * @brief 单次读取 ADC
     * @param ch 通道
     * @return 读数（原始值 + 电压）
     */
    ADCReading adcRead(ADCChannel ch);
    
    /**
     * @brief 读取全部 ADC 通道
     * @return 3 个通道的读数
     */
    std::vector<ADCReading> adcReadAll();
    
    /**
     * @brief 启动连续采样
     * @param ch 通道
     * @param rateHz 采样率（Hz）
     * @param durationMs 持续时间（毫秒，0=持续直到手动停止）
     */
    void adcStartSample(ADCChannel ch, uint32_t rateHz, uint32_t durationMs = 0);
    
    /**
     * @brief 停止连续采样
     * @param ch 通道
     */
    void adcStopSample(ADCChannel ch);
    
    /**
     * @brief 读取片上温度
     * @return 温度（摄氏度）
     */
    float adcReadTemp();
    
    /**
     * @brief 设置 ADC 阈值事件
     * @param ch 通道
     * @param lowMv 低阈值（mV）
     * @param highMv 高阈值（mV）
     * @param enable 是否启用
     */
    void adcThreshold(ADCChannel ch, uint16_t lowMv, uint16_t highMv, bool enable);

    // ========== 组合时序（SEQ）==========
    
    /**
     * @brief 定义序列
     * @param name 序列名称
     * @param script 脚本内容
     */
    void seqDefine(const std::string& name, const std::string& script);
    
    /**
     * @brief 列出已定义序列
     * @return 序列名称列表
     */
    std::vector<std::string> seqList();
    
    /**
     * @brief 执行序列
     * @param name 序列名称
     * @param repeat 循环次数（0=无限）
     */
    void seqRun(const std::string& name, uint16_t repeat = 1);
    
    /**
     * @brief 停止当前序列
     */
    void seqStop();
    
    /**
     * @brief 删除序列
     * @param name 序列名称
     */
    void seqDelete(const std::string& name);

    // ========== 事件处理 ==========
    
    /**
     * @brief 注册事件回调
     * @param callback 回调函数
     */
    void onEvent(EventCallback callback);
    
    /**
     * @brief 启动事件监听线程
     */
    void startEventListener();
    
    /**
     * @brief 停止事件监听
     */
    void stopEventListener();

    // ========== 工具函数 ==========
    
    /**
     * @brief 发送原始命令
     * @param cmd 命令字符串
     * @return 响应
     */
    Response sendCommand(const std::string& cmd);

private:
    Response sendCommandPrepared(std::string cmd, bool utcNow);
    std::string port_;
    uint32_t baud_;
    SerialPort* serial_;
    mutable std::mutex mutex_;
    std::mutex commandMutex_;
    std::mutex listenerMutex_;
    std::condition_variable responseReady_, eventReady_;
    std::thread readerThread_;
    std::atomic<bool> receiving_{false};
    bool synchronized_ = true;
    std::deque<std::string> responses_, events_;
    void receiveLoop();
    
    // 事件监听
    EventCallback eventCallback_;
    std::thread eventThread_;
    std::atomic<bool> listening_;
    
    // 内部方法
    Response parseResponse(const std::string& line);
    std::vector<uint8_t> hexToBytes(const std::string& hex);
    std::string bytesToHex(const std::vector<uint8_t>& bytes);
    std::string channelName(IOChannel ch);
    std::string channelName(PWMChannel ch);
    std::string channelName(UARTChannel ch);
    std::string channelName(ADCChannel ch);
};

} // namespace iodock
