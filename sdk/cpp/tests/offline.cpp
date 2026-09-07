#define IODOCK_TEST_TRANSPORT
#include "../iodock.cpp"
#include <future>

using namespace iodock;
void check(bool value) { if (!value) throw std::runtime_error("Assertion failed"); }
void reply(const std::string& command, const std::string& response) { mock::expect(command, {{0, response}}); }
void waitFor(const std::function<bool()>& condition) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("Test wait timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() {
    int count = 0;
    auto test = [&](const char* name, const std::function<void(IODock&)>& body) {
        mock::reset();
        IODock dock("offline-memory-only");
        check(dock.open());
        body(dock);
        dock.close();
        mock::verify();
        std::cout << "PASS " << name << '\n'; ++count;
    };
    try {
        test("timing UTC boot contract", [](IODock& d) {
            reply("SYNC 1700000000000123 UTC", "OK SYNC offset=12\n"); check(d.syncUtc(1700000000000123ULL).success);
            reply("TIME", "OK TIME source=HOST unix_us=1700000000000123\n"); check(d.timeStatus().payload.find("source=HOST") != std::string::npos);
            reply("TIMING CFG 1 14 10 10000 1000 0", "OK TIMING CFG\n"); check(d.timingConfigure(1,14,10,10000,1000).success);
            reply("TIMING NMEA 2 9600 899999", "OK TIMING NMEA\n"); check(d.timingNmea(2,9600,899999).success);
            reply("TIMING START", "ERR TIMING START E_BUSY resource owned\n"); check(d.timingStart().errorCode == 5);
            reply("TIMING START", "ERR TIMING E_BADCMD unknown command\n"); check(d.timingStart().errorCode == 0);
            reply("TIMING STOP", "OK TIMING STOP\n"); check(d.timingStop().success);
            reply("TIMING STAT", "OK TIMING STAT active=0 error=0\n"); check(d.timingStatus().payload == "active=0 error=0");
            reply("BOOT SEQ timing_boot", "OK BOOT SEQ\n"); check(d.bootSequence("timing_boot").success);
            reply("BOOT SEQ OFF", "OK BOOT SEQ\n"); check(d.bootSequenceOff().success);
            reply("BOOT SEQ STAT", "OK BOOT SEQ name=OFF error=0\n"); check(d.bootSequenceStatus().payload == "name=OFF error=0");
        });
        test("timing validation", [](IODock& d) {
            auto invalid = [](const std::function<void()>& f) { bool threw=false; try { f(); } catch(const std::invalid_argument&) { threw=true; } check(threw); };
            invalid([&]{d.timingConfigure(1,14,0,100,100);});
            invalid([&]{d.timingConfigure(1,14,100,100,10000);});
            invalid([&]{d.timingConfigure(1,14,10,99,100);});
            invalid([&]{d.timingConfigure(1,14,10,1000000,100);});
            invalid([&]{d.timingNmea(1,1199,0);});
            invalid([&]{d.timingNmea(1,9600,900000);});
            invalid([&]{d.bootSequence("OFF");});
            invalid([&]{d.bootSequence("x\nSAVE");});
        });
        test("fragmentation and short writes", [](IODock& d) {
            mock::expect("PING", {{0,"OK PI"},{130,"NG PONG 0.1.2\r"},{70,"\n"}});
            check(d.ping());
        });
        test("event and DATA interleaving plus callback reentry", [](IODock& d) {
            std::atomic<int> events{0}; std::atomic<bool> nested{false};
            d.onEvent([&](const std::string& e, const std::string& data) {
                if (e == "GPIO") { check(data == "IO1 HIGH"); nested = d.ping(); }
                ++events;
            });
            reply("ADC READ ADC0", "EVT GPIO IO1 HIGH\nDATA ADC0 1 2\nOK ADC READ ADC0 2048 1650mV\n");
            reply("PING", "OK PING PONG\n");
            d.startEventListener();
            auto r = d.adcRead(ADCChannel::ADC0); check(r.raw == 2048 && r.millivolts == 1650);
            waitFor([&] { return events == 2; }); check(nested);
            d.stopEventListener();
        });
        test("DATA_END interleaved with single and multiline responses", [](IODock& d) {
            std::atomic<int> ends{0}; std::atomic<bool> dataOK{true};
            d.onEvent([&](const std::string& event, const std::string& data) {
                if (event == "DATA_END") { if (data != "ADC0") dataOK = false; ++ends; }
            });
            d.startEventListener();
            reply("PING", "DATA_END ADC0\nOK PING PONG\n"); check(d.ping());
            reply("ADC READALL", "OK ADC READALL\n ADC0 0 0mV\nDATA_END ADC0\n ADC1 1 1mV\n ADC2 2 2mV\nEND\nDATA_END ADC0\n");
            check(d.adcReadAll()[2].raw == 2);
            reply("PING", "OK PING PONG\n"); check(d.ping());
            waitFor([&] { return ends == 3; }); check(dataOK); d.stopEventListener();
        });
        test("INFO END and packed key values", [](IODock& d) {
            mock::expect("INFO", {{0,"OK INFO\n board=IO-Dock\n fw=0.1.2\n"},{120," io=6 pwm=4 uart=2 adc=3\nEND\n"}});
            auto info = d.getInfo(); check(info.at("board") == "IO-Dock" && info.at("pwm") == "4" && info.at("adc") == "3");
            reply("PING", "OK PING PONG\n"); check(d.ping());
        });
        test("ADC READALL channel mapping", [](IODock& d) {
            reply("ADC READALL", "OK ADC READALL\n ADC2 4095 3300mV\nEVT PWM_TICK 1\n ADC0 0 0mV\n ADC1 2048 1650mV\nEND\n");
            auto a = d.adcReadAll(); check(a[0].raw == 0 && a[1].millivolts == 1650 && a[2].raw == 4095);
            reply("PING", "OK PING PONG\n"); check(d.ping());
        });
        test("IO READALL END", [](IODock& d) {
            reply("IO READALL", "OK IO READALL\n IO1 HIGH IO2 LOW IO3 HIGH IO4 LOW IO5 LOW IO6 HIGH\nEND\n");
            auto a = d.ioReadAll(); check(a[0] == Level::HIGH && a[1] == Level::LOW && a[5] == Level::HIGH);
        });
        test("I2C SCAN multiline and empty scan", [](IODock& d) {
            reply("I2C SCAN", "OK I2C SCAN\n 0x3C 0x68\nEND\n");
            check(d.i2cScan() == std::vector<uint8_t>({0x3c,0x68}));
            reply("I2C SCAN", "OK I2C SCAN\n\nEND\n"); check(d.i2cScan().empty());
        });
        test("HELP and CFG multiword command", [](IODock& d) {
            reply("HELP", "OK HELP\n INFO PING\nEND\n"); check(d.sendCommand("HELP").success);
            reply("CFG CLEAR", "OK CFG CLEAR\n"); check(d.sendCommand("CFG CLEAR").payload.empty());
        });
        test("malformed IO is rejected", [](IODock& d) {
            reply("IO READ IO1", "OK IO READ IO1 NOT_HIGH\n");
            bool threw = false; try { d.ioRead(IOChannel::IO1); } catch (const std::runtime_error&) { threw = true; } check(threw);
        });
        test("serial read failure wakes command", [](IODock& d) {
            reply("PING", "EVT TICK 1\n");
            auto f = std::async(std::launch::async,[&] {return d.sendCommand("PING");});
            waitFor([] {std::lock_guard<std::mutex> lock(mock::guard); return mock::steps.empty();});
            {std::lock_guard<std::mutex> lock(mock::guard); mock::disconnected = true;}
            check(!f.get().success && !d.isOpen());
        });
        test("multiword errors and initialized unknown error", [](IODock& d) {
            reply("I2C READ 0x68 0x00 1", "ERR I2C READ E_NACK no ack\n");
            auto r = d.sendCommand("I2C READ 0x68 0x00 1");
            check(!r.success && r.errorCode == 7 && r.command == "I2C READ 0x68 0x00 1");
            reply("PING", "ERR PING E_FUTURE future\n"); check(d.sendCommand("PING").errorCode == -1);
        });
        test("empty acknowledgement and original command", [](IODock& d) {
            reply("LED ON", "OK LED\n"); auto r = d.sendCommand("LED ON");
            check(r.success && r.payload.empty() && r.command == "LED ON" && r.errorCode == 0);
        });
        test("PWM and negative temperature parsing", [](IODock& d) {
            reply("PWM READ PWM1", "OK PWM READ PWM1 1000Hz 50% stopped pol=invert\n");
            auto c = d.pwmRead(PWMChannel::PWM1); check(c.frequency == 1000 && c.duty == 32768 && !c.running && c.inverted);
            reply("ADC TEMP", "OK ADC TEMP -2.125C\n"); check(d.adcReadTemp() == -2.125f);
            reply("PWM SYNC PWM1,PWM2", "OK PWM SYNC\n"); d.pwmSync({PWMChannel::PWM1,PWMChannel::PWM2});
        });
        test("I2C packed data and explicit raw PWM duty", [](IODock& d) {
            reply("I2C WRITE 0x68 0x10 00AF", "OK I2C WRITE\n"); d.i2cWriteReg(0x68,0x10,{0,175});
            reply("I2C WRONLY 0x68 0102", "OK I2C WRONLY\n"); d.i2cWrite(0x68,{1,2});
            reply("PWM CFG PWM1 1000 #50", "OK PWM CFG PWM1 1000Hz 0%\n"); d.pwmConfig(PWMChannel::PWM1,1000,50,false);
        });
        test("HEX strict decoding", [](IODock& d) {
            reply("UART RX USART1", "OK UART RX USART1 HEX:00 aF 10\n"); check(d.uartReceive(UARTChannel::USART1) == std::vector<uint8_t>({0,175,16}));
            for (auto bad : {"0G", "123"}) {
                reply("SPI READ 1", std::string("OK SPI READ HEX:") + bad + "\n");
                bool threw = false; try { d.spiRead(1); } catch (const std::runtime_error&) { threw = true; } check(threw);
            }
        });
        test("malformed ADC rejected", [](IODock& d) {
            for (auto payload : {"ADC0", "ADC0 5000 1mV", "ADC1 1 1mV", "ADC0 1x 1mV"}) {
                reply("ADC READ ADC0", std::string("OK ADC READ ") + payload + "\n");
                bool threw = false; try { d.adcRead(ADCChannel::ADC0); } catch (const std::exception&) { threw = true; } check(threw);
            }
        });
        test("events retained before listener starts and after stop", [](IODock& d) {
            std::atomic<int> n{0}; d.onEvent([&](const std::string&,const std::string&) { ++n; });
            mock::inject("EVT FIRST\n");
            reply("PING", "EVT SECOND\nOK PING PONG\n"); check(d.ping());
            d.startEventListener(); waitFor([&] { return n == 2; }); d.stopEventListener();
            mock::inject("EVT THIRD\n"); d.startEventListener(); waitFor([&] { return n == 3; }); d.stopEventListener();
        });
        test("concurrent commands serialized", [](IODock& d) {
            for (int i=0;i<40;++i) reply("PING", "EVT TICK 1\nOK PING PONG\n");
            std::vector<std::future<bool>> work;
            d.startEventListener();
            for (int i=0;i<4;++i) work.push_back(std::async(std::launch::async,[&] { for(int j=0;j<10;++j) if(!d.ping()) return false; return true; }));
            for (auto& f:work) check(f.get());
        });
        test("timeout prevents late response reuse and reopen recovers", [](IODock& d) {
            reply("PING", "EVT TICK 1\n"); check(!d.sendCommand("PING").success);
            mock::inject("OK PING PONG\n"); check(!d.sendCommand("PING").success);
            d.close(); mock::reset(); check(d.open()); reply("PING", "OK PING PONG\n"); check(d.ping());
        });
        test("missing END fails", [](IODock& d) {
            reply("INFO", "OK INFO\n board=IO-Dock\n"); check(!d.sendCommand("INFO").success);
        });
        test("unrecognized response is not success", [](IODock& d) {
            reply("PING", "garbage\n"); check(!d.sendCommand("PING").success);
        });
        test("callback stop and exception isolation", [](IODock& d) {
            std::atomic<int> n{0}; d.onEvent([&](const std::string&,const std::string&) {
                if (++n == 1) throw std::runtime_error("user error"); d.stopEventListener();
            });
            d.startEventListener(); mock::inject("EVT FIRST\nEVT SECOND\n"); waitFor([&] {return n == 2;}); d.stopEventListener();
        });
        test("restart after callback stops itself", [](IODock& d) {
            std::atomic<bool> stopped{false}; std::atomic<int> n{0};
            d.onEvent([&](const std::string&,const std::string&) {
                ++n; d.stopEventListener(); stopped = true;
            });
            d.startEventListener(); mock::inject("EVT FIRST\n"); waitFor([&] { return stopped.load(); });
            d.onEvent([&](const std::string&,const std::string&) { ++n; });
            d.startEventListener(); mock::inject("EVT SECOND\n"); waitFor([&] { return n == 2; });
            d.stopEventListener();
        });
        test("close cancels blocked response", [](IODock& d) {
            reply("PING", "EVT TICK 1\n"); auto f = std::async(std::launch::async,[&] { return d.sendCommand("PING"); });
            waitFor([] { std::lock_guard<std::mutex> lock(mock::guard); return mock::steps.empty(); });
            d.close(); check(!f.get().success);
        });
        test("line injection rejected before write", [](IODock& d) { check(!d.sendCommand("PING\nRESET").success); });
        std::cout << count << " offline tests passed\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
