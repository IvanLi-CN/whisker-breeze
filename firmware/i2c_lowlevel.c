#include "i2c_lowlevel.h"

#include "ch32fun.h"

void i2c1_configure_speed(uint32_t target_hz)
{
    if (target_hz == 0u) {
        return;
    }

    uint32_t pclk_hz = FUNCONF_SYSTEM_CORE_CLOCK;
    if (pclk_hz == 0u) {
        pclk_hz = 48000000u;
    }

    uint32_t freq_mhz = pclk_hz / 1000000u;
    if (freq_mhz < 2u) {
        freq_mhz = 2u;
    }
    if (freq_mhz > 32u) {
        freq_mhz = 32u;
    }

    I2C1->CTLR1 &= ~I2C_CTLR1_PE;

    I2C1->CTLR2 &= ~I2C_CTLR2_FREQ;
    I2C1->CTLR2 |= (uint16_t)(freq_mhz & I2C_CTLR2_FREQ);

    uint32_t fast_cutoff_hz = 100000u;
    uint32_t ckcfgr = 0u;

    if (target_hz <= fast_cutoff_hz) {
        uint32_t denom = target_hz * 2u;
        uint32_t ccr = (pclk_hz + denom - 1u) / denom;
        if (ccr < 4u) {
            ccr = 4u;
        }
        if (ccr > 0x0FFFu) {
            ccr = 0x0FFFu;
        }
        ckcfgr = ccr & 0x0FFFu;
    } else {
        uint32_t denom = target_hz * 3u;
        uint32_t ccr = (pclk_hz + denom - 1u) / denom;
        if (ccr < 1u) {
            ccr = 1u;
        }
        if (ccr > 0x0FFFu) {
            ccr = 0x0FFFu;
        }
        ckcfgr = I2C_CKCFGR_FS | (ccr & 0x0FFFu);
    }

    I2C1->CKCFGR = (uint16_t)ckcfgr;

    I2C1->CTLR1 |= I2C_CTLR1_PE;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
}

bool i2c1_wait_flag(uint32_t mask)
{
    int32_t timeout = 100000;
    while (timeout-- > 0) {
        uint32_t status = I2C1->STAR1 | ((uint32_t)I2C1->STAR2 << 16);
        if ((status & mask) == mask) {
            return true;
        }
    }
    return false;
}

bool i2c1_wait_not_busy(void)
{
    int32_t timeout = 100000;
    while (timeout-- > 0) {
        if ((I2C1->STAR2 & I2C_STAR2_BUSY) == 0u) {
            return true;
        }
    }
    return false;
}

bool i2c1_read_current_u8(uint8_t addr, uint8_t *value)
{
    if (!value) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    I2C1->CTLR1 |= I2C_CTLR1_START;

    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)(((uint16_t)addr << 1) | 1u);

    if (!i2c1_wait_flag(0x00030002u)) {
        return false;
    }

    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    I2C1->CTLR1 |= I2C_CTLR1_STOP;

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_RXNE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    *value = (uint8_t)I2C1->DATAR;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return true;
}

bool i2c1_write_u8(uint8_t addr, uint8_t reg, uint8_t data)
{
    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)addr << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        return false;
    }

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = reg;
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = data;
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return true;
}

bool i2c1_write_u16(uint8_t addr, uint8_t reg, uint16_t value)
{
    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)addr << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        return false;
    }

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = reg;
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = (uint8_t)(value >> 8);
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = (uint8_t)(value & 0xFFu);
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return true;
}

bool i2c1_read_u16(uint8_t addr, uint8_t reg, uint16_t *value)
{
    if (!value) {
        return false;
    }

    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)addr << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        return false;
    }

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = reg;
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)(((uint16_t)addr << 1) | 1u);
    if (!i2c1_wait_flag(0x00030002u)) {
        return false;
    }

    int32_t btf_timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_BTF) == 0u) && btf_timeout-- > 0) {
    }
    if (btf_timeout <= 0) {
        return false;
    }

    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    I2C1->CTLR1 |= I2C_CTLR1_STOP;

    uint8_t msb = (uint8_t)I2C1->DATAR;
    uint8_t lsb = (uint8_t)I2C1->DATAR;

    I2C1->CTLR1 |= I2C_CTLR1_ACK;

    *value = ((uint16_t)msb << 8) | (uint16_t)lsb;
    return true;
}

bool i2c1_read_u8(uint8_t addr, uint8_t reg, uint8_t *value)
{
    if (!value) {
        return false;
    }

    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)addr << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        return false;
    }

    int32_t timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = reg;
    if (!i2c1_wait_flag(0x00070084u)) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }

    I2C1->DATAR = (uint16_t)(((uint16_t)addr << 1) | 1u);
    if (!i2c1_wait_flag(0x00030002u)) {
        return false;
    }

    I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
    I2C1->CTLR1 |= I2C_CTLR1_STOP;

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_RXNE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    *value = (uint8_t)I2C1->DATAR;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    return true;
}
