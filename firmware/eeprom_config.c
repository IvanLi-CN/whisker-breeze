#include "eeprom_config.h"

#include <string.h>

#include "eeprom_font_storage.h" // reuse low-level I2C EEPROM helpers

#define CFG_BASE_OFFSET       0x0000u
#define CFG_BLOCK_SIZE        16u
#define CFG_MAGIC             0x5742u /* 'W''B' */
#define CFG_VERSION           0x01u

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint8_t  last_mode;              // 1 TEMP, 2 MANUAL
    uint16_t manual_ratio_q8_8;      // 0..256
    int16_t  temp_band_offset_cx100; // signed
    uint16_t checksum16;
} cfg_blob_v1_t;

static uint16_t checksum16_sum(const uint8_t *data, uint16_t len)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return (uint16_t)sum;
}

bool eeprom_cfg_load(eeprom_runtime_cfg_t *out)
{
    if (!out) return false;
    cfg_blob_v1_t blob;
    memset(&blob, 0, sizeof blob);
    if (!eeprom_read_bytes(CFG_BASE_OFFSET, (uint8_t *)&blob, sizeof blob)) {
        return false;
    }
    if (blob.magic != CFG_MAGIC || blob.version != CFG_VERSION) {
        return false;
    }
    uint16_t calc = checksum16_sum((const uint8_t *)&blob, (uint16_t)(sizeof(blob) - sizeof(blob.checksum16)));
    if (calc != blob.checksum16) {
        return false;
    }
    out->version = blob.version;
    out->last_mode = (blob.last_mode == CFG_MODE_MANUAL) ? CFG_MODE_MANUAL : CFG_MODE_TEMP;
    out->manual_ratio_q8_8 = blob.manual_ratio_q8_8;
    out->temp_band_offset_cx100 = blob.temp_band_offset_cx100;
    return true;
}

bool eeprom_cfg_save(const eeprom_runtime_cfg_t *in)
{
    if (!in) return false;
    cfg_blob_v1_t blob;
    memset(&blob, 0, sizeof blob);
    blob.magic = CFG_MAGIC;
    blob.version = CFG_VERSION;
    blob.flags = 0u;
    blob.last_mode = (uint8_t)((in->last_mode == CFG_MODE_MANUAL) ? CFG_MODE_MANUAL : CFG_MODE_TEMP);
    blob.manual_ratio_q8_8 = in->manual_ratio_q8_8;
    blob.temp_band_offset_cx100 = in->temp_band_offset_cx100;
    blob.checksum16 = checksum16_sum((const uint8_t *)&blob, (uint16_t)(sizeof(blob) - sizeof(blob.checksum16)));

    if (!eeprom_write_bytes(CFG_BASE_OFFSET, (const uint8_t *)&blob, sizeof blob)) {
        return false;
    }
    return true;
}

