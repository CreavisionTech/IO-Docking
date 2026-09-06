"""USB-only standard board regressions. Mutates outputs and saved configuration.
Run only on a board with no attached peripherals: python ... COM43
"""
import json
import struct
import sys
import time
from pathlib import Path
import serial

sys.stdout.reconfigure(encoding='utf-8')
PORT = sys.argv[1]
results = []


class Dock:
    def __init__(self):
        self.s = serial.Serial(PORT, 115200, timeout=.01)
        self.events = []

    def line(self):
        return self.s.readline().decode('utf-8', errors='replace').strip()

    def cmd(self, command, error=None, multi=False, timeout=3):
        self.s.write((command + '\n').encode())
        end = time.monotonic() + timeout
        lines = []
        while time.monotonic() < end:
            ln = self.line()
            if not ln:
                continue
            if ln.startswith(('EVT ', 'DATA ', 'DATA_END ')):
                self.events.append(ln)
                continue
            lines.append(ln)
            if ln.startswith('ERR '):
                assert error and error in ln, (command, lines)
                return lines
            if ln.startswith('OK ') and not multi or ln == 'END':
                assert error is None, (command, lines)
                return lines
        raise AssertionError((command, 'timeout', lines))

    def wait_event(self, prefix, timeout=3):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for i, ev in enumerate(self.events):
                if ev.startswith(prefix):
                    return self.events.pop(i)
            ln = self.line()
            if ln:
                self.events.append(ln)
        raise AssertionError(('missing event', prefix, self.events[-5:]))


d = Dock()


def run(name, fn):
    start = time.monotonic()
    try:
        detail = fn()
        results.append(dict(name=name, passed=True, seconds=round(time.monotonic()-start, 3), detail=detail))
        print('PASS', name, detail or '', flush=True)
    except Exception as e:
        results.append(dict(name=name, passed=False, error=repr(e)))
        print('FAIL', name, repr(e), flush=True)


def info():
    r = d.cmd('INFO', multi=True)
    assert 'fw=0.1.4' in r and any('io=6 pwm=4 uart=2 i2c=1 spi=1 adc=3' in x for x in r), r
    return r


def malformed():
    for command in ['IO WRITE IO10 HIGH', 'IO CFG IO1junk OUT', 'PWM CFG PWM10 1000 50',
                    'PWM CFG PWM1 banana 50', 'PWM DUTY PWM1 50junk', 'ADC READ ADC00',
                    'UART CFG USART10 115200 8 N 1', 'IO PULSE IO1 HIGH 0']:
        d.cmd(command, error='E_')
    d.cmd('IO CFG IO1 OUT')
    d.cmd('IO WRITE IO1 LOW')
    d.cmd('IO WRITE IO1 HIGH' + ' ' * 600, error='E_LONG')
    assert 'LOW' in d.cmd('IO READ IO1')[0]
    d.cmd('PING')


def io_pulse():
    for i in range(1, 7):
        d.cmd(f'IO CFG IO{i} OUT')
        d.cmd(f'IO WRITE IO{i} HIGH')
        assert 'HIGH' in d.cmd(f'IO READ IO{i}')[0]
        d.cmd(f'IO TOGGLE IO{i}')
        assert 'LOW' in d.cmd(f'IO READ IO{i}')[0]
        d.cmd(f'IO PULSE IO{i} HIGH 40')
        time.sleep(.06)
        assert 'LOW' in d.cmd(f'IO READ IO{i}')[0]
    d.cmd('IO PULSE IO1 HIGH 100')
    d.cmd('IO TOGGLE IO1')
    d.cmd('IO WRITE IO1 HIGH')
    time.sleep(.12)
    assert 'HIGH' in d.cmd('IO READ IO1')[0]


def gpio_event():
    d.events.clear()
    d.cmd('IO EVENT IO1 ON CHANGE')
    d.cmd('IO WRITE IO1 LOW')
    d.wait_event('EVT GPIO IO1 LOW')
    d.cmd('IO EVENT IO1 OFF')
    d.events.clear()
    time.sleep(.06)
    d.cmd('IO WRITE IO1 HIGH')
    time.sleep(.06)
    d.cmd('PING')
    assert not any(x.startswith('EVT GPIO IO1') for x in d.events), d.events


def seq_timing():
    d.events.clear()
    d.cmd('SEQ DEF timed "IO IO1 LOW; WAIT 100; IO IO1 HIGH; WAIT 100"')
    start = time.monotonic()
    d.cmd('SEQ RUN timed 2')
    d.wait_event('EVT SEQ_DONE timed')
    elapsed = time.monotonic() - start
    assert .37 < elapsed < .8, elapsed
    return {'elapsed': elapsed, 'expected': .4}


def seq_repeat():
    d.events.clear()
    d.cmd('SEQ DEF repeat "IO IO1 LOW; WAIT 1"')
    start = time.monotonic()
    d.cmd('SEQ RUN repeat 256')
    d.wait_event('EVT SEQ_DONE repeat', 2)
    elapsed = time.monotonic() - start
    assert .24 < elapsed < 1, elapsed
    return elapsed


def seq_invalid():
    d.cmd('SEQ DEF good "IO IO1 LOW; WAIT 5"')
    d.cmd('SEQ DEF bad "IO IO1 HIGH; GARBAGE"', error='E_')
    assert 'IO IO1 LOW' in d.cmd('SEQ SHOW good')[0]
    d.events.clear()
    d.cmd('SEQ RUN good 1')
    d.wait_event('EVT SEQ_DONE good')


def seq_busy():
    d.cmd('SEQ DEF busy "IO IO1 LOW; WAIT 100"')
    d.cmd('SEQ RUN busy 0')
    d.cmd('SEQ RUN busy 1', error='E_BUSY')
    d.cmd('SEQ DEF no "IO IO1 HIGH"', error='E_BUSY')
    for c in ['SAVE', 'LOAD', 'CFG CLEAR']:
        d.cmd(c, error='E_BUSY')
    d.cmd('SEQ STOP')
    d.wait_event('EVT SEQ_DONE busy')


def adc_stream(rate, ms=300, conflicts=False):
    d.events.clear()
    start = time.monotonic()
    d.cmd(f'ADC SAMPLE ADC0 {rate} {ms}')
    if conflicts:
        for command in ['ADC READ ADC1', 'ADC READALL', 'ADC TEMP', 'ADC SAMPLE ADC1 1000 10', 'SAVE']:
            d.cmd(command, error='E_BUSY')
    d.wait_event('DATA_END ADC0', 4)
    elapsed = time.monotonic() - start
    samples = sum(len(x.split(' ', 2)[2].split(',')) for x in d.events if x.startswith('DATA ADC0 '))
    overruns = [x for x in d.events if 'OVERRUN' in x]
    assert samples == rate * ms // 1000 and not overruns, (samples, overruns)
    assert ms / 1000 * .88 <= elapsed < ms / 1000 + .35, elapsed
    return {'rate': rate, 'samples': samples, 'elapsed': elapsed}


def seq_adc():
    d.events.clear()
    d.cmd('SEQ DEF sample "SAMPLE ADC0 1000 100; WAIT 120"')
    d.cmd('SEQ RUN sample 1')
    d.wait_event('DATA_END ADC0')
    d.wait_event('EVT SEQ_DONE sample')
    assert sum(len(x.split(' ',2)[2].split(',')) for x in d.events if x.startswith('DATA ADC0 ')) == 100


def pwm():
    d.cmd('PWM CFG PWM2 1000 25')
    d.cmd('PWM CFG PWM3 2000 50')
    assert '2000Hz' in d.cmd('PWM READ PWM2')[0]
    for i in range(1,5):
        d.cmd(f'PWM CFG PWM{i} 10000 100')
        d.cmd(f'PWM START PWM{i}')
        d.cmd(f'PWM PHADJ PWM{i} 1', error='E_DENIED')
        d.cmd(f'PWM STOP PWM{i}')
    d.cmd('PWM CFG PWM1 1000 50')
    d.cmd('PWM START PWM1')
    d.cmd('PWM PHADJ PWM1 1')
    d.cmd('PWM PHADJ PWM1 -1')


def tick():
    d.events.clear()
    d.cmd('PWM TICK PWM4 1')
    start = time.monotonic()
    d.wait_event('EVT PWM_TICK', 2)
    first = time.monotonic()
    d.wait_event('EVT PWM_TICK', 2)
    elapsed = time.monotonic() - first
    d.cmd('PWM TICK OFF')
    assert .85 < elapsed < 1.15, (first-start, elapsed)
    return {'event_period': elapsed}


def buses():
    for ch in [1,2]:
        for parity in ['N','O','E']:
            d.cmd(f'UART CFG USART{ch} 115200 8 {parity} 1')
            d.cmd(f'UART TX USART{ch} HEX:0055AAFF')
        d.cmd(f'UART CFG USART{ch} 115200 8 N 1')
    for speed in [100,400,1000]:
        d.cmd(f'I2C CFG {speed}')
        d.cmd('I2C READ 0x3C 0 1', error='E_NACK')
    for mode in range(4):
        for order in ['MSB','LSB']:
            for bits in [4,8,9,16]:
                d.cmd(f'SPI CFG 1000000 {mode} {order} {bits}')
                d.cmd('SPI XFR HEX:55AA')
                if bits > 8:
                    for c in ['SPI XFR HEX:55', 'SPI WRITE HEX:55','SPI READ 1']:
                        d.cmd(c,error='E_PARAM')
    d.cmd('SPI CFG 1000000 0 MSB 8')
    return 'Configuration and empty-bus responses only; no external loopback'


def persistence():
    d.cmd('PWM TICK OFF')
    for i in range(1,5):
        d.cmd(f'PWM STOP PWM{i}')
    d.cmd('SEQ DEF saved "IO IO1 LOW; WAIT 10"')
    d.cmd('IO CFG IO1 OUT')
    d.cmd('IO WRITE IO1 HIGH')
    for _ in range(3):
        d.cmd('SAVE')
        d.cmd('IO WRITE IO1 LOW')
        d.cmd('LOAD')
        assert 'HIGH' in d.cmd('IO READ IO1')[0]
    d.s.write(b'RESET\n')
    d.s.close()
    time.sleep(2)
    for _ in range(30):
        try:
            d.s = serial.Serial(PORT, 115200, timeout=.01)
            break
        except serial.SerialException:
            time.sleep(.2)
    d.cmd('PING')
    assert 'HIGH' in d.cmd('IO READ IO1')[0]
    assert 'WAIT 10' in d.cmd('SEQ SHOW saved')[0]
    d.cmd('CFG CLEAR')
    d.cmd('LOAD', error='E_NOTFOUND')


try:
    run('standard identity/version', info)
    run('strict malformed/overlong commands', malformed)
    run('all six GPIO / pulse cancellation', io_pulse)
    run('GPIO self edge and OFF suppression', gpio_event)
    run('sequence trailing WAIT duration', seq_timing)
    run('sequence repeat=256', seq_repeat)
    run('failed definition preserves old sequence', seq_invalid)
    run('sequence start/stop/busy and Flash exclusion', seq_busy)
    for rate in [1000,2000,10000]:
        run(f'ADC {rate}Hz exact count/timing', lambda rate=rate: adc_stream(rate))
    run('ADC streaming conflict exclusion', lambda: adc_stream(1000,400,True))
    run('core1 sequence ADC request', seq_adc)
    run('PWM shared frequency / PHADJ div=1 guard', pwm)
    run('PWM 1Hz event timing', tick)
    run('UART/I2C/SPI config and boundary paths', buses)
    run('Flash save/load/erase and reboot restore', persistence)
finally:
    try:
        d.cmd('PWM TICK OFF')
        for i in range(1,5):
            d.cmd(f'PWM STOP PWM{i}')
        for i in range(1,7):
            d.cmd(f'IO EVENT IO{i} OFF')
            d.cmd(f'IO CFG IO{i} IN FLOAT')
    except Exception as e:
        print('Cleanup error', e)
    d.s.close()
    Path(__file__).with_name('hardware_results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2),encoding='utf-8')
print(f'{sum(r["passed"] for r in results)}/{len(results)} passed')
sys.exit(0 if all(r['passed'] for r in results) else 1)
