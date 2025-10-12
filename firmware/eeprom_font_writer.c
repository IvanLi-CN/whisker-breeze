#include "ch32fun.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eeprom_font_storage.h"
#include "i2c_lowlevel.h"

#include "../ch32fun/extralibs/ssd1306_i2c.h"
#include "../ch32fun/extralibs/font_8x8.h"

#define FONT_WRITER_I2C_HZ 400000u

static void init_i2c(void)
{
    funGpioInitAll();
    ssd1306_i2c_init();
    i2c1_configure_speed(FONT_WRITER_I2C_HZ);
}

static void bus_stop(void)
{
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
}

static void clear_ack_failure(void)
{
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 &= (uint16_t)~I2C_STAR1_AF;
    }
}

static bool i2c_device_present(uint8_t addr)
{
    if (!i2c1_wait_not_busy()) {
        return false;
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        bus_stop();
        return false;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)addr << 1);
    if (!i2c1_wait_flag(0x00030002u)) {
        clear_ack_failure();
        bus_stop();
        return false;
    }

    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
    bus_stop();
    return true;
}

static void scan_bus(void)
{
    printf("I2C scan start\r\n");
    for (uint8_t addr = 0x08u; addr < 0x78u; ++addr) {
        if (i2c_device_present(addr)) {
            printf(" - found 0x%02X\r\n", addr);
        }
    }
    printf("I2C scan done\r\n");
}

static bool verify_font(void)
{
    uint8_t buffer[EEPROM_PAGE_SIZE_BYTES];
    const uint16_t font_len = (uint16_t)sizeof(fontdata);
    for (uint16_t offset = 0; offset < font_len; offset += EEPROM_PAGE_SIZE_BYTES) {
        uint16_t chunk = EEPROM_PAGE_SIZE_BYTES;
        if ((uint32_t)offset + chunk > font_len) {
            chunk = (uint16_t)(font_len - offset);
        }
        if (!eeprom_read_bytes((uint16_t)(EEPROM_FONT_BASE + offset), buffer, chunk)) {
            return false;
        }
        if (memcmp(buffer, &fontdata[offset], chunk) != 0) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    SystemInit();
    init_i2c();
    scan_bus();

    const uint16_t font_len = (uint16_t)sizeof(fontdata);
    if (font_len != EEPROM_FONT_LENGTH) {
        printf("font size mismatch (%u vs %u)\r\n", font_len, (unsigned)EEPROM_FONT_LENGTH);
        while (1) {
            Delay_Ms(1000);
        }
    }

    if (!eeprom_wait_ready(50u)) {
        printf("EEPROM not responding\r\n");
        while (1) {
            Delay_Ms(1000);
        }
    }

    if (!eeprom_write_bytes(EEPROM_FONT_BASE, fontdata, font_len)) {
        printf("EEPROM write failed\r\n");
        while (1) {
            Delay_Ms(1000);
        }
    }

    if (!verify_font()) {
        printf("EEPROM verify failed\r\n");
        while (1) {
            Delay_Ms(1000);
        }
    }

    printf("EEPROM font programmed (%u bytes)\r\n", font_len);
    while (1) {
        Delay_Ms(1000);
    }
}
