/**
 * @file iodock.cpp
 * @brief IO Docking Board C++ SDK 实现文件
 */

#include "iodock.h"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace iodock {

// ============================================================================
// 串口实现（跨平台）
// ============================================================================

class SerialPort {
public:
    SerialPort(const std::string& port, uint32_t baud) 
        : port_(port), baud_(baud), handle_(-1) {}
    
    ~SerialPort() { close(); }
    
    bool open() {
#ifdef _WIN32
        std::string fullPort = "\\\\.\\" + port_;
        handle_ = (intptr_t)CreateFileA(fullPort.c_str(), 
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, 
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        
        if (handle_ == -1) return false;
        
        DCB dcb = {0};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState((HANDLE)handle_, &dcb)) {
            close();
            return false;
        }
        
        dcb.BaudRate = baud_;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        
        if (!SetCommState((HANDLE)handle_, &dcb)) {
            close();
            return false;
        }
        
        // 设置超时
        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.ReadTotalTimeoutConstant = 100;
        SetCommTimeouts((HANDLE)handle_, &timeouts);
        
        return true;
#else
        handle_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (handle_ < 0) return false;
        
        struct termios tty;
        if (tcgetattr(handle_, &tty) != 0) {
            close();
            return false;
        }
        
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);
        
        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag |= CLOCAL | CREAD;
        
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_lflag = 0;
        tty.c_oflag = 0;
        
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1; // 0.1秒超时
        
        if (tcsetattr(handle_, TCSANOW, &tty) != 0) {
            close();
            return false;
        }
        
        return true;
#endif
    }
    
    void close() {
#ifdef _WIN32
        if (handle_ != -1) {
            CloseHandle((HANDLE)handle_);
            handle_ = -1;
        }
#else
        if (handle_ >= 0) {
            ::close(handle_);
            handle_ = -1;
        }
#endif
    }
    
    bool isOpen() const { return handle_ != -1; }
    
    int write(const std::string& data) {
#ifdef _WIN32
        DWORD written;
        if (!WriteFile((HANDLE)handle_, data.c_str(), data.size(), &written, nullptr)) {
            return -1;
        }
        return written;
#else
        return ::write(handle_, data.c_str(), data.size());
#endif
    }
    
    std::string readLine(uint32_t timeoutMs = 1000) {
        std::string result;
        auto start = std::chrono::steady_clock::now();
        
        while (true) {
            char c;
            int n = read(&c, 1);
            
            if (n > 0) {
                if (c == '\n') {
                    // 去除末尾 \r
                    if (!result.empty() && result.back() == '\r') {
                        result.pop_back();
                    }
                    return result;
                }
                result += c;
            }
            
            // 检查超时
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeoutMs) {
                break;
            }
        }
        
        return result;
    }
    
    void flush() {
#ifdef _WIN32
        PurgeComm((HANDLE)handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        tcflush(handle_, TCIOFLUSH);
#endif
    }

private:
    int read(void* buf, size_t count) {
#ifdef _WIN32
        DWORD bytesRead;
        if (!ReadFile((HANDLE)handle_, buf, count, &bytesRead, nullptr)) {
            return -1;
        }
        return bytesRead;
#else
        return ::read(handle_, buf, count);
#endif
    }
    
    std::string port_;
    uint32_t baud_;
    intptr_t handle_;
};

// ============================================================================
// IODock 实现
// ============================================================================

IODock::IODock(const std::string& port, uint32_t baud)
    : port_(port), baud_(baud), serial_(nullptr), listening_(false) {}

IODock::~IODock() {
    stopEventListener();
    close();
}

// ---------- 连接管理 ----------

bool IODock::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (serial_ && serial_->isOpen()) {
        return true;
    }
    
    serial_ = new SerialPort(port_, baud_);
    if (!serial_->open()) {
        delete serial_;
        serial_ = nullptr;
        return false;
    }
    
    // 清空缓冲区
    serial_->flush();
    
    return true;
}

void IODock::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (serial_) {
        serial_->close();
        delete serial_;
        serial_ = nullptr;
    }
}

bool IODock::isOpen() const {
    return serial_ && serial_->isOpen();
}

bool IODock::ping() {
    auto resp = sendCommand("PING");
    return resp.success && resp.payload.find("PONG") != std::string::npos;
}

// ---------- 系统命令 ----------

std::map<std::string, std::string> IODock::getInfo() {
    std::map<std::string, std::string> info;
    auto resp = sendCommand("INFO");
    
    if (!resp.success) return info;
    
    std::istringstream iss(resp.payload);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            // 去除首尾空格
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);
            info[key] = val;
        }
    }
    
    return info;
}

void IODock::reset() {
    sendCommand("RESET");
}

void IODock::enterBootloader() {
    sendCommand("BOOTLOADER");
}

void IODock::setLED(bool on) {
    sendCommand(std::string("LED ") + (on ? "ON" : "OFF"));
}

int64_t IODock::syncTime(uint64_t hostUs) {
    auto resp = sendCommand("SYNC " + std::to_string(hostUs));
    if (!resp.success) return 0;
    
    // 解析 offset=xxx
    auto pos = resp.payload.find("offset=");
    if (pos == std::string::npos) return 0;
    
    return std::stoll(resp.payload.substr(pos + 7));
}

// ---------- IO 控制 ----------

void IODock::ioConfig(IOChannel ch, Direction dir, PullMode pull) {
    std::string cmd = "IO CFG " + channelName(ch);
    cmd += (dir == Direction::INPUT) ? " IN" : " OUT";
    
    switch (pull) {
        case PullMode::PULLUP:   cmd += " PULLUP"; break;
        case PullMode::PULLDOWN: cmd += " PULLDOWN"; break;
        case PullMode::FLOAT:    cmd += " FLOAT"; break;
    }
    
    sendCommand(cmd);
}

void IODock::ioWrite(IOChannel ch, Level level) {
    sendCommand("IO WRITE " + channelName(ch) + 
                (level == Level::HIGH ? " HIGH" : " LOW"));
}

Level IODock::ioRead(IOChannel ch) {
    auto resp = sendCommand("IO READ " + channelName(ch));
    if (resp.success && resp.payload.find("HIGH") != std::string::npos) {
        return Level::HIGH;
    }
    return Level::LOW;
}

std::vector<Level> IODock::ioReadAll() {
    std::vector<Level> levels(6, Level::LOW);
    auto resp = sendCommand("IO READALL");
    
    if (!resp.success) return levels;
    
    // 解析 "IO1 HIGH IO2 LOW ..." 格式
    for (int i = 0; i < 6; i++) {
        std::string key = "IO" + std::to_string(i + 1);
        auto pos = resp.payload.find(key);
        if (pos != std::string::npos) {
            auto highPos = resp.payload.find("HIGH", pos);
            auto lowPos = resp.payload.find("LOW", pos);
            if (highPos != std::string::npos && 
                (lowPos == std::string::npos || highPos < lowPos)) {
                levels[i] = Level::HIGH;
            }
        }
    }
    
    return levels;
}

void IODock::ioToggle(IOChannel ch) {
    sendCommand("IO TOGGLE " + channelName(ch));
}

void IODock::ioPulse(IOChannel ch, Level level, uint16_t ms) {
    sendCommand("IO PULSE " + channelName(ch) + 
                (level == Level::HIGH ? " HIGH " : " LOW ") + 
                std::to_string(ms));
}

void IODock::ioEvent(IOChannel ch, bool enable, Edge edge) {
    std::string cmd = "IO EVENT " + channelName(ch);
    cmd += enable ? " ON" : " OFF";
    
    if (enable) {
        switch (edge) {
            case Edge::RISING:  cmd += " RISING"; break;
            case Edge::FALLING: cmd += " FALLING"; break;
            case Edge::CHANGE:  cmd += " CHANGE"; break;
        }
    }
    
    sendCommand(cmd);
}

// ---------- PWM 控制 ----------

void IODock::pwmConfig(PWMChannel ch, uint32_t freqHz, uint16_t duty, bool isPercent) {
    std::string cmd = "PWM CFG " + channelName(ch) + " " + 
                      std::to_string(freqHz) + " " + std::to_string(duty);
    sendCommand(cmd);
}

void IODock::pwmSetFreq(PWMChannel ch, uint32_t freqHz) {
    sendCommand("PWM FREQ " + channelName(ch) + " " + std::to_string(freqHz));
}

void IODock::pwmSetDuty(PWMChannel ch, uint16_t duty) {
    sendCommand("PWM DUTY " + channelName(ch) + " " + std::to_string(duty));
}

void IODock::pwmStart(PWMChannel ch) {
    sendCommand("PWM START " + channelName(ch));
}

void IODock::pwmStop(PWMChannel ch) {
    sendCommand("PWM STOP " + channelName(ch));
}

void IODock::pwmSweep(PWMChannel ch, uint16_t from, uint16_t to, 
                      uint16_t stepMs, uint16_t repeat) {
    sendCommand("PWM SWEEP " + channelName(ch) + " " +
                std::to_string(from) + " " + std::to_string(to) + " " +
                std::to_string(stepMs) + " " + std::to_string(repeat));
}

PWMConfig IODock::pwmRead(PWMChannel ch) {
    PWMConfig config = {0, 0, false, false};
    auto resp = sendCommand("PWM READ " + channelName(ch));
    
    if (resp.success) {
        // 解析响应（简化处理）
        if (resp.payload.find("running") != std::string::npos) {
            config.running = true;
        }
    }
    
    return config;
}

void IODock::pwmSync(const std::vector<PWMChannel>& channels) {
    std::string cmd = "PWM SYNC";
    for (size_t i = 0; i < channels.size(); i++) {
        if (i > 0) cmd += ",";
        cmd += channelName(channels[i]);
    }
    sendCommand(cmd);
}

void IODock::pwmSetPhase(PWMChannel ch, PWMChannel ref, uint16_t degrees) {
    sendCommand("PWM PHASE " + channelName(ch) + " " + 
                channelName(ref) + " " + std::to_string(degrees));
}

void IODock::pwmSetPolarity(PWMChannel ch, Polarity pol) {
    sendCommand("PWM POL " + channelName(ch) + 
                (pol == Polarity::INVERT ? " INVERT" : " NORMAL"));
}

void IODock::pwmTick(PWMChannel ch, uint32_t freqHz) {
    sendCommand("PWM TICK " + channelName(ch) + " " + std::to_string(freqHz));
}

void IODock::pwmTickOff(PWMChannel ch) {
    sendCommand("PWM TICK " + channelName(ch) + " OFF");
}

// ---------- UART 控制 ----------

void IODock::uartConfig(UARTChannel ch, uint32_t baud, uint8_t dataBits, 
                        char parity, uint8_t stopBits) {
    std::string cmd = "UART CFG " + channelName(ch) + " " + std::to_string(baud) +
                      " " + std::to_string(dataBits) + " " + parity + 
                      " " + std::to_string(stopBits);
    sendCommand(cmd);
}

void IODock::uartSendText(UARTChannel ch, const std::string& text) {
    sendCommand("UART TX " + channelName(ch) + " TXT:" + text);
}

void IODock::uartSendHex(UARTChannel ch, const std::vector<uint8_t>& data) {
    sendCommand("UART TX " + channelName(ch) + " HEX:" + bytesToHex(data));
}

std::vector<uint8_t> IODock::uartReceive(UARTChannel ch, uint16_t maxBytes) {
    std::string cmd = "UART RX " + channelName(ch);
    if (maxBytes > 0) {
        cmd += " " + std::to_string(maxBytes);
    }
    
    auto resp = sendCommand(cmd);
    if (!resp.success) return {};
    
    // 解析 HEX:xxxx 格式
    auto pos = resp.payload.find("HEX:");
    if (pos == std::string::npos) return {};
    
    return hexToBytes(resp.payload.substr(pos + 4));
}

void IODock::uartFlush(UARTChannel ch) {
    sendCommand("UART FLUSH " + channelName(ch));
}

void IODock::uartStream(UARTChannel ch, bool enable) {
    sendCommand("UART STREAM " + channelName(ch) + 
                (enable ? " ON" : " OFF"));
}

// ---------- I2C 控制 ----------

void IODock::i2cSetSpeed(uint32_t khz) {
    sendCommand("I2C CFG " + std::to_string(khz));
}

std::vector<uint8_t> IODock::i2cScan() {
    std::vector<uint8_t> addrs;
    auto resp = sendCommand("I2C SCAN");
    
    if (!resp.success) return addrs;
    
    // 解析 "0x3C 0x50 ..." 格式
    std::istringstream iss(resp.payload);
    std::string token;
    while (iss >> token) {
        if (token.substr(0, 2) == "0x" || token.substr(0, 2) == "0X") {
            addrs.push_back(std::stoi(token, nullptr, 16));
        }
    }
    
    return addrs;
}

void IODock::i2cWriteReg(uint8_t addr, uint8_t reg, const std::vector<uint8_t>& data) {
    std::string cmd = "I2C WRITE 0x" + bytesToHex({addr}) + 
                      " 0x" + bytesToHex({reg});
    for (auto b : data) {
        cmd += " 0x" + bytesToHex({b});
    }
    sendCommand(cmd);
}

std::vector<uint8_t> IODock::i2cReadReg(uint8_t addr, uint8_t reg, uint16_t count) {
    auto resp = sendCommand("I2C READ 0x" + bytesToHex({addr}) + 
                            " 0x" + bytesToHex({reg}) + " " + 
                            std::to_string(count));
    
    if (!resp.success) return {};
    
    auto pos = resp.payload.find("HEX:");
    if (pos == std::string::npos) return {};
    
    return hexToBytes(resp.payload.substr(pos + 4));
}

void IODock::i2cWrite(uint8_t addr, const std::vector<uint8_t>& data) {
    std::string cmd = "I2C WRONLY 0x" + bytesToHex({addr});
    for (auto b : data) {
        cmd += " 0x" + bytesToHex({b});
    }
    sendCommand(cmd);
}

std::vector<uint8_t> IODock::i2cRead(uint8_t addr, uint16_t count) {
    auto resp = sendCommand("I2C RONLY 0x" + bytesToHex({addr}) + 
                            " " + std::to_string(count));
    
    if (!resp.success) return {};
    
    auto pos = resp.payload.find("HEX:");
    if (pos == std::string::npos) return {};
    
    return hexToBytes(resp.payload.substr(pos + 4));
}

// ---------- SPI 控制 ----------

void IODock::spiConfig(uint32_t baud, uint8_t mode, 
                       const std::string& bitOrder, uint8_t bits) {
    sendCommand("SPI CFG " + std::to_string(baud) + " " + 
                std::to_string(mode) + " " + bitOrder + " " + 
                std::to_string(bits));
}

std::vector<uint8_t> IODock::spiTransfer(const std::vector<uint8_t>& txData) {
    auto resp = sendCommand("SPI XFR HEX:" + bytesToHex(txData));
    
    if (!resp.success) return {};
    
    auto pos = resp.payload.find("HEX:");
    if (pos == std::string::npos) return {};
    
    return hexToBytes(resp.payload.substr(pos + 4));
}

void IODock::spiWrite(const std::vector<uint8_t>& txData) {
    sendCommand("SPI WRITE HEX:" + bytesToHex(txData));
}

std::vector<uint8_t> IODock::spiRead(uint16_t count) {
    auto resp = sendCommand("SPI READ " + std::to_string(count));
    
    if (!resp.success) return {};
    
    auto pos = resp.payload.find("HEX:");
    if (pos == std::string::npos) return {};
    
    return hexToBytes(resp.payload.substr(pos + 4));
}

void IODock::spiCS(const std::string& mode) {
    sendCommand("SPI CS " + mode);
}

// ---------- ADC 控制 ----------

ADCReading IODock::adcRead(ADCChannel ch) {
    ADCReading reading = {0, 0};
    auto resp = sendCommand("ADC READ " + channelName(ch));
    
    if (resp.success) {
        // 解析 "2048 1650mV" 格式
        std::istringstream iss(resp.payload);
        std::string rawStr, mvStr;
        iss >> rawStr >> mvStr;
        
        try {
            reading.raw = std::stoi(rawStr);
            // 去除 "mV" 后缀
            if (mvStr.back() == 'V') mvStr.pop_back();
            if (mvStr.back() == 'm') mvStr.pop_back();
            reading.millivolts = std::stoi(mvStr);
        } catch (...) {}
    }
    
    return reading;
}

std::vector<ADCReading> IODock::adcReadAll() {
    std::vector<ADCReading> readings(3, {0, 0});
    auto resp = sendCommand("ADC READALL");
    
    if (!resp.success) return readings;
    
    // 解析多行响应（简化处理）
    std::istringstream iss(resp.payload);
    std::string line;
    int idx = 0;
    while (std::getline(iss, line) && idx < 3) {
        std::istringstream lineStream(line);
        std::string rawStr, mvStr;
        lineStream >> rawStr >> mvStr;
        
        try {
            readings[idx].raw = std::stoi(rawStr);
            if (!mvStr.empty() && mvStr.back() == 'V') mvStr.pop_back();
            if (!mvStr.empty() && mvStr.back() == 'm') mvStr.pop_back();
            readings[idx].millivolts = std::stoi(mvStr);
        } catch (...) {}
        
        idx++;
    }
    
    return readings;
}

void IODock::adcStartSample(ADCChannel ch, uint32_t rateHz, uint32_t durationMs) {
    std::string cmd = "ADC SAMPLE " + channelName(ch) + " " + 
                      std::to_string(rateHz);
    if (durationMs > 0) {
        cmd += " " + std::to_string(durationMs);
    }
    sendCommand(cmd);
}

void IODock::adcStopSample(ADCChannel ch) {
    sendCommand("ADC SAMPLE " + channelName(ch) + " STOP");
}

float IODock::adcReadTemp() {
    auto resp = sendCommand("ADC TEMP");
    
    if (!resp.success) return 0.0f;
    
    // 解析 "27.3C" 格式
    std::string tempStr = resp.payload;
    if (tempStr.back() == 'C') tempStr.pop_back();
    
    try {
        return std::stof(tempStr);
    } catch (...) {
        return 0.0f;
    }
}

void IODock::adcThreshold(ADCChannel ch, uint16_t lowMv, uint16_t highMv, bool enable) {
    sendCommand("ADC THRESH " + channelName(ch) + " " + 
                std::to_string(lowMv) + " " + std::to_string(highMv) + 
                (enable ? " ON" : " OFF"));
}

// ---------- 组合时序 ----------

void IODock::seqDefine(const std::string& name, const std::string& script) {
    sendCommand("SEQ DEF " + name + " \"" + script + "\"");
}

std::vector<std::string> IODock::seqList() {
    std::vector<std::string> names;
    auto resp = sendCommand("SEQ LIST");
    
    if (!resp.success) return names;
    
    std::istringstream iss(resp.payload);
    std::string name;
    while (iss >> name) {
        if (!name.empty() && name != "END") {
            names.push_back(name);
        }
    }
    
    return names;
}

void IODock::seqRun(const std::string& name, uint16_t repeat) {
    sendCommand("SEQ RUN " + name + " " + std::to_string(repeat));
}

void IODock::seqStop() {
    sendCommand("SEQ STOP");
}

void IODock::seqDelete(const std::string& name) {
    sendCommand("SEQ DEL " + name);
}

// ---------- 事件处理 ----------

void IODock::onEvent(EventCallback callback) {
    eventCallback_ = callback;
}

void IODock::startEventListener() {
    if (listening_) return;
    
    listening_ = true;
    eventThread_ = std::thread([this]() {
        while (listening_ && isOpen()) {
            std::string line;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (serial_) {
                    line = serial_->readLine(100);
                }
            }
            
            if (!line.empty()) {
                // 检查是否是事件
                if (line.substr(0, 4) == "EVT ") {
                    auto spacePos = line.find(' ', 4);
                    if (spacePos != std::string::npos) {
                        std::string event = line.substr(4, spacePos - 4);
                        std::string data = line.substr(spacePos + 1);
                        
                        if (eventCallback_) {
                            eventCallback_(event, data);
                        }
                    }
                }
            }
        }
    });
}

void IODock::stopEventListener() {
    listening_ = false;
    if (eventThread_.joinable()) {
        eventThread_.join();
    }
}

// ---------- 工具函数 ----------

Response IODock::sendCommand(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Response resp;
    resp.success = false;
    resp.command = cmd;
    resp.errorCode = 0;
    
    if (!serial_ || !serial_->isOpen()) {
        resp.error = "串口未打开";
        return resp;
    }
    
    // 清空接收缓冲
    serial_->flush();
    
    // 发送命令
    std::string fullCmd = cmd + "\n";
    if (serial_->write(fullCmd) != fullCmd.size()) {
        resp.error = "发送失败";
        return resp;
    }
    
    // 读取响应（可能多行）
    std::string line = serial_->readLine(1000);
    if (line.empty()) {
        resp.error = "响应超时";
        return resp;
    }
    
    return parseResponse(line);
}

Response IODock::parseResponse(const std::string& line) {
    Response resp;
    resp.success = false;
    
    if (line.substr(0, 3) == "OK ") {
        resp.success = true;
        resp.payload = line.substr(3);
        
        // 提取命令名
        auto spacePos = resp.payload.find(' ');
        if (spacePos != std::string::npos) {
            resp.command = resp.payload.substr(0, spacePos);
            resp.payload = resp.payload.substr(spacePos + 1);
        }
    } else if (line.substr(0, 4) == "ERR ") {
        resp.success = false;
        std::string content = line.substr(4);
        
        // 解析 "CMD E_CODE message" 格式
        std::istringstream iss(content);
        iss >> resp.command;
        
        std::string codeStr;
        iss >> codeStr;
        
        // 错误码映射
        static std::map<std::string, int> errorCodes = {
            {"E_BADCMD", 0}, {"E_PARAM", 1}, {"E_NOTFOUND", 2},
            {"E_RANGE", 3}, {"E_CFG", 4}, {"E_BUSY", 5},
            {"E_TIMEOUT", 6}, {"E_NACK", 7}, {"E_OVERRUN", 8},
            {"E_IO", 9}, {"E_LONG", 10}, {"E_DENIED", 11}
        };
        
        auto it = errorCodes.find(codeStr);
        if (it != errorCodes.end()) {
            resp.errorCode = it->second;
        }
        
        // 剩余部分作为错误信息
        std::string msg;
        std::getline(iss, msg);
        resp.error = msg.empty() ? codeStr : msg;
    } else {
        // 可能是多行响应的第一行
        resp.success = true;
        resp.payload = line;
    }
    
    return resp;
}

std::vector<uint8_t> IODock::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        std::string byteStr = hex.substr(i, 2);
        try {
            bytes.push_back(std::stoi(byteStr, nullptr, 16));
        } catch (...) {
            break;
        }
    }
    
    return bytes;
}

std::string IODock::bytesToHex(const std::vector<uint8_t>& bytes) {
    static const char hexChars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 2);
    
    for (auto b : bytes) {
        result += hexChars[(b >> 4) & 0x0F];
        result += hexChars[b & 0x0F];
    }
    
    return result;
}

std::string IODock::channelName(IOChannel ch) {
    return "IO" + std::to_string(static_cast<int>(ch) + 1);
}

std::string IODock::channelName(PWMChannel ch) {
    return "PWM" + std::to_string(static_cast<int>(ch) + 1);
}

std::string IODock::channelName(UARTChannel ch) {
    return "USART" + std::to_string(static_cast<int>(ch) + 1);
}

std::string IODock::channelName(ADCChannel ch) {
    return "ADC" + std::to_string(static_cast<int>(ch));
}

} // namespace iodock
