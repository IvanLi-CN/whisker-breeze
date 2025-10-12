// Minimal EEPROM-backed configuration for Whisker Breeze.
// Stores last mode, manual target, and optional temp-band offset.

#ifndef EEPROM_CONFIG_H
#define EEPROM_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// On-EEPROM representation of control mode.
typedef enum {
    CFG_MODE_TEMP   = 1,
    CFG_MODE_MANUAL = 2,
} cfg_mode_t;

typedef struct {
    uint8_t   version;                  // 0x01
    cfg_mode_t last_mode;               // TEMP or MANUAL
    uint16_t  manual_ratio_q8_8;        // 0..256 (1.00 -> 256)
    int16_t   temp_band_offset_cx100;   // centi-°C offset relative to [20,40]°C
} eeprom_runtime_cfg_t;

// Load the persisted configuration if valid; returns true on success.
bool eeprom_cfg_load(eeprom_runtime_cfg_t *out);

// Save a configuration snapshot; returns true on success.
bool eeprom_cfg_save(const eeprom_runtime_cfg_t *in);

#endif // EEPROM_CONFIG_H

