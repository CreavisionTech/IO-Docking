#ifndef CONFIG_H
#define CONFIG_H

// Flash-resident parameter storage.
//   SAVE       -> snapshot current driver state (PWM / IO / UART / I2C / SPI /
//                 sequence) into the last flash sector.
//   LOAD       -> read, validate (magic+version+CRC) and apply it now.
//   CFG CLEAR  -> erase the stored config (next boot restores defaults).
// On power-up main() calls config_restore() which auto-applies if valid.
// Saving is explicit (manual SAVE), so config changes do not wear the flash.

int config_save(void);       // 0 on success
int config_load(void);       // 0 on success, E_NOTFOUND if no valid config
int config_clear(void);      // always 0
int config_restore(void);    // boot-time auto-restore (no-op if nothing saved)
int config_valid(void);      // 1 if a valid config is currently stored

#endif
