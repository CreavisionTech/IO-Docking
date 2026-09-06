#ifndef SEQ_H
#define SEQ_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// Core 0 only: initialize once before launching core 1.
void seq_init(void);
// True only after core 1 successfully installs flash safety support.
// Main should wait with a timeout (false also covers initialization failure).
bool seq_core1_ready(void);
// Call on every core-0 main-loop iteration to service ADC SAMPLE requests.
void seq_poll_core0(void);
void seq_core1_entry(void); // runs on core 1

int seq_define(const char *name, const char *script);
int seq_show(const char *name, char *out, uint16_t cap);
int seq_list(char *out, uint16_t cap);
int seq_run(const char *name, uint16_t repeat); // repeat=0 -> infinite; E_BUSY until previous DONE is consumed
int seq_stop(void);
int seq_stat(uint8_t *state, char *name, uint16_t namecap, uint32_t *remaining, uint32_t *step);
int seq_del(const char *name);

// Core-0 APIs are serialized by the main loop; do not call them from IRQs.
// Main loop: if a run just finished, return 1 and copy the sequence name.
int seq_take_done(char *name, uint16_t cap);
// First driver error for the last/current run; 0 for normal finish or STOP.
// Preserved through define/delete/DONE consumption, reset on accepted RUN.
// DONE means termination, not success: inspect this before the next RUN.
int seq_last_error(void);

#endif
