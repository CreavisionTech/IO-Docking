"""Host-only seq.c regression tests. No SDK, serial port or hardware required.
Run: python firmware/test_seq_regression.py [--cc gcc]
Compiles the actual seq.c against temporary deterministic SDK/driver doubles.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

STUB = r"""
#ifndef SEQ_TEST_SDK
#define SEQ_TEST_SDK
#include <stdint.h>
#include <stdbool.h>
typedef struct { int held; } critical_section_t;
void critical_section_init(critical_section_t *);
void critical_section_enter_blocking(critical_section_t *);
void critical_section_exit(critical_section_t *);
uint64_t time_us_64(void);
void tight_loop_contents(void);
void __sev(void);
void __wfe(void);
bool flash_safe_execute_core_init(void);
#define PWM_CHAN_A 0
#define PWM_CHAN_B 1
#endif
"""

TEST = r"""
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
// The bundled MinGW lacks POSIX strtok_r; use an equivalent host shim.
static char *host_strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    s += strspn(s, delim);
    if (!*s) { *save = s; return NULL; }
    char *end = s + strcspn(s, delim);
    if (*end) *end++ = 0;
    *save = end;
    return s;
}
#define strtok_r host_strtok_r
#endif
#include "seq.c"
static unsigned checks, writes, samples, pwm_calls;
static uint64_t now, stop_at;
static int core, adc_busy, adc_rc, level, io_rc, pwm_rc, uart_rc;
static uint16_t last_duty;
static bool flash_ok = true, flash_called, cancel_sample, poll_enabled = true;
static jmp_buf idle;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)
void critical_section_init(critical_section_t *p) { p->held = 0; }
void critical_section_enter_blocking(critical_section_t *p) { assert(!p->held); p->held = 1; }
void critical_section_exit(critical_section_t *p) { assert(p->held); p->held = 0; }
uint64_t time_us_64(void) { return now; }
void __sev(void) {}
void tight_loop_contents(void) {
    now += 1000;
    if (stop_at && now >= stop_at) {
        int old = core; core = 0; CHECK(seq_stop() == 0); core = old; stop_at = 0;
    }
}
void __wfe(void) {
    if (sample_pending && poll_enabled) {
        int old = core; core = 0;
        CHECK(samples == 0); // no ADC initialization before core-0 poll
        if (cancel_sample) CHECK(seq_stop() == 0);
        seq_poll_core0(); core = old;
    } else longjmp(idle, 1);
}
bool flash_safe_execute_core_init(void) {
    CHECK(core == 1); CHECK(!core1_ready); flash_called = true; return flash_ok;
}
int io_configure(uint8_t ch, uint8_t dir, uint8_t pull) { (void)ch; (void)dir; (void)pull; return 0; }
int io_write(uint8_t ch, uint8_t v) { (void)ch; ++writes; level = v; return io_rc; }
int pwm_duty(uint8_t ch, uint16_t duty) { (void)ch; ++pwm_calls; last_duty = duty; return pwm_rc; }
int uart_tx(uint8_t ch, const uint8_t *data, uint16_t n) { (void)ch; (void)data; (void)n; return uart_rc; }
int adc_sample_active(void) { return adc_busy; }
int adc_sample_start(uint8_t ch, uint32_t rate, uint32_t ms) {
    CHECK(core == 0); CHECK(ch == 0 && rate == 1000 && ms == 10); ++samples; return adc_rc;
}
static void reset(void) {
    seq_init(); now = stop_at = 0; writes = samples = pwm_calls = 0;
    core = 0; adc_busy = adc_rc = io_rc = pwm_rc = uart_rc = 0; cancel_sample = false;
    flash_ok = true; flash_called = false; poll_enabled = true;
}
static void launch(void) {
    core = 1;
    if (!setjmp(idle)) seq_core1_entry();
    core = 0;
}
static void execute(void) {
    CHECK(start_pending); start_pending = false;
    core = 1; execute_run(); core = 0;
}
static void done(const char *expected) {
    char name[16]; CHECK(seq_take_done(name, sizeof(name)) == 1);
    CHECK(strcmp(name, expected) == 0); CHECK(seq_take_done(name, sizeof(name)) == 0);
}
int main(void) {
    char out[256];
    reset(); CHECK(!seq_core1_ready()); CHECK(seq_run("", 1) == E_NOTFOUND);
    CHECK(seq_define("a", "IO IO1 HIGH") == 0); CHECK(seq_run("a", 1) == E_CFG);
    launch(); CHECK(flash_called && seq_core1_ready());
    CHECK(seq_run("a", 1) == 0);
    CHECK(seq.state == 1); CHECK(seq_take_done(out, sizeof(out)) == 0);
    CHECK(seq_run("a", 1) == E_BUSY); CHECK(seq_define("b", "WAIT 1") == E_BUSY);
    CHECK(seq_del("a") == E_BUSY); CHECK(seq_stop() == 0);
    execute(); CHECK(writes == 0); CHECK(seq.state == 0);
    CHECK(seq_define("b", "WAIT 1") == 0); CHECK(seq_run("b", 1) == E_BUSY);
    CHECK(seq_del("b") == 0); done("a"); CHECK(seq_run("", 1) == E_NOTFOUND);
    for (unsigned i = 0; i < 3; ++i) {
        const uint16_t repeats[] = {256, 257, 65535};
        reset(); launch(); CHECK(seq_define("r", "IO IO1 HIGH") == 0);
        CHECK(seq_run("r", repeats[i]) == 0); execute();
        CHECK(writes == repeats[i] && seq.remaining == 0); done("r");
    }
    reset(); launch(); CHECK(seq_define("w", "IO IO1 HIGH; WAIT 7") == 0);
    CHECK(seq_run("w", 3) == 0); execute(); CHECK(writes == 3 && now == 21000); done("w");
    CHECK(seq_define("w", "WAIT 2 @5; WAIT 3 +4") == 0);
    CHECK(seq_run("w", 1) == 0); now = 0; execute(); CHECK(now == 14000); done("w");
    CHECK(seq_define("w", "WAIT 5") == 0); CHECK(seq_run("w", 0) == 0);
    now = 0; stop_at = 17000; execute(); CHECK(now == 17000); done("w");
    CHECK(seq_define("p", "PULSE IO1 HIGH 100; IO IO2 HIGH") == 0);
    writes = 0; now = 0; stop_at = 5000; CHECK(seq_run("p", 1) == 0); execute();
    CHECK(writes == 2 && level == 0); done("p");
    CHECK(seq_define("s", "SWEEP PWM1 #65534 #65535 1") == 0);
    CHECK(seq_run("s", 1) == 0); execute(); CHECK(pwm_calls == 2 && last_duty == 65535); done("s");
    const char *bad[] = {
        "", "WAIT", "WAIT -1", "WAIT 1x", "WAIT 1000001", "WAIT 4294967296",
        "WAIT 1 extra", "WAIT 1 @", "WAIT 1 @4294967295", "WAIT 1; IO IO1 HIGH @0",
        "IO IO10 HIGH", "IO I HIGH", "IO IO1 HIGH x", "IO IO1 HIGH@1x",
        "IO IO1 HIGH @4294967295; IO IO1 LOW +1", "IO IO1 HIGH a b c d e f g h",
        "PWM PWM10 1", "PWM PWM1 #", "PWM PWM1 #2x", "PWM PWM1 101%",
        "PWM PWM1 1%x", "PWM PWM1 #1%", "PWM PWM1 65536", "PWM PWM1 abc",
        "SWEEP PWM1 0 100 4294967295", "SWEEP PWM1 0 1 0",
        "PULSE IO1 HIGH 12x", "PULSE IO1 HIGH 2 @4294967295",
        "TX USART10 HEX:00", "TX USART1 HEX:0", "TX USART1 HEX:00zz",
        "SAMPLE ADC00 1000 10", "SAMPLE ADC0 999 10", "SAMPLE ADC0 500001 10",
        "SAMPLE ADC0 1000 0", "SAMPLE ADC0 1000 1000001",
        "SAMPLE ADC0 1000x 10", "SAMPLE ADC0 1000 10x"
    };
    CHECK(seq_define("keep", "TX USART1 HEX:1234; IO IO1 HIGH") == 0);
    seq_t saved = seq;
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        int rejected = seq_define("bad", bad[i]);
        if (!rejected) fprintf(stderr, "accepted invalid script: %s\n", bad[i]);
        CHECK(rejected != 0);
        CHECK(memcmp(&seq, &saved, sizeof(seq)) == 0);
        CHECK(seq_show("keep", out, sizeof(out)) == 0 && !strcmp(out, saved.script));
    }
    CHECK(seq_define("maxadc", "SAMPLE ADC0 500000 1000000") == 0);
    CHECK(seq_define("t", "TX USART1 TXT:a@b+c @5;\tIO\tIO1\tLOW") == 0);
    CHECK(seq.pool_len == 5 && !memcmp(seq.pool, "a@b+c", 5));
    CHECK(seq_list(NULL, 1) == E_PARAM); CHECK(seq_list(out, 0) == E_PARAM);
    CHECK(seq_show("t", NULL, 1) == E_PARAM); CHECK(seq_stat(NULL, out, 1, NULL, NULL) == E_PARAM);
    for (unsigned mode = 0; mode < 3; ++mode) {
        reset(); launch(); CHECK(seq_define("adc", "SAMPLE ADC0 1000 10; IO IO1 HIGH") == 0);
        adc_busy = 1; CHECK(seq_run("adc", 1) == E_BUSY); adc_busy = 0;
        CHECK(seq_run("adc", 1) == 0); adc_rc = mode == 1 ? E_BUSY : 0; cancel_sample = mode == 2;
        execute(); CHECK(samples == (mode == 2 ? 0u : 1u)); CHECK(writes == (mode == 0 ? 1u : 0u));
        CHECK(!sample_pending); CHECK(seq_last_error() == (mode == 1 ? E_BUSY : 0)); done("adc");
        CHECK(seq_last_error() == (mode == 1 ? E_BUSY : 0));
    }
    const char *driver_scripts[] = {
        "IO IO1 HIGH; IO IO2 HIGH", "PWM PWM1 20; IO IO2 HIGH",
        "SWEEP PWM1 #1 #2 1; IO IO2 HIGH", "TX USART1 HEX:00; IO IO2 HIGH"
    };
    for (unsigned i = 0; i < 4; ++i) {
        reset(); launch(); CHECK(seq_define("err", driver_scripts[i]) == 0);
        CHECK(seq_run("err", 1) == 0);
        if (i == 0) io_rc = E_IO;
        else if (i < 3) pwm_rc = E_CFG;
        else uart_rc = E_OVERRUN;
        execute(); CHECK(writes == (i == 0 ? 1u : 0u));
        CHECK(seq_last_error() == (i == 0 ? E_IO : i < 3 ? E_CFG : E_OVERRUN));
        done("err"); CHECK(seq_define("new", "WAIT 0") == 0);
        CHECK(seq_last_error() != 0); CHECK(seq_run("new", 1) == 0);
        CHECK(seq_last_error() == 0); execute(); done("new");
    }
    reset(); CHECK(seq_define("broken", "IO IO1 HIGH; nonsense") == E_PARAM);
    CHECK(seq_run("broken", 1) == E_NOTFOUND);
    CHECK(seq_list(out, sizeof(out)) == 0 && out[0] == 0);
    reset(); flash_ok = false; launch(); CHECK(flash_called && !seq_core1_ready());
    CHECK(seq_define("a", "WAIT 1") == 0); CHECK(seq_run("a", 1) == E_CFG);
    printf("PASS: %u checks (host deterministic scheduler; no hardware)\n", checks);
    return 0;
}
"""

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    src = Path(__file__).resolve().parent / "src"
    with tempfile.TemporaryDirectory(prefix="seq-regression-") as tmp:
        root = Path(tmp)
        for name in ("pico/stdlib.h", "pico/flash.h", "pico/critical_section.h", "pico/time.h", "hardware/pwm.h"):
            dest = root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(STUB)
        (root / "test.c").write_text(TEST)
        exe = root / "seq_test.exe"
        subprocess.run([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(root), "-I", str(src), str(root / "test.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=20)

if __name__ == "__main__":
    main()
