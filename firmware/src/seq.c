// Sequence engine. Sequences are parsed on core 0 and executed on core 1,
// so USB command handling is never blocked by a running sequence.
// Script language (per Host_Protocol.md 6.8): steps separated by ';',
// optional absolute "@ms" / relative "+ms" time per step.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/critical_section.h"
#include "pico/time.h"
#include "board.h"
#include "protocol.h"
#include "drivers.h"
#include "seq.h"

#define SEQ_MAX_STEPS  48
#define SEQ_POOL_SIZE  512
#define SEQ_SCRIPT_MAX 256


typedef struct {
    uint8_t  type;
    uint8_t  ch;
    uint32_t t_ms;      // absolute offset from sequence start
    uint32_t a, b, c;
    uint16_t data_off;  // into pool
    uint16_t data_len;
} seq_step_t;

enum { STEP_IO = 0, STEP_PULSE, STEP_PWM, STEP_SWEEP, STEP_TX, STEP_SAMPLE, STEP_WAIT };

typedef struct {
    volatile uint8_t state;       // 0 idle, 1 running
    uint16_t repeat;               // total passes (0 = infinite)
    volatile uint8_t step_idx;
    volatile uint8_t stop;
    uint8_t step_count;
    char name[16];
    char script[SEQ_SCRIPT_MAX];
    seq_step_t steps[SEQ_MAX_STEPS];
    uint8_t pool[SEQ_POOL_SIZE];
    uint16_t pool_len;
    volatile uint32_t remaining;
} seq_t;

static seq_t seq;
static critical_section_t seq_lock;
static volatile bool core1_ready;
static bool start_pending, done_pending;
static char done_name[16];
// Mailbox is protected by seq_lock; only core 0 calls the ADC driver.
static bool sample_pending;
static uint8_t sample_ch;
static uint32_t sample_rate, sample_ms;
static int sample_result;
static int last_error;
static seq_t draft; // core-0-only scratch; avoid a large stack allocation

static bool is_running(void) {
    critical_section_enter_blocking(&seq_lock);
    bool running = seq.state != 0;
    critical_section_exit(&seq_lock);
    return running;
}

static bool stop_requested(void) {
    critical_section_enter_blocking(&seq_lock);
    bool stop = seq.stop != 0;
    critical_section_exit(&seq_lock);
    return stop;
}

static int p_uint(const char *s, uint32_t *out) {
    if (!s || !*s) return E_PARAM;
    uint32_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return E_PARAM;
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT32_MAX - d) / 10u) return E_RANGE;
        v = v * 10u + d;
    }
    *out = v;
    return 0;
}
static int add_ms(uint32_t a, uint32_t b, uint32_t *out) {
    if (b > UINT32_MAX - a) return E_RANGE;
    *out = a + b;
    return 0;
}

// ---------- shared helpers (duplicated locally to keep modules independent) ----------

static int p_io(const char *s) {
    if (s && strlen(s) == 3 && s[0] == 'I' && s[1] == 'O' && s[2] >= '1' && s[2] <= '6') return s[2] - '1';
    return -1;
}
static int p_pwm(const char *s) {
    if (s && strlen(s) == 4 && s[0] == 'P' && s[1] == 'W' && s[2] == 'M' && s[3] >= '1' && s[3] <= '4') return s[3] - '1';
    return -1;
}
static int p_uart(const char *s) {
    if (s && strlen(s) == 6 && strncasecmp(s, "USART", 5) == 0 && (s[5] == '1' || s[5] == '2')) return s[5] - '1';
    return -1;
}
static int p_adc(const char *s) {
    if (s && strlen(s) == 4 && strncasecmp(s, "ADC", 3) == 0 && s[3] >= '0' && s[3] <= '2') return s[3] - '0';
    return -1;
}
static int p_level(const char *s) {
    if (!s) return -1;
    if (!strcasecmp(s, "HIGH") || !strcmp(s, "1")) return 1;
    if (!strcasecmp(s, "LOW") || !strcmp(s, "0")) return 0;
    return -1;
}
static int p_duty(const char *s, uint32_t *raw) {
    if (!s || !*s) return E_PARAM;
    bool explicit_raw = *s == '#';
    if (explicit_raw) ++s;
    char buf[SEQ_SCRIPT_MAX];
    size_t len = strlen(s);
    if (len >= sizeof(buf)) return E_PARAM;
    strcpy(buf, s);
    bool percent = len && buf[len - 1] == '%';
    if (percent) buf[--len] = 0;
    if (percent && explicit_raw) return E_PARAM;
    uint32_t v;
    int rc = p_uint(buf, &v);
    if (rc) return rc;
    if (v > (percent ? 100u : 65535u)) return E_RANGE;
    *raw = !explicit_raw && v <= 100 ? v * 65535u / 100u : v;
    return 0;
}
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int p_hex_pool(const char *s, uint16_t *off, uint16_t *len) {
    size_t l = strlen(s);
    if (l % 2) return E_PARAM;
    *len = (uint16_t)(l / 2);
    if (draft.pool_len + *len > SEQ_POOL_SIZE) return E_RANGE;
    *off = draft.pool_len;
    for (uint16_t i = 0; i < *len; i++) {
        int hi = hexv(s[2 * i]), lo = hexv(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return E_PARAM;
        draft.pool[draft.pool_len++] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// ---------- script parsing (core 0) ----------

static int parse_step(char *buf, uint32_t cur_ms, uint32_t *next_ms) {
    *next_ms = cur_ms;

    // tokenize into a small array (suffix handling needs the last token)
    char *toks[9];
    int n = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, " \t\r\n", &save);
    while (tok && n < (int)(sizeof(toks) / sizeof(toks[0]))) {
        toks[n++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &save);
    }
    if (tok || n == 0) return E_PARAM;

    // time suffix: either a separate token ("@N"/"+N") or glued to the last
    // arg ("LOW@0", "50@50"). 1=absolute(@), 2=relative(+).
    int time_kind = 0;
    uint32_t time_val = 0;
    if (n >= 2 && (toks[n - 1][0] == '@' || toks[n - 1][0] == '+')) {
        time_kind = (toks[n - 1][0] == '@') ? 1 : 2;
        int rc = p_uint(toks[n - 1] + 1, &time_val);
        if (rc) return rc;
        n--;
    } else {
        char *last = toks[n - 1];
        size_t len = strlen(last);
        for (size_t i = 1; i < len; i++) {
            // Preserve nonnumeric @/+ in text while retaining legacy TXT:x@10.
            if (strncasecmp(last, "TXT:", 4) == 0 &&
                (i + 1 == len || last[i + 1] < '0' || last[i + 1] > '9')) continue;
            if (last[i] == '@' || last[i] == '+') {
                time_kind = (last[i] == '@') ? 1 : 2;
                int rc = p_uint(&last[i + 1], &time_val);
                if (rc) return rc;
                last[i] = '\0'; // truncate the suffix off the token
                break;
            }
        }
    }
    if (n == 0) return E_PARAM;

    const char *op = toks[0];

    if (draft.step_count >= SEQ_MAX_STEPS) return E_PARAM;
    seq_step_t *st = &draft.steps[draft.step_count];
    memset(st, 0, sizeof(*st));
    st->t_ms = cur_ms;
    if (time_kind == 1) st->t_ms = time_val;
    else if (time_kind == 2 && add_ms(cur_ms, time_val, &st->t_ms)) return E_RANGE;
    if (st->t_ms < cur_ms) return E_RANGE;

    if (n < 2) return E_PARAM;
    const char *a = toks[1];
    const char *b, *c, *d;

    if (strcasecmp(op, "WAIT") == 0) {
        if (n != 2) return E_PARAM;
        st->type = STEP_WAIT;
        int rc = p_uint(a, &st->a);
        if (rc) return rc;
        if (st->a > 1000000u) return E_RANGE;
    } else if (strcasecmp(op, "IO") == 0) {
        if (n != 3) return E_PARAM;
        int ch = p_io(a);
        if (ch < 0) return E_PARAM;
        st->type = STEP_IO; st->ch = (uint8_t)ch;
        int lv = p_level(toks[2]);
        if (lv < 0) return E_PARAM;
        st->a = (uint32_t)lv;
    } else if (strcasecmp(op, "PULSE") == 0) {
        if (n != 4) return E_PARAM;
        int ch = p_io(a);
        if (ch < 0) return E_PARAM;
        st->type = STEP_PULSE; st->ch = (uint8_t)ch;
        int lv = p_level(toks[2]);
        if (lv < 0) return E_PARAM;
        st->a = (uint32_t)lv;
        int rc = p_uint(toks[3], &st->b);
        if (rc) return rc;
    } else if (strcasecmp(op, "PWM") == 0) {
        if (n != 3) return E_PARAM;
        int ch = p_pwm(a);
        if (ch < 0) return E_PARAM;
        st->type = STEP_PWM; st->ch = (uint8_t)ch;
        int rc = p_duty(toks[2], &st->a);
        if (rc) return rc;
    } else if (strcasecmp(op, "SWEEP") == 0) {
        if (n != 5) return E_PARAM;
        int ch = p_pwm(a);
        if (ch < 0) return E_PARAM;
        st->type = STEP_SWEEP; st->ch = (uint8_t)ch;
        b = toks[2]; c = toks[3]; d = toks[4];
        int rc = p_duty(b, &st->a);
        if (rc) return rc;
        rc = p_duty(c, &st->b);
        if (rc) return rc;
        rc = p_uint(d, &st->c);
        if (rc) return rc;
        if (st->c == 0) return E_PARAM;
    } else if (strcasecmp(op, "TX") == 0) {
        if (n != 3) return E_PARAM;
        int ch = p_uart(a);
        if (ch < 0) return E_PARAM;
        st->type = STEP_TX; st->ch = (uint8_t)ch;
        b = toks[2];
        if (strncasecmp(b, "HEX:", 4) == 0) {
            int rc = p_hex_pool(b + 4, &st->data_off, &st->data_len);
            if (rc) return rc;
        } else if (strncasecmp(b, "TXT:", 4) == 0) {
            const char *t = b + 4;
            st->data_len = (uint16_t)strlen(t);
            if (draft.pool_len + st->data_len > SEQ_POOL_SIZE) return E_RANGE;
            st->data_off = draft.pool_len;
            memcpy(&draft.pool[draft.pool_len], t, st->data_len);
            draft.pool_len += st->data_len;
        } else {
            return E_PARAM;
        }
    } else if (strcasecmp(op, "SAMPLE") == 0) {
        if (n != 4) return E_PARAM;
        int ch = p_adc(a);
        if (ch < 0) return E_PARAM;
        st->type = STEP_SAMPLE; st->ch = (uint8_t)ch;
        int rc_rate = p_uint(toks[2], &st->a);
        if (rc_rate) return rc_rate;
        int rc = p_uint(toks[3], &st->b);
        if (rc) return rc;
        if (st->a < 1000 || st->a > 500000 || !st->b || st->b > 1000000 ||
            (uint64_t)st->a * st->b / 1000u > UINT32_MAX) return E_RANGE;
    } else {
        return E_PARAM;
    }

    *next_ms = st->t_ms;
    if (st->type == STEP_WAIT) {
        if (add_ms(st->t_ms, st->a, next_ms)) return E_RANGE;
        st->t_ms = *next_ms;
    } else if (st->type == STEP_PULSE) {
        if (add_ms(st->t_ms, st->b, next_ms)) return E_RANGE;
    }
    else if (st->type == STEP_SWEEP) {
        uint32_t steps = (st->a > st->b) ? (st->a - st->b) : (st->b - st->a);
        uint64_t duration = (uint64_t)steps * st->c;
        if (duration > UINT32_MAX || add_ms(st->t_ms, (uint32_t)duration, next_ms)) return E_RANGE;
    }

    draft.step_count++;
    return 0;
}

int seq_define(const char *name, const char *script) {
    if (is_running()) return E_BUSY;
    if (!name || !name[0] || strlen(name) >= sizeof(draft.name)) return E_PARAM;
    if (!script || strlen(script) >= SEQ_SCRIPT_MAX) return E_PARAM;

    memset(&draft, 0, sizeof(draft));
    strcpy(draft.name, name);
    strcpy(draft.script, script);
    draft.step_count = 0;
    draft.pool_len = 0;

    char tmp[SEQ_SCRIPT_MAX];
    strcpy(tmp, script);
    uint32_t cur_ms = 0;
    char *save = NULL;
    char *step = strtok_r(tmp, ";", &save);
    while (step) {
        while (*step == ' ' || *step == '\t') step++;
        if (*step) {
            int rc = parse_step(step, cur_ms, &cur_ms);
            if (rc) return rc;
        }
        step = strtok_r(NULL, ";", &save);
    }
    if (draft.step_count == 0) return E_PARAM;
    critical_section_enter_blocking(&seq_lock);
    seq = draft;
    critical_section_exit(&seq_lock);
    return 0;
}

// ---------- core 0 API ----------

int seq_show(const char *name, char *out, uint16_t cap) {
    if (!out || !cap) return E_PARAM;
    if (!seq.step_count || !name || strcmp(name, seq.name) != 0) return E_NOTFOUND;
    snprintf(out, cap, "%s", seq.script);
    return 0;
}

int seq_list(char *out, uint16_t cap) {
    if (!out || !cap) return E_PARAM;
    if (seq.step_count > 0) snprintf(out, cap, "%s", seq.name);
    else out[0] = '\0';
    return 0;
}

int seq_run(const char *name, uint16_t repeat) {
    critical_section_enter_blocking(&seq_lock);
    int rc = 0;
    if (seq.state || done_pending) rc = E_BUSY;
    else if (!seq.step_count || !name || strcmp(name, seq.name)) rc = E_NOTFOUND;
    else if (!core1_ready) rc = E_CFG;
    else if (adc_sample_active()) rc = E_BUSY;
    else {
        last_error = 0;
        seq.repeat = repeat;
        seq.stop = 0;
        seq.remaining = repeat;
        seq.step_idx = 0;
        seq.state = 1; // publish running BEFORE core 1 can consume the request
        start_pending = true;
    }
    critical_section_exit(&seq_lock);
    __sev();
    return rc;
}

int seq_stop(void) {
    critical_section_enter_blocking(&seq_lock);
    int rc = seq.state ? 0 : E_CFG;
    if (!rc) seq.stop = 1;
    critical_section_exit(&seq_lock);
    __sev();
    return rc;
}

int seq_stat(uint8_t *state, char *name, uint16_t namecap, uint32_t *remaining, uint32_t *step) {
    if (!state || !name || !namecap || !remaining || !step) return E_PARAM;
    critical_section_enter_blocking(&seq_lock);
    *state = seq.state;
    snprintf(name, namecap, "%s", seq.name);
    *remaining = seq.remaining;
    *step = seq.step_idx;
    critical_section_exit(&seq_lock);
    return 0;
}

int seq_del(const char *name) {
    if (is_running()) return E_BUSY;
    if (!seq.step_count || !name || strcmp(name, seq.name)) return E_NOTFOUND;
    memset(&seq, 0, sizeof(seq));
    return 0;
}

int seq_take_done(char *name, uint16_t cap) {
    if (!name || !cap) return 0;
    critical_section_enter_blocking(&seq_lock);
    bool done = done_pending;
    if (done) {
        snprintf(name, cap, "%s", done_name);
        done_pending = false;
    }
    critical_section_exit(&seq_lock);
    return done;
}

int seq_last_error(void) {
    critical_section_enter_blocking(&seq_lock);
    int rc = last_error;
    critical_section_exit(&seq_lock);
    return rc;
}

// Core 1 aborts subsequent steps on any driver failure.
static bool driver_failed(int rc) {
    if (!rc) return false;
    critical_section_enter_blocking(&seq_lock);
    if (!last_error) last_error = rc;
    seq.stop = 1;
    critical_section_exit(&seq_lock);
    return true;
}

bool seq_core1_ready(void) {
    critical_section_enter_blocking(&seq_lock);
    bool ready = core1_ready;
    critical_section_exit(&seq_lock);
    return ready;
}

void seq_poll_core0(void) {
    critical_section_enter_blocking(&seq_lock);
    if (sample_pending) {
        sample_result = seq.stop ? 0 : adc_sample_start(sample_ch, sample_rate, sample_ms);
        sample_pending = false;
    }
    critical_section_exit(&seq_lock);
    __sev();
}

// ---------- core 1 executor ----------

static void exec_pass(uint64_t start) {
    for (uint8_t i = 0; i < seq.step_count; i++) {
        if (stop_requested()) return;
        critical_section_enter_blocking(&seq_lock);
        seq.step_idx = i;
        critical_section_exit(&seq_lock);
        seq_step_t *s = &seq.steps[i];
        uint64_t target = start + (uint64_t)s->t_ms * 1000u;
        while (time_us_64() < target) {
            if (stop_requested()) return;
            tight_loop_contents();
        }
        if (stop_requested()) return;

        switch (s->type) {
            case STEP_IO:
                if (driver_failed(io_configure(s->ch, 0, 0))) return; // output
                if (driver_failed(io_write(s->ch, (uint8_t)s->a))) return;
                break;
            case STEP_PULSE:
                if (driver_failed(io_configure(s->ch, 0, 0))) return;
                if (driver_failed(io_write(s->ch, (uint8_t)s->a))) return;
                {
                    uint64_t end = time_us_64() + (uint64_t)s->b * 1000u;
                    while (time_us_64() < end) {
                        if (stop_requested()) break;
                        tight_loop_contents();
                    }
                }
                if (driver_failed(io_write(s->ch, (uint8_t)(s->a ^ 1)))) return;
                break;
            case STEP_PWM:
                if (driver_failed(pwm_duty(s->ch, (uint16_t)s->a))) return;
                break;
            case STEP_SWEEP: {
                int32_t cur = (int32_t)s->a;
                int32_t dst = (int32_t)s->b;
                int32_t dir = (dst >= cur) ? 1 : -1;
                while (cur != dst) {
                    if (stop_requested()) return;
                    if (driver_failed(pwm_duty(s->ch, (uint16_t)cur))) return;
                    uint64_t end = time_us_64() + (uint64_t)s->c * 1000u;
                    while (time_us_64() < end) {
                        if (stop_requested()) return;
                        tight_loop_contents();
                    }
                    cur += dir;
                }
                if (driver_failed(pwm_duty(s->ch, (uint16_t)dst))) return;
                break;
            }
            case STEP_TX:
                if (driver_failed(uart_tx(s->ch, &seq.pool[s->data_off], s->data_len))) return;
                break;
            case STEP_SAMPLE: {
                critical_section_enter_blocking(&seq_lock);
                sample_ch = s->ch;
                sample_rate = s->a;
                sample_ms = s->b;
                sample_pending = true;
                critical_section_exit(&seq_lock);
                __sev();
                for (;;) {
                    critical_section_enter_blocking(&seq_lock);
                    bool pending = sample_pending;
                    int rc = sample_result;
                    critical_section_exit(&seq_lock);
                    if (!pending) {
                        if (driver_failed(rc)) return;
                        break;
                    }
                    __wfe();
                }
                break;
            }
            default:
                break;
        }
    }
}

// A single completion slot applies backpressure until core 0 consumes DONE.
static void execute_run(void) {
    uint64_t start = time_us_64();
    for (;;) {
        exec_pass(start);
        if (stop_requested()) break;
        if (seq.repeat) {
            critical_section_enter_blocking(&seq_lock);
            bool last = --seq.remaining == 0;
            critical_section_exit(&seq_lock);
            if (last) break;
        }
        start = time_us_64();
    }
    critical_section_enter_blocking(&seq_lock);
    strcpy(done_name, seq.name);
    done_pending = true;
    seq.stop = 0;
    seq.state = 0; // last publication; no old-run writes after this point
    critical_section_exit(&seq_lock);
    __sev();
}

void seq_core1_entry(void) {
    bool ready = flash_safe_execute_core_init();
    critical_section_enter_blocking(&seq_lock);
    core1_ready = ready;
    critical_section_exit(&seq_lock);
    __sev();
    for (;;) {
        critical_section_enter_blocking(&seq_lock);
        bool run = ready && start_pending;
        if (run) start_pending = false;
        critical_section_exit(&seq_lock);
        if (run) execute_run();
        else __wfe(); // FIFO and its IRQ belong exclusively to flash lockout
    }
}

void seq_init(void) {
    // Call once on core 0 before launching core 1.
    critical_section_init(&seq_lock);
    memset(&seq, 0, sizeof(seq));
    last_error = 0;
    core1_ready = false;
    start_pending = done_pending = sample_pending = false;
}
