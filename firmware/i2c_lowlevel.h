#ifndef I2C_LOWLEVEL_H
#define I2C_LOWLEVEL_H

#include <stdbool.h>
#include <stdint.h>

void i2c1_configure_speed(uint32_t target_hz);
bool i2c1_wait_flag(uint32_t mask);
bool i2c1_wait_not_busy(void);
bool i2c1_read_current_u8(uint8_t addr, uint8_t *value);
bool i2c1_write_u8(uint8_t addr, uint8_t reg, uint8_t data);
bool i2c1_write_u16(uint8_t addr, uint8_t reg, uint16_t value);
bool i2c1_read_u16(uint8_t addr, uint8_t reg, uint16_t *value);
bool i2c1_read_u8(uint8_t addr, uint8_t reg, uint8_t *value);

#endif /* I2C_LOWLEVEL_H */
