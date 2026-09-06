"""Python SDK concurrent commands + ADC + PWM event integration, USB-only."""
import concurrent.futures
import json
import sys
import threading
import time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'sdk/python'))
from iodock import IODock

d=IODock(sys.argv[1],timeout=3)
assert d.open()
samples=[]; events=[]; done=threading.Event()


def data(name,payload):
    if name=='ADC0': samples.extend(map(int,payload.split(',')))
    elif name=='DATA_END': done.set()


def command(c):
    r=d.send_command(c)
    assert r.success,(c,r)
    return r


def worker(number):
    for _ in range(50):
        assert d.ping()
    return 50


try:
    assert d.get_info()['fw']=='0.1.4'
    d.on_event(lambda name,payload: events.append((name,payload)))
    d.on_data(data)
    d.start_event_listener()
    command('PWM TICK PWM4 10')
    command('ADC SAMPLE ADC0 10000 500')
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        count=sum(pool.map(worker,range(4)))
    assert done.wait(3),'No DATA_END callback'
    assert len(samples)==5000,len(samples)
    assert any(n=='PWM_TICK' for n,_ in events),events
    assert all(0<=v<=4095 for v in samples)
    command('IO CFG IO1 OUT')
    command('IO WRITE IO1 HIGH')
    assert d.io_read('IO1').value==1
    assert len(d.io_read_all())==6
    assert len(d.adc_read_all())==3
    assert len(d.get_info())>=6
    error=d.send_command('IO READ IO10')
    assert not error.success and error.error_code==1,error
    result=dict(passed=True,concurrent_pings=count,adc_samples=len(samples),pwm_events=len(events),multiline_and_error_parsing=True)
    Path(__file__).with_name('sdk_hardware_results.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(result)
finally:
    command('PWM TICK OFF')
    command('PWM STOP PWM4')
    command('IO CFG IO1 IN FLOAT')
    d.close()
