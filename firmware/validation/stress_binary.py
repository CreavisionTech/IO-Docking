"""CRC, framing, malformed request and ADC throughput tests; USB-only board."""
import binascii
import json
import struct
import sys
import time
from pathlib import Path
import serial

sys.stdout.reconfigure(encoding='utf-8')
s = serial.Serial(sys.argv[1],115200,timeout=.01)
s.write(b'BIN ENTER\n')
assert s.readline().strip() == b'OK BIN'
buf = bytearray()
results = []
seq = 0


def frame(op, p=b'', number=0):
    body = bytes([0xcb,1,op,0]) + struct.pack('<HH',len(p),number) + p
    return body + struct.pack('<H',binascii.crc_hqx(body,65535))


def read(timeout=4):
    end = time.perf_counter()+timeout
    while time.perf_counter()<end:
        if len(buf)>=8:
            assert buf[0]==0xcb, ('lost framing',bytes(buf[:30]))
            length = struct.unpack_from('<H',buf,4)[0]+10
            if len(buf)>=length:
                raw=bytes(buf[:length]); del buf[:length]
                assert binascii.crc_hqx(raw[:-2],65535)==struct.unpack_from('<H',raw,length-2)[0], 'CRC'
                return raw[1],raw[2],struct.unpack_from('<H',raw,6)[0],raw[8:-2]
        buf.extend(s.read(s.in_waiting or 1))
    raise AssertionError(('frame timeout',bytes(buf[:30])))


def cmd(op,p=b'',err=None):
    global seq
    seq+=1
    s.write(frame(op,p,seq))
    while True:
        t,o,n,r=read()
        if t in (2,3):
            assert (o,n)==(op,seq),(o,n,op,seq)
            if err is None: assert t==2,(op,r)
            else: assert t==3 and r[0]==err,(op,r)
            return r


def test(name, fn):
    try:
        result=fn()
        results.append(dict(name=name,passed=True,detail=result))
        print('PASS',name,result or '',flush=True)
    except Exception as e:
        results.append(dict(name=name,passed=False,error=repr(e)))
        print('FAIL',name,repr(e),flush=True)
        raise


def pipeline():
    payload=b''.join(frame(0,number=i) for i in range(1000,1500))
    s.write(payload)
    for i in range(1000,1500):
        t,o,n,r=read()
        assert (t,o,n,r)==(2,0,i,b'O\x00\x01\x04'),(t,o,n,r)
    return '500 commands, matched sequence IDs and CRCs'


def invalid():
    # Each fixed-size request rejects a short and oversized payload before dispatch.
    sizes={0x10:3,0x11:2,0x12:1,0x14:1,0x15:4,0x16:2,
           0x20:7,0x21:5,0x22:3,0x23:1,0x24:1,0x25:9,0x26:1,
           0x30:8,0x32:3,0x33:1,0x34:2,0x35:1,0x40:4,
           0x50:7,0x53:2,0x54:1,0x60:1,0x62:9,0x63:1,0x65:6}
    for op,size in sizes.items():
        cmd(op,b'\0'*(size-1),err=1)
        cmd(op,b'\0'*(size+1),err=1)
    cmd(0xff,err=0)
    cmd(0x10,bytes([255,0,0]),err=3)
    cmd(0x10,bytes([0,3,0]),err=3)
    bad=bytearray(frame(0,number=65500)); bad[-1]^=0xff
    s.write(bad)
    typ,op,number,p = read()
    assert (typ,op,number)==(3,0,65500), (typ,op,number,p)
    cmd(0)
    s.write(frame(0x51,b'\0'*100)[:12])
    time.sleep(1.1)
    cmd(0)
    return 'fixed payload boundaries, invalid channel, unknown opcode, bad CRC recovery'


def large_spi():
    cmd(0x50,struct.pack('<IBBB',1000000,0,1,8))
    for count in [1,255,512,2048,4094]:
        data=bytes(i&255 for i in range(count))
        reply=cmd(0x51,struct.pack('<H',count)+data)
        assert len(reply)==count+2 and struct.unpack_from('<H',reply)[0]==count
    return 'up to 4096-byte request payload; CRC and response length checked'


def adc(rate, duration=200):
    start=time.perf_counter()
    cmd(0x62,struct.pack('<BII',0,rate,duration))
    samples=0; drops=0; packets=0
    while True:
        typ,op,number,p=read(8)
        if typ==5:
            assert op==0x62
            count=struct.unpack_from('<H',p)[0]
            assert len(p)==2+2*count
            assert all(x[0]<=4095 for x in struct.iter_unpack('<H',p[2:]))
            samples+=count; packets+=1
        elif typ==4:
            if op==0x66: drops+=struct.unpack_from('<I',p,1)[0]
            elif op==0x07: raise AssertionError(('USB TX dropped frames',p))
        elif typ==6:
            assert op==0x62 and struct.unpack('<I',p)[0]==samples
            break
    elapsed=time.perf_counter()-start
    assert samples<=rate*duration//1000
    if rate<=100000: assert drops==0 and samples==rate*duration//1000,(samples,drops)
    if samples<rate*duration//1000: assert drops>0
    assert elapsed<duration/1000+2,elapsed
    cmd(0)
    return dict(rate=rate,requested=rate*duration//1000,received=samples,lost_at_least=drops,seconds=round(elapsed,4),frames=packets)


try:
    test('500 pipelined PING frames',pipeline)
    test('binary malformed payloads and CRC',invalid)
    test('large SPI frames',large_spi)
    for rate in [1000,10000,50000,100000,250000,500000]:
        test(f'ADC binary {rate}Hz',lambda rate=rate: adc(rate))
    test('ADC binary 100kSPS sustained 5 seconds',lambda: adc(100000,5000))
finally:
    try: cmd(0x0f)
    except Exception: pass
    s.close()
    Path(__file__).with_name('binary_stress_results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
