#include "iodock.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace iodock;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    IODock dock(argv[1]);
    if (!dock.open()) return 3;
    std::atomic<int> samples{0}, ticks{0}, errors{0};
    std::atomic<bool> done{false};
    auto cmd = [&](const char *s) {
        auto r = dock.sendCommand(s);
        if (!r.success) throw std::runtime_error(std::string(s)+": "+r.error);
    };
    try {
        if (dock.getInfo()["fw"] != "0.1.4") throw std::runtime_error("version");
        dock.onEvent([&](const std::string& name, const std::string& data) {
            if (name == "PWM_TICK") ++ticks;
            if (name == "DATA_END") done = true;
            if (name == "DATA" && data.rfind("ADC0 ",0)==0)
                samples += 1 + std::count(data.begin()+5,data.end(),',');
        });
        dock.startEventListener();
        cmd("PWM TICK PWM4 10");
        cmd("ADC SAMPLE ADC0 10000 500");
        std::vector<std::thread> workers;
        for (int i=0;i<4;i++) workers.emplace_back([&]{
            for(int j=0;j<50;j++) if(!dock.ping()) ++errors;
        });
        for (auto &w: workers) w.join();
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!done && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!done || samples!=5000 || !ticks || errors) throw std::runtime_error("concurrent stream");
        if(dock.ioReadAll().size()!=6 || dock.adcReadAll().size()!=3) throw std::runtime_error("multiline");
        auto bad=dock.sendCommand("IO READ IO10");
        if(bad.success || bad.errorCode!=1) throw std::runtime_error("error parse");
        cmd("PWM TICK OFF"); cmd("PWM STOP PWM4");
        dock.close();
        std::cout<<"PASS C++ hardware: 200 concurrent PING, "<<samples<<" ADC samples, "<<ticks<<" PWM events, multiline/error parsing\n";
    } catch(const std::exception& e) {
        std::cerr<<"FAIL "<<e.what()<<" samples="<<samples<<" ticks="<<ticks<<" errors="<<errors<<"\n";
        dock.close(); return 1;
    }
}
