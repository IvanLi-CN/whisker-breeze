#ifndef EEPROM_FONT_STORAGE_H
#define EEPROM_FONT_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define EEPROM_TOTAL_SIZE_BYTES 4096u
#define EEPROM_PAGE_SIZE_BYTES 32u
#define EEPROM_FONT_BASE 0x0200u
#define EEPROM_FONT_LENGTH 0x800u

bool eeprom_read_bytes(uint16_t offset, uint8_t *data, uint16_t length);
bool eeprom_write_bytes(uint16_t offset, const uint8_t *data, uint16_t length);
bool eeprom_wait_ready(uint32_t timeout_ms);
bool font_storage_fetch_rows(uint8_t code, uint8_t *rows);
void ssd1306_font_fetch(uint8_t chr, uint8_t *rows);

#endif /* EEPROM_FONT_STORAGE_H */
