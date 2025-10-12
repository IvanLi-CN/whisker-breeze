#include "eeprom_font_storage.h"

#include <string.h>

#include "ch32fun.h"
#include "i2c_lowlevel.h"

#define EEPROM_I2C_ADDR 0x50u

static void eeprom_clear_ack_failure(void)
{
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 &= (uint16_t)~I2C_STAR1_AF;
    }
}

static void eeprom_issue_stop(void)
{
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
}

static bool eeprom_read_byte(uint16_t offset, uint8_t *value)
{
    if (!value) {
        return false;
    }
    if (offset >= EEPROM_TOTAL_SIZE_BYTES) {
        return false;
    }

    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)EEPROM_I2C_ADDR << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        eeprom_clear_ack_failure();
        eeprom_issue_stop();
        return false;
    }

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint8_t)(offset >> 8);
    if (!i2c1_wait_flag(0x00070084u)) {
        eeprom_issue_stop();
        return false;
    }

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint8_t)(offset & 0xFFu);
    if (!i2c1_wait_flag(0x00070084u)) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint16_t)(((uint16_t)EEPROM_I2C_ADDR << 1) | 1u);
    if (!i2c1_wait_flag(0x00030002u)) {
        eeprom_clear_ack_failure();
        eeprom_issue_stop();
        return false;
    }

    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    I2C1->CTLR1 |= I2C_CTLR1_STOP;

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_RXNE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        I2C1->CTLR1 |= I2C_CTLR1_ACK;
        return false;
    }

    *value = (uint8_t)I2C1->DATAR;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return true;
}

static bool eeprom_write_page(uint16_t offset, const uint8_t *data, uint16_t length)
{
    if (!data || length == 0u) {
        return false;
    }
    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)EEPROM_I2C_ADDR << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        eeprom_clear_ack_failure();
        eeprom_issue_stop();
        return false;
    }

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint8_t)(offset >> 8);
    if (!i2c1_wait_flag(0x00070084u)) {
        eeprom_issue_stop();
        return false;
    }

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        eeprom_issue_stop();
        return false;
    }

    I2C1->DATAR = (uint8_t)(offset & 0xFFu);
    if (!i2c1_wait_flag(0x00070084u)) {
        eeprom_issue_stop();
        return false;
    }

    for (uint16_t idx = 0; idx < length; ++idx) {
        timeout = 100000;
        while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
        }
        if (timeout <= 0) {
            eeprom_issue_stop();
            return false;
        }

        I2C1->DATAR = data[idx];
        if (!i2c1_wait_flag(0x00070084u)) {
            eeprom_issue_stop();
            return false;
        }
    }

    eeprom_issue_stop();
    return true;
}

bool eeprom_read_bytes(uint16_t offset, uint8_t *data, uint16_t length)
{
    if (!data || length == 0u) {
        return false;
    }
    uint32_t end = (uint32_t)offset + (uint32_t)length;
    if (end > EEPROM_TOTAL_SIZE_BYTES) {
        return false;
    }

    for (uint16_t idx = 0; idx < length; ++idx) {
        if (!eeprom_read_byte((uint16_t)(offset + idx), &data[idx])) {
            return false;
        }
    }
    return true;
}

bool eeprom_write_bytes(uint16_t offset, const uint8_t *data, uint16_t length)
{
    if (!data || length == 0u) {
        return false;
    }
    uint32_t end = (uint32_t)offset + (uint32_t)length;
    if (end > EEPROM_TOTAL_SIZE_BYTES) {
        return false;
    }

    uint16_t remaining = length;
    uint16_t current_offset = offset;
    const uint8_t *current = data;

    while (remaining > 0u) {
        uint16_t page_space = EEPROM_PAGE_SIZE_BYTES - (current_offset % EEPROM_PAGE_SIZE_BYTES);
        uint16_t chunk = (remaining < page_space) ? remaining : page_space;

        if (!eeprom_write_page(current_offset, current, chunk)) {
            return false;
        }
        if (!eeprom_wait_ready(20u)) {
            return false;
        }

        current_offset = (uint16_t)(current_offset + chunk);
        current += chunk;
        remaining = (uint16_t)(remaining - chunk);
    }

    return true;
}

bool eeprom_wait_ready(uint32_t timeout_ms)
{
    uint32_t attempts = (timeout_ms == 0u) ? 1u : timeout_ms;

    while (attempts-- > 0u) {
        if (!i2c1_wait_not_busy()) {
            continue;
        }

        I2C1->CTLR1 |= I2C_CTLR1_START;
        if (!i2c1_wait_flag(0x00030001u)) {
            eeprom_issue_stop();
            continue;
        }

        I2C1->DATAR = (uint16_t)((uint16_t)EEPROM_I2C_ADDR << 1);
        if (i2c1_wait_flag(0x00030002u)) {
            (void)I2C1->STAR1;
            (void)I2C1->STAR2;
            eeprom_issue_stop();
            return true;
        }

        eeprom_clear_ack_failure();
        eeprom_issue_stop();
        Delay_Ms(1u);
    }

    return false;
}

bool font_storage_fetch_rows(uint8_t code, uint8_t *rows)
{
    if (!rows) {
        return false;
    }

    uint32_t start = EEPROM_FONT_BASE + ((uint32_t)code << 3);
    if ((start + 8u) > EEPROM_TOTAL_SIZE_BYTES) {
        memset(rows, 0, 8u);
        return false;
    }

    if (!eeprom_read_bytes((uint16_t)start, rows, 8u)) {
        memset(rows, 0, 8u);
        return false;
    }

    return true;
}

void ssd1306_font_fetch(uint8_t chr, uint8_t *rows)
{
    if (!rows) {
        return;
    }
    if (!font_storage_fetch_rows(chr, rows)) {
        memset(rows, 0, 8u);
    }
}
