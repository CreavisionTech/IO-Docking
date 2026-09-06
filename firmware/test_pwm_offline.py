"""Compile actual PWM driver sections against a host register model. No hardware.

Run: python firmware/test_pwm_offline.py [--cc gcc]
The model checks software state/order; it does not simulate electrical timing.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

STUB = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define PWM_COUNT 4
#define PWM_CHAN_A 0
#define PWM_CHAN_B 1
#define PWM_CH0_CSR_EN_BITS 1
#define PWM_CH0_CSR_PH_ADV_BITS 128
#define PWM_CH0_CSR_PH_RET_BITS 64
#define PWM_CH0_DIV_INT_BITS 0xff0
#define PWM_CH0_DIV_INT_LSB 4
#define PWM_CH0_DIV_FRAC_BITS 15
#define PWM_CH0_DIV_FRAC_LSB 0
#define GPIO_OVERRIDE_NORMAL 0
#define GPIO_OVERRIDE_LOW 2
#define GPIO_FUNC_PWM 4
#define GPIO_FUNC_SIO 5
#define GPIO_OUT 1
#define PWM_IRQ_WRAP 4
#define clk_sys 0
enum { E_PARAM=1, E_RANGE, E_CFG, E_DENIED, E_BUSY, E_TIMEOUT };
static const struct { uint8_t gpio,slice,chan; } PWM_MAP[4] = {
    {7,3,1}, {8,4,0}, {9,4,1}, {10,5,0}
};
static struct { struct { uint32_t csr,div,top,ctr,cc[2]; } slice[8];
    uint32_t en,intr,inte;
} regs, *pwm_hw=&regs;
static uint8_t overrides[32], pins[32], funcs[32];
static uint64_t now_us;
static bool check_stopped, stuck_phase;
static uint32_t clock_get_hz(int c) { (void)c; return 125000000; }
static uint64_t time_us_64(void) { return now_us++; }
static void tight_loop_contents(void) {}
static uint32_t save_and_disable_interrupts(void) { return 0; }
static void restore_interrupts(uint32_t s) { (void)s; }
static void hw_set_bits(uint32_t *p,uint32_t v) {
    *p |= v;
    for(int s=0;s<8;s++) {
        if(p==&regs.en) regs.slice[s].csr |= ((v>>s)&1);
        if(p==&regs.slice[s].csr && !stuck_phase) *p &= ~(128u|64u);
    }
}
static void hw_clear_bits(uint32_t *p,uint32_t v) {
    *p &= ~v;
    if(p==&regs.en) for(int s=0;s<8;s++) if(v&(1u<<s)) regs.slice[s].csr &= ~1u;
}
static void pwm_set_enabled(uint8_t s,bool on) {
    if(on) hw_set_bits(&regs.en,1u<<s); else hw_clear_bits(&regs.en,1u<<s);
}
typedef struct { uint32_t top,div; } pwm_config;
static pwm_config pwm_get_default_config(void) { return (pwm_config){65535,16}; }
static void pwm_config_set_clkdiv_int_frac(pwm_config *c,uint32_t i,uint32_t f) { c->div=i*16+f; }
static void pwm_config_set_wrap(pwm_config *c,uint32_t t) { c->top=t; }
static void pwm_init(uint8_t s,const pwm_config *c,bool on) {
    regs.slice[s].top=c->top; regs.slice[s].div=c->div;
    regs.slice[s].ctr=0; pwm_set_enabled(s,on);
}
static void pwm_set_chan_level(uint8_t s,uint8_t c,uint16_t v) { regs.slice[s].cc[c]=v; }
static void pwm_set_output_polarity(uint8_t s,bool a,bool b) { (void)s; (void)a; (void)b; }
static void gpio_set_outover(uint8_t g,uint8_t v) { overrides[g]=v; }
static void gpio_set_function(uint8_t g,uint8_t v) { funcs[g]=v; }
static void gpio_set_dir(uint8_t g,uint8_t v) { (void)g; (void)v; }
static void gpio_put(uint8_t g,uint8_t v) { pins[g]=v; }
static void irq_set_enabled(int irq,bool on) { (void)irq; (void)on; }
static void pwm_set_counter(uint8_t s,uint32_t v) {
    if(check_stopped) assert(!(regs.en & (1u<<s)));
    assert(v<=regs.slice[s].top); regs.slice[s].ctr=v;
}
int pwm_tick_off(void);
'''

TEST = r'''
int main(void) {
    uint32_t top,di,df;
    assert(pwm_start(0)==E_CFG);
    for(uint32_t f=8;f<=62500000;f=f*2+1) {
        assert(!pwm_compute(f,&top,&di,&df));
        assert(top<65535 && pwm_duty_to_cc(top,65535)==top+1);
        assert(pwm_duty_to_cc(top,0)==0);
    }
    assert(!pwm_cfg(2,1000,65535));
    assert(pwm_freqs[1]==1000 && pwm_freqs[2]==1000);
    assert(!pwm_start(1)); assert(!pwm_start(2)); assert(!pwm_stop(2));
    assert(!pwm_cfg(1,500,30000)); assert(!pwm_duty(2,65535));
    assert(regs.slice[4].cc[1]==0 && overrides[9]==GPIO_OVERRIDE_LOW);
    assert(pwm_freqs[2]==500 && pwm_run[1] && !pwm_run[2]);
    assert(!pwm_pol(2,1)); assert(overrides[9]==GPIO_OVERRIDE_LOW);
    assert(!pwm_cfg(0,500,65535)); assert(!pwm_cfg(3,777,100)); assert(!pwm_start(3));
    uint8_t channels[]={0,2}; check_stopped=true;
    assert(!pwm_sync(channels,2));
    assert(regs.en & (1u<<5)); assert(regs.slice[4].cc[1]==regs.slice[4].top+1);
    assert(!pwm_phase(0,2,0,360)); assert(regs.slice[3].ctr==0);
    assert(pwm_phase(0,2,2,0)==E_PARAM); check_stopped=false;
    assert(!pwm_freq(0,100000)); assert(pwm_phadj(0,1)==E_DENIED);
    assert(!pwm_freq(0,100)); assert(!pwm_phadj(0,1));
    stuck_phase=true; assert(pwm_phadj(0,1)==E_TIMEOUT); stuck_phase=false;
    assert(!pwm_sweep(0,10,12,1,0));
    for(int i=0;i<6;i++) { now_us+=1000; poll_pwm(now_us); }
    assert(sweep[0].active && pwm_duties[0]==10);
    assert(!pwm_sweep(0,10,12,1,1));
    for(int i=0;i<2;i++) { now_us+=1000; poll_pwm(now_us); }
    assert(!sweep[0].active && pwm_duties[0]==12);
    assert(!pwm_stop(0)); assert(!pwm_sweep(0,10,12,1,0));
    now_us+=1000; poll_pwm(now_us); assert(regs.slice[3].cc[1]==0);
    assert(pwm_tick_set(2,2)==E_BUSY); // running sibling
    for(uint32_t f=1;f<=10;f++) {
        assert(!pwm_tick_set(0,f));
        assert(g_tick_divide==1 || !(g_tick_divide&1));
        assert(pwm_freq(0,100)==E_BUSY && pwm_duty(0,1)==E_BUSY);
        unsigned high=0;
        for(unsigned i=0;i<g_tick_divide;i++) {
            regs.intr=1u<<g_tick_slice; pwm_tick_irq(); high+=pins[7];
        }
        if(g_tick_divide>1) assert(high==g_tick_divide/2);
        assert(pwm_tick_count()==1);
    }
    assert(!pwm_stop(0)); assert(!g_tick_on && !pwm_run[0]);
    assert(overrides[7]==GPIO_OVERRIDE_LOW && funcs[7]==GPIO_FUNC_PWM);
    puts("PWM offline regressions passed");
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    src = (Path(__file__).parent / 'src/drivers.c').read_text(encoding='utf-8')
    pwm = src.split('// ============================= PWM ====')[1].split('// ============================= UART')[0]
    pwm = pwm[pwm.index('\n'):]
    tick = src[src.index('static volatile uint8_t  g_tick_on;'):src.index('int adc_sample_start(')]
    sweep = src.split('    // PWM sweeps\n', 1)[1].split('    // ADC threshold monitor', 1)[0]
    code = STUB + pwm + tick + '\nstatic void poll_pwm(uint64_t now) {\n' + sweep + '}\n' + TEST
    with tempfile.TemporaryDirectory(prefix='pwm-offline-') as tmp:
        c = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test.exe'
        c.write_text(code, encoding='utf-8')
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()
