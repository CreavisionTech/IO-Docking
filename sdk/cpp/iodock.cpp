/**
 * @file iodock.cpp
 * @brief IO Docking Board C++ SDK 实现文件
 */

#include "iodock.h"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <regex>
#include <cerrno>
#include <cctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace iodock {
namespace { thread_local IODock* callbackDock = nullptr; }

// ============================================================================
// 串口实现（跨平台）
// ============================================================================

#ifdef IODOCK_TEST_TRANSPORT
#include "tests/mock_serial.inc"
#endif
class SerialPort {
public:
    SerialPort(const std::string& port, uint32_t baud) 
        : port_(port), baud_(baud), handle_(-1) {}
    
    ~SerialPort() { close(); }
    
    bool open() {
#ifdef IODOCK_TEST_TRANSPORT
        handle_ = 1; return true;
#elif defined(_WIN32)
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
        // Do not inherit a previous application's handshake/flow-control state.
        // TinyUSB CDC accepts commands only while DTR is asserted.
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fAbortOnError = FALSE;
        
        if (!SetCommState((HANDLE)handle_, &dcb)) {
            close();
            return false;
        }
        
        // 设置超时
        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = 10;
        timeouts.WriteTotalTimeoutConstant = 1000;
        if (!SetCommTimeouts((HANDLE)handle_, &timeouts)) { close(); return false; }
        
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
#ifdef CRTSCTS
        tty.c_cflag &= ~CRTSCTS;
#endif
        
        cfmakeraw(&tty);
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
#ifdef IODOCK_TEST_TRANSPORT
        handle_ = -1;
#elif defined(_WIN32)
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
#ifdef IODOCK_TEST_TRANSPORT
        return mock::write(data);
#elif defined(_WIN32)
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

        auto start = std::chrono::steady_clock::now();
        
        while (true) {
            char c;
            int n = read(&c, 1);
            
            if (n > 0) {
                if (c == '\n') {
                    // 去除末尾 \r
                    if (!partial_.empty() && partial_.back() == '\r') {
                        partial_.pop_back();
                    }
                    std::string result;
                    result.swap(partial_);
                    return result;
                }
                partial_ += c;
            }
            
            if (n < 0) throw std::runtime_error("Serial read failed");
            if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // 检查超时
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeoutMs) {
                break;
            }
        }
        
        return {}; // Keep incomplete lines for the next read.
    }
    

private:
    int read(void* buf, size_t count) {
#ifdef IODOCK_TEST_TRANSPORT
        return mock::read(buf, count);
#elif defined(_WIN32)
        DWORD bytesRead;
        if (!ReadFile((HANDLE)handle_, buf, count, &bytesRead, nullptr)) {
            return -1;
        }
        return bytesRead;
#else
        int n = ::read(handle_, buf, count);
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : n;
#endif
    }
    
    std::string port_;
    uint32_t baud_;
    intptr_t handle_;
    std::string partial_;
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
    std::lock_guard<std::mutex> commandLock(commandMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (serial_) return receiving_ && synchronized_;
    serial_ = new SerialPort(port_, baud_);
    if (!serial_->open()) {
        delete serial_;
        serial_ = nullptr;
        return false;
    }
    responses_.clear();
    events_.clear();
    synchronized_ = true;
    receiving_ = true;
    readerThread_ = std::thread(&IODock::receiveLoop, this);
    return true;
}

void IODock::close() {
    receiving_ = false;
    responseReady_.notify_all();
    stopEventListener();
    std::lock_guard<std::mutex> commandLock(commandMutex_);
    if (readerThread_.joinable()) readerThread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    delete serial_;
    serial_ = nullptr;
}

bool IODock::isOpen() const {
    return receiving_;
}

void IODock::receiveLoop() {
    try {
        while (receiving_) {
            auto line = serial_->readLine(50);
            if (line.empty()) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            const auto kind = line.substr(0, line.find(' '));
            if (kind == "EVT" || kind == "DATA" || kind == "DATA_END") {
                events_.push_back(line);
                eventReady_.notify_one();
            } else {
                responses_.push_back(line);
                responseReady_.notify_one();
            }
        }
    } catch (...) {
        receiving_ = false;
        responseReady_.notify_all();
        eventReady_.notify_all();
    }
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
    std::string token;
    while (iss >> token) {
        auto pos = token.find('=');
        if (pos != std::string::npos) info[token.substr(0, pos)] = token.substr(pos + 1);
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
    if (!resp.success) return Level::LOW;
    if (resp.payload == channelName(ch) + " HIGH") return Level::HIGH;
    if (resp.payload == channelName(ch) + " LOW") return Level::LOW;
    throw std::runtime_error("Malformed IO response");
}

std::vector<Level> IODock::ioReadAll() {
    std::vector<Level> levels(6, Level::LOW);
    auto resp = sendCommand("IO READALL");
    if (!resp.success) return levels;
    bool seen[6] = {};
    std::istringstream fields(resp.payload);
    std::string channel, level;
    while (fields >> channel) {
        if (!(fields >> level) || channel.size() != 3 || channel.substr(0, 2) != "IO" ||
            channel[2] < '1' || channel[2] > '6' || (level != "HIGH" && level != "LOW"))
            throw std::runtime_error("Malformed IO READALL response");
        auto index = channel[2] - '1';
        if (seen[index]) throw std::runtime_error("Duplicate IO channel");
        seen[index] = true;
        levels[index] = level == "HIGH" ? Level::HIGH : Level::LOW;
    }
    for (bool present : seen) if (!present) throw std::runtime_error("Missing IO channel");
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
                      std::to_string(freqHz) + " " + (isPercent ? "" : "#") + std::to_string(duty);
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
        std::smatch match;
        if (!std::regex_match(resp.payload, match,
            std::regex(R"(PWM[1-4] ([0-9]+)Hz ([0-9]+)% (running|stopped) pol=(normal|invert))")))
            throw std::runtime_error("Malformed PWM response");
        auto frequency = std::stoull(match[1]);
        auto percent = std::stoul(match[2]);
        if (frequency > UINT32_MAX || percent > 100) throw std::runtime_error("PWM response out of range");
        config.frequency = static_cast<uint32_t>(frequency);
        config.duty = static_cast<uint16_t>((percent * 65535u + 50u) / 100u);
        config.running = match[3] == "running";
        config.inverted = match[4] == "invert";
    }

    return config;
}

void IODock::pwmSync(const std::vector<PWMChannel>& channels) {
    std::string cmd = "PWM SYNC ";
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
    cmd += " " + bytesToHex(data);
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
    cmd += " " + bytesToHex(data);
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

namespace {
ADCReading parseADC(const std::string& payload, const std::string& channel) {
    std::istringstream input(payload);
    std::string actual, raw, mv, extra;
    if (!(input >> actual >> raw >> mv) || input >> extra || actual != channel ||
        !std::regex_match(raw, std::regex("[0-9]+")) ||
        !std::regex_match(mv, std::regex("[0-9]+mV")))
        throw std::runtime_error("Malformed ADC response");
    auto r = std::stoul(raw), v = std::stoul(mv.substr(0, mv.size() - 2));
    if (r > 4095 || v > 3300) throw std::runtime_error("ADC response out of range");
    return {static_cast<uint16_t>(r), static_cast<uint16_t>(v)};
}
}

ADCReading IODock::adcRead(ADCChannel ch) {
    auto resp = sendCommand("ADC READ " + channelName(ch));
    if (!resp.success) return {0, 0};
    return parseADC(resp.payload, channelName(ch));
}

std::vector<ADCReading> IODock::adcReadAll() {
    auto resp = sendCommand("ADC READALL");
    std::vector<ADCReading> readings(3, {0, 0});
    if (!resp.success) return readings;
    bool seen[3] = {};
    std::istringstream input(resp.payload);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string channel;
        fields >> channel;
        if (channel.size() != 4 || channel.substr(0, 3) != "ADC" || channel[3] < '0' || channel[3] > '2')
            throw std::runtime_error("Invalid ADC channel");
        auto index = channel[3] - '0';
        if (seen[index]) throw std::runtime_error("Duplicate ADC channel");
        readings[index] = parseADC(line, channel);
        seen[index] = true;
    }
    if (!seen[0] || !seen[1] || !seen[2]) throw std::runtime_error("Missing ADC channel");
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
    if (!std::regex_match(tempStr, std::regex(R"(-?[0-9]+(\.[0-9]+)?C)")))
        throw std::runtime_error("Malformed temperature response");
    tempStr.pop_back();
    
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
    std::lock_guard<std::mutex> lock(mutex_);
    eventCallback_ = std::move(callback);
}

void IODock::startEventListener() {
    if (callbackDock == this) return;
    std::lock_guard<std::mutex> lifecycleLock(listenerMutex_);
    if (listening_) return;
    if (eventThread_.joinable()) eventThread_.join();
    listening_ = true;
    eventThread_ = std::thread([this]() {
        callbackDock = this;
        while (listening_) {
            EventCallback callback;
            std::string line;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                eventReady_.wait(lock, [this] { return !listening_ || !events_.empty(); });
                if (!listening_) break;
                line = std::move(events_.front());
                events_.pop_front();
                callback = eventCallback_;
            }
            if (!callback) continue;
            auto start = line.compare(0, 4, "EVT ") == 0 ? 4u : 0u;
            auto end = line.find(' ', start);
            try { callback(line.substr(start, end - start),
                           end == std::string::npos ? "" : line.substr(end + 1)); }
            catch (...) { /* User callbacks must not terminate the receiver. */ }
        }
        callbackDock = nullptr;
    });
}

void IODock::stopEventListener() {
    if (callbackDock == this) {
        listening_ = false; eventReady_.notify_all(); return;
    }
    std::lock_guard<std::mutex> lifecycleLock(listenerMutex_);
    listening_ = false;
    eventReady_.notify_all();
    if (eventThread_.joinable()) eventThread_.join();
}

Response IODock::syncUtc() { return sendCommandPrepared("SYNC 0 UTC", true); }
Response IODock::syncUtc(uint64_t unixUs) { return sendCommand("SYNC " + std::to_string(unixUs) + " UTC"); }
Response IODock::timeStatus() { return sendCommand("TIME"); }
Response IODock::timingConfigure(int pps, int mask, int hz, uint32_t pw, uint32_t tw, bool invert) {
    if (pps < 1 || pps > 4 || mask < 1 || mask > 15 || hz < 1 || hz > 100 || pw < 100 || pw >= 1000000 || tw < 100 || tw >= 1000000u / hz)
        throw std::invalid_argument("Invalid timing configuration");
    return sendCommand("TIMING CFG " + std::to_string(pps) + " " + std::to_string(mask) + " " +
        std::to_string(hz) + " " + std::to_string(pw) + " " + std::to_string(tw) + " " + (invert ? "1" : "0"));
}
Response IODock::timingNmea(int uart, uint32_t baud, uint32_t delayUs) {
    if (uart < 0 || uart > 2 || baud < 1200 || baud > 7800000 || delayUs >= 900000) throw std::invalid_argument("Invalid NMEA configuration");
    return sendCommand("TIMING NMEA " + std::to_string(uart) + " " + std::to_string(baud) + " " + std::to_string(delayUs));
}
Response IODock::timingStart() { return sendCommand("TIMING START"); }
Response IODock::timingStop() { return sendCommand("TIMING STOP"); }
Response IODock::timingStatus() { return sendCommand("TIMING STAT"); }
Response IODock::bootSequence(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    if (name.empty() || name.size() > 15 || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos || upper == "OFF" || upper == "STAT")
        throw std::invalid_argument("Invalid boot sequence name");
    return sendCommand("BOOT SEQ " + name);
}
Response IODock::bootSequenceOff() { return sendCommand("BOOT OFF"); }
Response IODock::bootSequenceStatus() { return sendCommand("BOOT STAT"); }

Response IODock::sendCommand(const std::string& cmd) {
    return sendCommandPrepared(cmd, false);
}
Response IODock::sendCommandPrepared(std::string cmd, bool utcNow) {
    std::lock_guard<std::mutex> commandLock(commandMutex_);
    Response resp{false, cmd, "", "", -1};
    std::unique_lock<std::mutex> lock(mutex_);
    if (!receiving_ || !serial_) { resp.error = "Serial port is closed"; return resp; }
    if (!synchronized_) { resp.error = "Response synchronization lost; close and reopen"; return resp; }
    if (cmd.empty() || cmd.find_first_of("\r\n") != std::string::npos) {
        resp.error = "Expected one command line"; return resp;
    }
    std::istringstream words(cmd);
    std::string name, sub;
    words >> name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
    if (name == "IO" || name == "PWM" || name == "UART" || name == "I2C" ||
        name == "SPI" || name == "ADC" || name == "SEQ" || name == "BIN" || name == "CFG" || name == "TIMING" || name == "BOOT") {
        words >> sub;
        std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char c) { return std::toupper(c); });
        if (!sub.empty()) name += " " + sub;
    }
    const bool multiline = name == "INFO" || name == "IO READALL" || name == "ADC READALL" || name == "I2C SCAN" || name == "HELP";
    if (!responses_.empty()) {
        synchronized_ = false; resp.error = "Unsolicited response; close and reopen"; return resp;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    if (utcNow) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        cmd = "SYNC " + std::to_string(us) + " UTC";
        resp.command = cmd;
    }
    const auto wire = cmd + "\n";
    size_t sent = 0;
    while (sent < wire.size()) {
        int n = serial_->write(wire.substr(sent));
        if (n <= 0 || std::chrono::steady_clock::now() >= deadline) {
            synchronized_ = false; resp.error = "Serial write failed"; return resp;
        }
        sent += static_cast<size_t>(n);
    }
    bool header = false;
    while (receiving_) {
        if (!responseReady_.wait_until(lock, deadline, [this] { return !responses_.empty() || !receiving_; })) break;
        if (!receiving_) break;
        auto line = std::move(responses_.front()); responses_.pop_front();
        if (!header) {
            const auto ok = "OK " + name;
            if (line == ok || line.compare(0, ok.size() + 1, ok + " ") == 0) {
                resp.payload = line.size() > ok.size() ? line.substr(ok.size() + 1) : "";
                if (!multiline) { resp.success = true; resp.errorCode = 0; return resp; }
                header = true;
            } else if (line.compare(0, 4, "ERR ") == 0) {
                resp = parseResponse(line);
                if (resp.command != name && resp.command != "LINE" && !(resp.errorCode == 0 && resp.command == name.substr(0, name.find(' ')))) {
                    synchronized_ = false; resp.error = "Mismatched error response: " + line;
                }
                resp.command = cmd; return resp;
            } else {
                synchronized_ = false; resp.error = "Unexpected response: " + line; return resp;
            }
        } else if (line == "END") {
            resp.success = true; resp.errorCode = 0; return resp;
        } else {
            if (line.compare(0, 4, "ERR ") == 0) {
                resp = parseResponse(line); resp.command = cmd; synchronized_ = false; return resp;
            }
            if (!resp.payload.empty()) resp.payload += "\n";
            resp.payload += line;
        }
    }
    synchronized_ = false;
    resp.error = receiving_ ? "Response timeout; close and reopen" : "Serial connection closed";
    return resp;
}

Response IODock::parseResponse(const std::string& line) {
    Response resp{false, "", "", line, -1};
    if (line.compare(0, 4, "ERR ") != 0) return resp;
    std::istringstream iss(line.substr(4));
    std::string token;
    while (iss >> token) {
        if (token.compare(0, 2, "E_") == 0) {
            static const std::map<std::string, int> codes = {
                {"E_BADCMD",0},{"E_PARAM",1},{"E_NOTFOUND",2},{"E_RANGE",3},
                {"E_CFG",4},{"E_BUSY",5},{"E_TIMEOUT",6},{"E_NACK",7},
                {"E_OVERRUN",8},{"E_IO",9},{"E_LONG",10},{"E_DENIED",11}};
            auto found = codes.find(token);
            if (found != codes.end()) resp.errorCode = found->second;
            std::string message;
            std::getline(iss, message);
            resp.error = token + message;
            break;
        }
        if (!resp.command.empty()) resp.command += " ";
        resp.command += token;
    }
    return resp;
}

std::vector<uint8_t> IODock::hexToBytes(const std::string& hex) {
    std::string digits;
    for (unsigned char c : hex) {
        if (std::isspace(c)) continue;
        if (!std::isxdigit(c)) throw std::runtime_error("Malformed HEX response");
        digits += static_cast<char>(c);
    }
    if (digits.size() % 2) throw std::runtime_error("Odd HEX response length");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < digits.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoul(digits.substr(i, 2), nullptr, 16)));
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
