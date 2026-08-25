/**
 * @file demo.cpp
 * @brief IO Docking Board SDK 调用示例
 * 
 * 演示如何使用 IODock SDK 控制板卡各外设
 * 编译：g++ -std=c++17 demo.cpp iodock.cpp -o demo -lpthread
 * 运行：./demo COM3    (Windows)
 *       ./demo /dev/ttyACM0    (Linux)
 */

#include "iodock.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace iodock;
using namespace std::chrono_literals;

// 辅助函数：打印分隔线
void printSection(const std::string& title) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  " << title << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// 辅助函数：等待用户按键
void waitKey(const std::string& msg = "按回车继续...") {
    std::cout << msg;
    std::cin.get();
}

int main(int argc, char* argv[]) {
    // 检查命令行参数
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <串口>" << std::endl;
        std::cout << "示例: " << argv[0] << " COM3" << std::endl;
        return 1;
    }
    
    std::string port = argv[1];
    
    // ========================================================================
    // 1. 创建 IODock 对象并连接
    // ========================================================================
    printSection("1. 连接板卡");
    
    IODock dock(port);
    
    if (!dock.open()) {
        std::cerr << "错误：无法打开串口 " << port << std::endl;
        return 1;
    }
    std::cout << "串口已打开：" << port << std::endl;
    
    // 测试连接
    if (dock.ping()) {
        std::cout << "PING 成功，板卡在线" << std::endl;
    } else {
        std::cerr << "PING 失败，请检查连接" << std::endl;
        return 1;
    }
    
    // ========================================================================
    // 2. 获取板卡信息
    // ========================================================================
    printSection("2. 板卡信息");
    
    auto info = dock.getInfo();
    for (const auto& [key, val] : info) {
        std::cout << "  " << key << " = " << val << std::endl;
    }
    
    waitKey();
    
    // ========================================================================
    // 3. IO 控制示例
    // ========================================================================
    printSection("3. IO 控制");
    
    // 配置 IO1 为输出
    std::cout << "配置 IO1 为输出模式..." << std::endl;
    dock.ioConfig(IOChannel::IO1, Direction::OUTPUT);
    
    // 输出高电平
    std::cout << "IO1 输出高电平（LED 灭）..." << std::endl;
    dock.ioWrite(IOChannel::IO1, Level::HIGH);
    std::this_thread::sleep_for(500ms);
    
    // 输出低电平
    std::cout << "IO1 输出低电平（LED 亮）..." << std::endl;
    dock.ioWrite(IOChannel::IO1, Level::LOW);
    std::this_thread::sleep_for(500ms);
    
    // 翻转输出
    std::cout << "IO1 翻转输出..." << std::endl;
    dock.ioToggle(IOChannel::IO1);
    std::this_thread::sleep_for(500ms);
    
    // 输出脉冲
    std::cout << "IO1 输出 200ms 脉冲..." << std::endl;
    dock.ioPulse(IOChannel::IO1, Level::LOW, 200);
    std::this_thread::sleep_for(300ms);
    
    // 配置 IO2 为上拉输入
    std::cout << "\n配置 IO2 为上拉输入..." << std::endl;
    dock.ioConfig(IOChannel::IO2, Direction::INPUT, PullMode::PULLUP);
    
    // 读取输入
    Level level = dock.ioRead(IOChannel::IO2);
    std::cout << "IO2 当前电平：" << (level == Level::HIGH ? "HIGH" : "LOW") << std::endl;
    
    // 读取全部 IO
    std::cout << "\n读取全部 IO 状态：" << std::endl;
    auto allLevels = dock.ioReadAll();
    for (int i = 0; i < 6; i++) {
        std::cout << "  IO" << (i + 1) << ": " 
                  << (allLevels[i] == Level::HIGH ? "HIGH" : "LOW") << std::endl;
    }
    
    waitKey();
    
    // ========================================================================
    // 4. PWM 控制示例
    // ========================================================================
    printSection("4. PWM 控制");
    
    // 配置 PWM1 为 1kHz，50% 占空比
    std::cout << "配置 PWM1: 1kHz, 50% 占空比..." << std::endl;
    dock.pwmConfig(PWMChannel::PWM1, 1000, 50);
    
    // 启动 PWM
    std::cout << "启动 PWM1..." << std::endl;
    dock.pwmStart(PWMChannel::PWM1);
    std::this_thread::sleep_for(1s);
    
    // 修改占空比
    std::cout << "修改占空比为 25%..." << std::endl;
    dock.pwmSetDuty(PWMChannel::PWM1, 25);
    std::this_thread::sleep_for(1s);
    
    // 呼吸灯效果
    std::cout << "运行呼吸灯效果（0→100→0）..." << std::endl;
    dock.pwmSweep(PWMChannel::PWM1, 0, 100, 10, 1);
    std::this_thread::sleep_for(3s);
    
    // 停止 PWM
    std::cout << "停止 PWM1..." << std::endl;
    dock.pwmStop(PWMChannel::PWM1);
    
    // 多路同步示例
    std::cout << "\n配置 PWM1 和 PWM2 同频同步..." << std::endl;
    dock.pwmConfig(PWMChannel::PWM1, 1000, 50);
    dock.pwmConfig(PWMChannel::PWM2, 1000, 50);
    dock.pwmSync({PWMChannel::PWM1, PWMChannel::PWM2});
    std::this_thread::sleep_for(2s);
    
    // 设置相位偏移
    std::cout << "PWM2 相对 PWM1 相位滞后 90°..." << std::endl;
    dock.pwmSetPhase(PWMChannel::PWM2, PWMChannel::PWM1, 90);
    std::this_thread::sleep_for(2s);
    
    dock.pwmStop(PWMChannel::PWM1);
    dock.pwmStop(PWMChannel::PWM2);
    
    waitKey();
    
    // ========================================================================
    // 5. UART 控制示例
    // ========================================================================
    printSection("5. UART 控制");
    
    // 配置 USART1
    std::cout << "配置 USART1: 9600 8N1..." << std::endl;
    dock.uartConfig(UARTChannel::USART1, 9600);
    
    // 发送文本
    std::cout << "发送文本 'Hello'..." << std::endl;
    dock.uartSendText(UARTChannel::USART1, "Hello");
    
    // 发送十六进制数据
    std::cout << "发送十六进制数据 0x01 0x02 0x03..." << std::endl;
    dock.uartSendHex(UARTChannel::USART1, {0x01, 0x02, 0x03});
    
    // 读取接收缓冲
    std::cout << "读取接收缓冲..." << std::endl;
    auto rxData = dock.uartReceive(UARTChannel::USART1);
    if (!rxData.empty()) {
        std::cout << "收到 " << rxData.size() << " 字节: ";
        for (auto b : rxData) {
            printf("0x%02X ", b);
        }
        std::cout << std::endl;
    } else {
        std::cout << "接收缓冲为空" << std::endl;
    }
    
    // 启用透传模式
    std::cout << "\n启用 USART1 透传模式..." << std::endl;
    dock.uartStream(UARTChannel::USART1, true);
    std::cout << "（透传模式下收到的数据会通过事件上报）" << std::endl;
    std::this_thread::sleep_for(2s);
    dock.uartStream(UARTChannel::USART1, false);
    
    waitKey();
    
    // ========================================================================
    // 6. I2C 控制示例
    // ========================================================================
    printSection("6. I2C 控制");
    
    // 设置 I2C 速率
    std::cout << "设置 I2C 速率为 400kHz..." << std::endl;
    dock.i2cSetSpeed(400);
    
    // 扫描总线
    std::cout << "扫描 I2C 总线..." << std::endl;
    auto addrs = dock.i2cScan();
    if (!addrs.empty()) {
        std::cout << "发现 " << addrs.size() << " 个从机: ";
        for (auto addr : addrs) {
            printf("0x%02X ", addr);
        }
        std::cout << std::endl;
        
        // 尝试读取第一个设备的寄存器 0x00
        uint8_t devAddr = addrs[0];
        std::cout << "\n读取设备 0x" << std::hex << (int)devAddr 
                  << " 的寄存器 0x00 (4字节)..." << std::endl;
        auto data = dock.i2cReadReg(devAddr, 0x00, 4);
        if (!data.empty()) {
            std::cout << "读取成功: ";
            for (auto b : data) {
                printf("0x%02X ", b);
            }
            std::cout << std::endl;
        }
    } else {
        std::cout << "未发现 I2C 设备" << std::endl;
    }
    
    waitKey();
    
    // ========================================================================
    // 7. SPI 控制示例
    // ========================================================================
    printSection("7. SPI 控制");
    
    // 配置 SPI
    std::cout << "配置 SPI: 1MHz, 模式0, MSB, 8位..." << std::endl;
    dock.spiConfig(1000000, 0, "MSB", 8);
    
    // 全双工传输
    std::cout << "SPI 全双工传输 0xDEADBEEF..." << std::endl;
    auto spiRx = dock.spiTransfer({0xDE, 0xAD, 0xBE, 0xEF});
    std::cout << "收到: ";
    for (auto b : spiRx) {
        printf("0x%02X ", b);
    }
    std::cout << std::endl;
    
    // 手动控制 CS
    std::cout << "\n手动拉低 CS..." << std::endl;
    dock.spiCS("LOW");
    dock.spiWrite({0x01, 0x02});
    std::cout << "手动拉高 CS..." << std::endl;
    dock.spiCS("HIGH");
    
    waitKey();
    
    // ========================================================================
    // 8. ADC 控制示例
    // ========================================================================
    printSection("8. ADC 控制");
    
    // 单次读取
    std::cout << "读取 ADC0..." << std::endl;
    auto adcVal = dock.adcRead(ADCChannel::ADC0);
    std::cout << "  原始值: " << adcVal.raw << std::endl;
    std::cout << "  电压: " << adcVal.millivolts << " mV" << std::endl;
    
    // 读取全部通道
    std::cout << "\n读取全部 ADC 通道：" << std::endl;
    auto allAdc = dock.adcReadAll();
    for (int i = 0; i < 3; i++) {
        std::cout << "  ADC" << i << ": " << allAdc[i].raw 
                  << " (" << allAdc[i].millivolts << " mV)" << std::endl;
    }
    
    // 读取温度
    std::cout << "\n读取片上温度..." << std::endl;
    float temp = dock.adcReadTemp();
    std::cout << "  温度: " << temp << " °C" << std::endl;
    
    // 连续采样示例
    std::cout << "\n启动 ADC0 连续采样: 100Hz, 1秒..." << std::endl;
    dock.adcStartSample(ADCChannel::ADC0, 100, 1000);
    std::cout << "（采样数据会通过 DATA 事件上报）" << std::endl;
    std::this_thread::sleep_for(1500ms);
    
    // 设置阈值事件
    std::cout << "\n设置 ADC0 阈值事件: 1000mV ~ 2000mV..." << std::endl;
    dock.adcThreshold(ADCChannel::ADC0, 1000, 2000, true);
    
    waitKey();
    
    // ========================================================================
    // 9. 组合时序示例
    // ========================================================================
    printSection("9. 组合时序");
    
    // 定义一个简单的序列
    std::string script = "IO IO1 LOW@0; WAIT 200; IO IO1 HIGH@200; WAIT 200; IO IO1 LOW@400";
    
    std::cout << "定义序列 'blink'：" << std::endl;
    std::cout << "  " << script << std::endl;
    dock.seqDefine("blink", script);
    
    // 列出已定义序列
    std::cout << "\n已定义的序列：" << std::endl;
    auto seqs = dock.seqList();
    for (const auto& name : seqs) {
        std::cout << "  - " << name << std::endl;
    }
    
    // 执行序列
    std::cout << "\n执行序列 'blink' (循环3次)..." << std::endl;
    dock.seqRun("blink", 3);
    std::this_thread::sleep_for(3s);
    
    // 删除序列
    std::cout << "\n删除序列 'blink'..." << std::endl;
    dock.seqDelete("blink");
    
    waitKey();
    
    // ========================================================================
    // 10. 事件监听示例
    // ========================================================================
    printSection("10. 事件监听");
    
    // 注册事件回调
    std::cout << "注册事件回调..." << std::endl;
    dock.onEvent([](const std::string& event, const std::string& data) {
        std::cout << "[事件] " << event << ": " << data << std::endl;
    });
    
    // 启动事件监听
    std::cout << "启动事件监听线程..." << std::endl;
    dock.startEventListener();
    
    // 使能 IO2 边沿事件
    std::cout << "使能 IO2 下降沿事件..." << std::endl;
    dock.ioConfig(IOChannel::IO2, Direction::INPUT, PullMode::PULLUP);
    dock.ioEvent(IOChannel::IO2, true, Edge::FALLING);
    
    std::cout << "\n请手动触发 IO2（连接到 GND），观察事件上报..." << std::endl;
    std::cout << "等待 5 秒..." << std::endl;
    std::this_thread::sleep_for(5s);
    
    // 停止事件监听
    dock.stopEventListener();
    dock.ioEvent(IOChannel::IO2, false);
    
    // ========================================================================
    // 11. 清理
    // ========================================================================
    printSection("11. 清理退出");
    
    // 关闭所有输出
    std::cout << "关闭所有外设..." << std::endl;
    dock.ioWrite(IOChannel::IO1, Level::HIGH);  // LED 灭
    dock.pwmStop(PWMChannel::PWM1);
    dock.pwmStop(PWMChannel::PWM2);
    
    // 关闭连接
    dock.close();
    std::cout << "连接已关闭" << std::endl;
    
    std::cout << "\n演示完成！" << std::endl;
    
    return 0;
}
