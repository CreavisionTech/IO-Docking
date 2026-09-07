"""Offline regressions: serial.Serial is replaced before every open."""
import concurrent.futures
import queue
import threading
import unittest
from unittest.mock import patch

from iodock import IODock, Level, ADCReading, PWMConfig


class FakeSerial:
    def __init__(self, **kwargs):
        self.is_open = True
        self.timeout = kwargs['timeout']
        self.incoming = queue.Queue()
        self.writes = []
        self.readers = set()
        self.responder = lambda cmd: 'OK ' + IODock._command_name(cmd) + '\n'
        self.resets = 0
        self.written = threading.Event()

    @property
    def in_waiting(self):
        return 0

    def read(self, size):
        self.readers.add(threading.get_ident())
        item = self.incoming.get()
        if isinstance(item, Exception):
            raise item
        return item

    def emit(self, text):
        # Split even command words / UTF-8 sequences across serial reads.
        for b in text.encode():
            self.incoming.put(bytes([b]))

    def write(self, data):
        cmd = data.decode().rstrip('\n')
        self.writes.append(cmd)
        response = self.responder(cmd)
        if response:
            self.emit(response)
        self.written.set()
        return len(data)

    def reset_input_buffer(self):
        self.resets += 1

    def reset_output_buffer(self):
        pass

    def close(self):
        self.is_open = False
        self.incoming.put(OSError('closed'))


class SDKTests(unittest.TestCase):
    def setUp(self):
        self.patch = patch('iodock.serial.Serial', side_effect=FakeSerial)
        self.factory = self.patch.start()
        self.addCleanup(self.patch.stop)
        self.dock = IODock('OFFLINE', timeout=.5)
        self.assertTrue(self.dock.open())
        self.addCleanup(self.dock.close)
        self.ser = self.dock.serial

    def reply(self, text):
        self.ser.responder = lambda cmd: text

    def test_timing_contract(self):
        calls = [(lambda: self.dock.sync_utc(1700000000000123), 'SYNC 1700000000000123 UTC'),
                 (self.dock.time_status, 'TIME'),
                 (lambda: self.dock.timing_configure(1, 14, 10, 10000, 1000), 'TIMING CFG 1 14 10 10000 1000 0'),
                 (lambda: self.dock.timing_nmea(2, 9600, 899999), 'TIMING NMEA 2 9600 899999'),
                 (self.dock.timing_start, 'TIMING START'), (self.dock.timing_stop, 'TIMING STOP'),
                 (self.dock.timing_status, 'TIMING STAT'),
                 (lambda: self.dock.boot_sequence('timing_boot'), 'BOOT SEQ timing_boot'),
                 (self.dock.boot_sequence, 'BOOT SEQ OFF'),
                 (self.dock.boot_sequence_status, 'BOOT SEQ STAT')]
        for call, command in calls:
            self.assertTrue(call().success)
            self.assertEqual(self.ser.writes[-1], command)
        self.reply('OK TIME source=HOST unix_us=1700000000000123\n')
        self.assertIn('source=HOST', self.dock.time_status().payload)
        self.reply('ERR TIMING START E_BUSY resource owned\n')
        self.assertEqual(self.dock.timing_start().error_code, 5)
        self.reply('ERR TIMING E_BADCMD unknown command\n')
        self.assertEqual(self.dock.timing_start().error_code, 0)
        self.assertIsNone(self.dock._failure)

    def test_utc_sampled_after_send_lock(self):
        with patch('iodock.time.time_ns', return_value=1700000000123456789) as now:
            with concurrent.futures.ThreadPoolExecutor() as pool:
                with self.dock._command_lock:
                    started = threading.Event()
                    def run():
                        started.set()
                        return self.dock.syncUtc()
                    future = pool.submit(run)
                    self.assertTrue(started.wait(1))
                    now.assert_not_called()
                self.assertTrue(future.result(timeout=1).success)
            now.assert_called_once()
        self.assertEqual(self.ser.writes[-1], 'SYNC 1700000000123456 UTC')

    def test_timing_invalid_values_never_write(self):
        for args in [(0,14,10,100,100), (1,0,10,100,100), (1,14,0,100,100),
                     (1,14,101,100,100), (1,14,10,99,100), (1,14,10,1000000,100),
                     (1,14,100,100,10000), (1,14,10,100,99)]:
            with self.assertRaises(ValueError): self.dock.timing_configure(*args)
        for args in [(3,9600,0), (0,1199,0), (1,7800001,0), (1,9600,900000)]:
            with self.assertRaises(ValueError): self.dock.timing_nmea(*args)
        for name in ['OFF', 'stat', 'x'*16, 'a b', 'x\nSAVE']:
            with self.assertRaises(ValueError): self.dock.boot_sequence(name)
        self.assertEqual(self.ser.writes, [])

    def test_multiline_info_and_following_ping(self):
        self.ser.responder = lambda cmd: (
            'OK INFO\r\n board=IO-Dock\n mcu=RP2040\n fw=0.1.3\n'
            ' uid=abc\n clk=125000000\n io=6 pwm=4 uart=2 i2c=1 spi=1 adc=3\nEND\n'
            if cmd == 'INFO' else 'OK PING PONG 0.1.3\n')
        info = self.dock.get_info()
        self.assertEqual(info['io'], '6')
        self.assertEqual(info['adc'], '3')
        self.assertEqual(info['board'], 'IO-Dock')
        self.assertTrue(self.dock.ping())
        self.assertEqual(self.ser.resets, 1)

    def test_events_data_interleaved_and_empty_event(self):
        received = queue.Queue()
        self.dock.on_event(lambda *args: received.put(('EVT', args)))
        self.dock.on_data(lambda *args: received.put(('DATA', args)))
        self.dock.start_event_listener()
        self.reply('EVT READY\nOK INFO\n board=IO-Dock\n'
                   'DATA ADC0 1 2048\nEVT GPIO IO1 HIGH\nEND\n')
        self.assertEqual(self.dock.get_info(), {'board': 'IO-Dock'})
        self.assertEqual([received.get(timeout=1) for _ in range(3)], [
            ('EVT', ('READY', '')), ('DATA', ('ADC0', '1 2048')),
            ('EVT', ('GPIO', 'IO1 HIGH'))])
        self.assertEqual(len(self.ser.readers), 1)

    def test_data_end_during_commands(self):
        received = queue.Queue()
        self.dock.on_data(lambda *args: received.put(args))
        self.ser.responder = lambda cmd: (
            'DATA ADC0 2048,2049\nDATA_END ADC0\nOK INFO\n'
            ' board=IO-Dock\nDATA_END ADC1\nEND\n'
            if cmd == 'INFO' else 'DATA_END ADC2\nOK PING PONG\n')
        response = self.dock.send_command('INFO')
        self.assertTrue(response.success)
        self.assertEqual(response.payload, 'board=IO-Dock')
        self.assertTrue(self.dock.ping())
        self.assertEqual([received.get(timeout=1) for _ in range(4)], [
            ('ADC0', '2048,2049'), ('DATA_END', 'ADC0'),
            ('DATA_END', 'ADC1'), ('DATA_END', 'ADC2')])
        self.assertIsNone(self.dock._failure)
        self.assertEqual(len(self.ser.readers), 1)

    def test_data_end_without_callback(self):
        self.reply('DATA_END ADC0\nOK INFO\n board=IO-Dock\n'
                   'DATA_END ADC1\nEND\n')
        response = self.dock.send_command('INFO')
        self.assertTrue(response.success)
        self.assertEqual(response.payload, 'board=IO-Dock')
        self.reply('DATA_END ADC2\nOK PING PONG\n')
        self.assertTrue(self.dock.ping())

    def test_data_end_callback_payload(self):
        received = queue.Queue()
        self.dock.on_data(lambda *args: received.put(args))
        self.reply('DATA_END\nDATA_END ADC0 count=2\nOK PING PONG\n')
        self.assertTrue(self.dock.ping())
        self.assertEqual(received.get(timeout=1), ('DATA_END', ''))
        self.assertEqual(received.get(timeout=1), ('DATA_END', 'ADC0 count=2'))

    def test_parallel_commands(self):
        self.ser.responder = lambda cmd: 'OK SYNC offset=' + cmd.split()[1] + '\n'
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(self.dock.sync_time, range(40)))
        self.assertEqual(results, list(range(40)))
        self.assertEqual(len(self.ser.readers), 1)

    def test_callback_can_send_command_and_exception_isolated(self):
        finished = queue.Queue()
        def callback(event, data):
            if event == 'BAD':
                raise RuntimeError('intentional')
            finished.put(self.dock.ping())
        self.dock.on_event(callback)
        self.dock.start_event_listener()
        self.reply('OK PING PONG 0.1.3\n')
        with self.assertLogs('iodock', level='ERROR'):
            self.ser.emit('EVT BAD\nEVT GOOD\n')
            self.assertTrue(finished.get(timeout=1))

    def test_stop_listener_keeps_command_reader(self):
        self.dock.start_event_listener()
        self.dock.stop_event_listener()
        self.reply('EVT GPIO IO1 HIGH\nOK PING PONG\n')
        self.assertTrue(self.dock.ping())

    def test_io_channel_boundaries(self):
        self.reply('OK IO READALL\n IO1 LOW IO2 HIGH IO3 LOW IO4 LOW IO5 HIGH IO6 LOW\nEND\n')
        self.assertEqual(self.dock.io_read_all(), [Level.LOW, Level.HIGH, Level.LOW,
                                                Level.LOW, Level.HIGH, Level.LOW])
        self.reply('OK IO READ IO1 LOW\n')
        self.assertEqual(self.dock.io_read('IO1'), Level.LOW)
        self.reply('OK IO READ IO2 HIGH\n')
        with self.assertRaises(ValueError):
            self.dock.io_read('IO1')

    def test_adc_and_pwm_read(self):
        self.reply('OK ADC READ ADC2 2048 1650mV\n')
        self.assertEqual(self.dock.adc_read('ADC2'), ADCReading(2048, 1650))
        self.reply('OK ADC READALL\n ADC2 4095 3300mV\n ADC0 0 0mV\n ADC1 2048 1650mV\nEND\n')
        self.assertEqual(self.dock.adc_read_all(), [ADCReading(0, 0), ADCReading(2048, 1650), ADCReading(4095, 3300)])
        self.reply('OK PWM READ PWM3 1000Hz 50% running pol=invert\n')
        self.assertEqual(self.dock.pwm_read('PWM3'), PWMConfig(1000, 32768, True, True))
        self.reply('OK ADC TEMP -2.125C\n')
        self.assertEqual(self.dock.adc_read_temp(), -2.125)

    def test_errors_single_and_two_word(self):
        for command, line, code in [('LED ON', 'ERR LED E_PARAM ON|OFF', 1),
                                    ('IO WRITE IO1 HIGH', 'ERR IO WRITE E_CFG', 4),
                                    ('IO WHAT', 'ERR IO E_BADCMD unknown command', 0),
                                    ('PING', 'ERR PING E_FUTURE message', -1)]:
            with self.subTest(command=command):
                self.reply(line + '\n')
                result = self.dock.send_command(command)
                self.assertFalse(result.success)
                self.assertEqual(result.command, command)
                self.assertEqual(result.error_code, code)
                self.assertTrue(result.error)

    def test_hex_and_scan(self):
        for method, args, reply in [
            (self.dock.uart_receive, ('USART2',), 'OK UART RX USART2 HEX:00FF'),
            (self.dock.i2c_read_reg, (0x68, 0, 2), 'OK I2C READ HEX:00FF'),
            (self.dock.i2c_read, (0x68, 2), 'OK I2C RONLY HEX:00FF'),
            (self.dock.spi_read, (2,), 'OK SPI READ HEX:00FF'),
            (self.dock.spi_transfer, (b'12',), 'OK SPI XFR HEX:00FF')]:
            self.reply(reply + '\n')
            self.assertEqual(method(*args), b'\x00\xff')
        self.reply('OK I2C SCAN\n 0x3C 0x68\nEND\n')
        self.assertEqual(self.dock.i2c_scan(), [0x3c, 0x68])
        self.reply('OK I2C SCAN\n\nEND\n')
        self.assertEqual(self.dock.i2c_scan(), [])

    def test_corrected_commands(self):
        self.dock.i2c_write_reg(0x68, 0x6b, b'\x00\xff')
        self.dock.i2c_write(0x68, b'\x12\x34')
        self.dock.pwm_tick_off('PWM3')
        self.dock.adc_start_sample('ADC0', 1000)
        self.assertEqual(self.ser.writes, ['I2C WRITE 0x68 0x6B 00FF',
                         'I2C WRONLY 0x68 1234', 'PWM TICK OFF', 'ADC SAMPLE ADC0 1000 1000'])
        with self.assertRaises(ValueError):
            self.dock.adc_start_sample('ADC0', 1000, 0)

    def test_timeout_late_response_and_reopen(self):
        self.dock.timeout = .03
        self.reply('OK INFO\n board=partial\n')
        result = self.dock.send_command('INFO')
        self.assertFalse(result.success)
        self.assertEqual(result.error_code, 6)
        self.ser.emit('END\nOK PING PONG\n')
        self.assertFalse(self.dock.ping())
        self.assertEqual(self.ser.writes, ['INFO'])
        self.dock.close()
        self.assertTrue(self.dock.open())
        self.dock.serial.responder = lambda cmd: 'OK PING PONG\n'
        self.assertTrue(self.dock.ping())

    def test_close_unblocks_pending(self):
        self.reply(None)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            result = pool.submit(self.dock.send_command, 'PING')
            self.assertTrue(self.ser.written.wait(1))
            self.dock.close()
            self.assertFalse(result.result(timeout=1).success)
        self.assertFalse(self.dock._reader_thread.is_alive())

    def test_read_disconnect(self):
        self.reply(None)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            result = pool.submit(self.dock.send_command, 'PING')
            self.assertTrue(self.ser.written.wait(1))
            self.ser.incoming.put(OSError('disconnected'))
            self.assertIn('disconnected', result.result(timeout=1).error)

    def test_bad_response_and_short_write(self):
        self.reply('noise\n')
        self.assertFalse(self.dock.send_command('PING').success)
        self.dock.close()
        self.assertTrue(self.dock.open())
        self.dock.serial.write = lambda data: 1
        self.assertFalse(self.dock.send_command('PING').success)

    def test_open_idempotent_and_validation(self):
        self.assertTrue(self.dock.open())
        self.assertEqual(self.factory.call_count, 1)
        for cmd in ['', 'PING\nRESET', 'PING\r', 'x' * 512, 'BIN ENTER']:
            with self.assertRaises(ValueError):
                self.dock.send_command(cmd)
        self.assertEqual(self.ser.writes, [])


if __name__ == '__main__':
    unittest.main()
