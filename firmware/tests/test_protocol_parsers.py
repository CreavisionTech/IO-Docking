"""Offline protocol tests: compile the real parsers against side-effect-counting stubs.
Run: python firmware/tests/test_protocol_parsers.py [--cc gcc]
No Pico SDK, serial access, or hardware is used. Temporary files are auto-cleaned.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

SRC = pathlib.Path(__file__).resolve().parents[1] / 'src'
COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"
#ifdef _WIN32
/* MinGW CRT lacks POSIX strtok_r. Match its delimiter-skipping semantics. */
static char *test_strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { *save=s; return NULL; }
    char *end = s + strcspn(s, delim);
    if (*end) *end++=0;
    *save=end; return s;
}
#define strtok_r test_strtok_r
#endif
#define IO_COUNT 6
#define PWM_COUNT 4
#define ADC_COUNT 3
#define STATUS_LED_GPIO 2
#define clk_sys 0
static unsigned effects, writes;
static int forced_rc;
static unsigned char output[20000];
static size_t output_len;
ring_t g_tx;
volatile uint8_t g_mode;
const char *const err_str[E_COUNT] = {"E_BADCMD","E_PARAM","E_NOTFOUND","E_RANGE","E_CFG","E_BUSY","E_TIMEOUT","E_NACK","E_OVERRUN","E_IO","E_LONG","E_DENIED"};
void ring_put(ring_t *r, const uint8_t *p, uint16_t n) {
    (void)r; assert(output_len + n < sizeof(output));
    memcpy(output + output_len, p, n); output_len += n; output[output_len] = 0; writes++;
}
static void clear_test(void) { effects = writes = output_len = 0; output[0] = 0; forced_rc = 0; }
typedef struct { uint8_t id[8]; } pico_unique_board_id_t;
static void pico_get_unique_board_id(pico_unique_board_id_t *p) { memset(p, 0, sizeof(*p)); }
static uint32_t clock_get_hz(int c) { (void)c; return 125000000; }
static uint64_t mock_now = 123;
static uint64_t time_us_64(void) { return mock_now; }
static void gpio_put(unsigned a, unsigned b) { (void)a; (void)b; effects++; }
static void watchdog_reboot(unsigned a, unsigned b, unsigned c) { (void)a; (void)b; (void)c; effects++; }
static void reset_usb_boot(unsigned a, unsigned b) { (void)a; (void)b; effects++; }
static struct { unsigned cs, result; } adc_regs;
#define adc_hw (&adc_regs)
'''
TEXT_TEST = r'''
static void bad(const char *s) {
    clear_test(); process_line(s);
    if (effects || strncmp((char *)output, "ERR ", 4)) { fprintf(stderr,"accepted invalid: %s; output=%s effects=%u\n",s,output,effects); abort(); }
}
int main(void) {
    const char *cases[] = {
        "IO WRITE IO10 HIGH", "IO WRITE IO1x HIGH", "IO WRITE IO0 HIGH", "IO WRITE I HIGH",
        "IO WRITE IO1 HIGH garbage", "IO PULSE IO1 LOW 12ms", "IO PULSE IO1 LOW -0",
        "PWM CFG PWM10 10 50", "PWM DUTY PWM1 #", "PWM DUTY PWM1 50%junk",
        "PWM DUTY PWM1 12xyz", "PWM DUTY PWM1 #65536", "PWM DUTY PWM1 %",
        "PWM FREQ PWM1 9999999999999999999999999", "PWM PHADJ PWM1 -32769",
        "PWM SYNC PWM1,PWM2,PWM3,PWM4,PWM1", "PWM SYNC PWM1,,PWM2", "PWM SYNC PWM1,PWM1",
        "PWM TICK OFF garbage", "IO EVENT IO1 off CHANGE", "ADC SAMPLE ADC0 stop 5",
        "UART CFG USART1 9600 264", "UART CFG USART10 9600", "UART CFG USART1 9600 8 N 257",
        "I2C READ 0x168 0 1", "I2C READ 0x68 256 1", "I2C READ 0x68 0 1x",
        "SPI CFG 1000000 256", "SPI CFG 1000000 0 garbage", "ADC THRESH ADC0 65536 0 ON",
        "ADC READ ADC00", "SYNC 9223372036854775808", "SEQ RUN name 65536", "SEQ DEF name \"unclosed",
        "RESET extra", "BOOTLOADER extra", "SAVE extra", "CFG CLEAR extra", "BIN ENTER extra",
        "IO WRITE IO1 HIGH\nRESET", "UART TX USART1 HEX:0z", "SPI WRITE HEX:", "PINGjunk"
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) bad(cases[i]);
    char longline[800]; memset(longline, 'a', sizeof(longline)); memcpy(longline,"UART TX USART1 TXT:",19); longline[799]=0; bad(longline);
    clear_test(); process_line("IO WRITE IO1 HIGH"); assert(effects == 1 && !strncmp((char *)output,"OK ",3));
    clear_test(); process_line("LED ON"); assert(effects == 1);
    clear_test(); process_line("PING"); assert(strstr((char *)output,"0.1.4"));
    clear_test(); forced_rc=E_BUSY; process_line("ADC READALL"); assert(strstr((char *)output,"E_BUSY") && !strstr((char *)output,"OK"));
    clear_test(); forced_rc=E_IO; process_line("CFG CLEAR"); assert(strstr((char *)output,"E_IO"));
    puts("text: 42 invalid cases + valid/return-code checks passed");
}
'''
BIN_TEST = r'''
static void request(uint8_t op, const uint8_t *p, uint16_t n, uint8_t flags, int corrupt) {
    uint8_t f[4110]={BIN_SOF,BIN_TYPE_CMD,op,flags,(uint8_t)n,(uint8_t)(n>>8),0x34,0x12};
    if(n) memcpy(f+8,p,n);
    uint16_t c=crc16(f,8+n); f[8+n]=(uint8_t)c; f[9+n]=(uint8_t)(c>>8); if(corrupt) f[9+n]^=1;
    for(unsigned i=0;i<n+10u;i++) process_bin_byte(f[i]);
}
static void bad(uint8_t op, const uint8_t *p, uint16_t n) {
    clear_test(); request(op,p,n,0,0);
    if(effects || !output_len || output[1]!=BIN_TYPE_ERR) { fprintf(stderr,"accepted invalid op=%02x len=%u effects=%u\n",op,n,effects); abort(); }
    assert(writes==1 && output[6]==0x34 && output[7]==0x12);
}
int main(void) {
    uint8_t p[4096]={0};
    /* Every fixed handler rejects all short payloads and one extra byte. */
    const uint8_t fixed[][2] = {
        {0,0},{1,0},{2,0},{3,0},{4,1},{12,8},{13,0},{15,0},
        {0x10,3},{0x11,2},{0x12,1},{0x13,0},{0x14,1},{0x15,4},{0x16,2},
        {0x20,7},{0x21,5},{0x22,3},{0x23,1},{0x24,1},{0x25,9},{0x26,1},{0x28,5},{0x29,3},{0x2a,2},{0x2b,5},
        {0x30,8},{0x32,3},{0x33,1},{0x34,2},{0x35,1},{0x40,4},{0x41,0},{0x44,4},{0x45,3},{0x46,4},
        {0x50,7},{0x53,2},{0x54,1},{0x60,1},{0x61,0},{0x62,9},{0x63,1},{0x64,0},{0x65,6},
        {0x71,0},{0x74,0},{0x75,0}
    };
    for(unsigned i=0;i<sizeof(fixed)/sizeof(fixed[0]);i++) {
        for(unsigned n=0;n<fixed[i][1];n++) bad(fixed[i][0],p,n);
        bad(fixed[i][0],p,fixed[i][1]+1);
    }
    p[0]=255; bad(0x70,p,3); bad(0x33,p,1); bad(0x2b,p,5);
    memset(p,0,sizeof(p)); p[0]=1; p[1]='x'; p[2]=1; p[3]=0; p[4]='a'; bad(0x70,p,6);
    p[4]=0; bad(0x70,p,5);
    memset(p,0,sizeof(p)); p[1]=255; p[2]=255; bad(0x45,p,3); bad(0x32,p,3);
    memset(p,0,sizeof(p)); bad(0x31,p,4); bad(0x42,p,5); bad(0x43,p,4); bad(0x51,p,3); bad(0x52,p,3);
    p[0]=2; p[1]=0; p[2]=0; bad(0x27,p,3);
    clear_test(); request(2,NULL,0,1,0); assert(!effects && output[1]==BIN_TYPE_ERR);
    clear_test(); request(2,NULL,0,0,1); assert(!effects && output[1]==BIN_TYPE_ERR);
    clear_test(); request(0,NULL,0,0,0); assert(output[8]==0x4f && output[11]==4 && writes==1);
    clear_test(); forced_rc=E_BUSY; request(0x61,NULL,0,0,0); assert(output[1]==BIN_TYPE_ERR && output[8]==E_BUSY);
    clear_test(); bin_send(BIN_TYPE_RESP,0x51,5,p,4096); assert(writes==1 && output_len==4106 && crc16(output,4104)==rd16(output+4104));
    clear_test(); bin_send(BIN_TYPE_RESP,0,0,p,4097); assert(!writes);
    clear_test(); process_bin_byte(BIN_SOF); process_bin_byte(BIN_TYPE_CMD);
    mock_now += 1000001; request(0,NULL,0,0,0); assert(output[1]==BIN_TYPE_RESP && output[8]==0x4f);
    puts("binary: all fixed payload boundaries, malformed variable payloads, flags/CRC, version, atomic TX, truncated-frame timeout passed");
}
'''

def run(cc):
    # Extract exact declarations, replacing only platform/driver implementation.
    declarations = '\n'.join((SRC / f).read_text(encoding='utf-8') for f in ('drivers.h','seq.h','config.h'))
    stubs = []
    for ret,name,args in re.findall(r'^(void|int|bool|u?int(?:8|16|32|64)_t)\s+(\w+)\(([^;]*?)\);',declarations,re.M):
        body='effects++;' + ('' if ret=='void' else ' return forced_rc;')
        stubs.append(f'{ret} {name}({args}) {{ {body} }}')
    with tempfile.TemporaryDirectory(prefix='iodock-parser-') as tmp:
        tmp=pathlib.Path(tmp)
        for filename,tests in [('text_cmd.c',TEXT_TEST),('bin_proto.c',BIN_TEST)]:
            source=(SRC/filename).read_text(encoding='utf-8')
            source=re.sub(r'^#include "(?!version.h|parse_utils.h|text_cmd.h|bin_proto.h|protocol.h)[^"]+"\n','',source,flags=re.M)
            c=tmp/(filename+'.c'); exe=tmp/(filename+'.exe')
            c.write_text(COMMON+'\n'+'\n'.join(stubs)+'\n'+source+'\n'+tests,encoding='utf-8')
            subprocess.run([cc,'-std=gnu99','-O1','-I',str(SRC),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--cc',default='gcc'); run(parser.parse_args().cc)
