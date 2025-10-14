#include "eeprom_config.h"

#include <string.h>

#include "eeprom_font_storage.h" // reuse low-level I2C EEPROM helpers

#define CFG_BASE_OFFSET       0x0000u
#define CFG_BLOCK_SIZE        32u
#define CFG_MAGIC             0x5742u /* 'W''B' */
#define CFG_VERSION           0x02u

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint8_t  last_mode;              // 1 TEMP, 2 MANUAL
    uint16_t manual_ratio_q8_8;      // 0..256
    int16_t  temp_band_offset_cx100; // signed
    uint16_t checksum16;
} cfg_blob_v1_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;                // 0x02
    uint8_t  flags;                  // reserved
    uint8_t  last_mode;              // 1 TEMP, 2 MANUAL
    uint16_t manual_ratio_q8_8;      // 0..256
    int16_t  temp_band_offset_cx100; // legacy compatibility
    int16_t  auto_min_cx100;         // -2000..6000
    int16_t  auto_max_cx100;         // -2000..6000
    uint8_t  display_sleep_s;        // 5..60
    uint8_t  reserved0;              // align to even size for checksum
    uint16_t checksum16;
} cfg_blob_v2_t;

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
    /* First read the version field by fetching the minimum header. */
    uint8_t header[4];
    if (!eeprom_read_bytes(CFG_BASE_OFFSET + 0, header, sizeof header)) {
        return false;
    }
    uint16_t magic = (uint16_t)((uint16_t)header[1] << 8) | header[0];
    uint8_t version = header[2];
    if (magic != CFG_MAGIC) {
        return false;
    }

    if (version == 0x01u) {
        cfg_blob_v1_t b1;
        memset(&b1, 0, sizeof b1);
        if (!eeprom_read_bytes(CFG_BASE_OFFSET, (uint8_t *)&b1, sizeof b1)) {
            return false;
        }
        uint16_t calc = checksum16_sum((const uint8_t *)&b1, (uint16_t)(sizeof(b1) - sizeof(b1.checksum16)));
        if (calc != b1.checksum16) {
            return false;
        }
        out->version = b1.version;
        out->last_mode = (b1.last_mode == CFG_MODE_MANUAL) ? CFG_MODE_MANUAL : CFG_MODE_TEMP;
        out->manual_ratio_q8_8 = b1.manual_ratio_q8_8;
        out->temp_band_offset_cx100 = b1.temp_band_offset_cx100;
        /* Map legacy band offset to absolute min/max band centered on [20,40]°C. */
        int32_t min_c = 2000 + (int32_t)out->temp_band_offset_cx100;
        int32_t max_c = 4000 + (int32_t)out->temp_band_offset_cx100;
        if (min_c < -2000) min_c = -2000;
        if (min_c > 6000)  min_c = 6000;
        if (max_c < -2000) max_c = -2000;
        if (max_c > 6000)  max_c = 6000;
        out->auto_min_cx100 = (int16_t)min_c;
        out->auto_max_cx100 = (int16_t)max_c;
        out->display_sleep_s = 30u; /* default */
        return true;
    }

    if (version == 0x02u) {
        cfg_blob_v2_t b2;
        memset(&b2, 0, sizeof b2);
        if (!eeprom_read_bytes(CFG_BASE_OFFSET, (uint8_t *)&b2, sizeof b2)) {
            return false;
        }
        uint16_t calc = checksum16_sum((const uint8_t *)&b2, (uint16_t)(sizeof(b2) - sizeof(b2.checksum16)));
        if (calc != b2.checksum16) {
            return false;
        }
        out->version = b2.version;
        out->last_mode = (b2.last_mode == CFG_MODE_MANUAL) ? CFG_MODE_MANUAL : CFG_MODE_TEMP;
        out->manual_ratio_q8_8 = b2.manual_ratio_q8_8;
        out->temp_band_offset_cx100 = b2.temp_band_offset_cx100;
        out->auto_min_cx100 = b2.auto_min_cx100;
        out->auto_max_cx100 = b2.auto_max_cx100;
        out->display_sleep_s = b2.display_sleep_s;
        return true;
    }

    return false;
}

bool eeprom_cfg_save(const eeprom_runtime_cfg_t *in)
{
    if (!in) return false;
    cfg_blob_v2_t blob;
    memset(&blob, 0, sizeof blob);
    blob.magic = CFG_MAGIC;
    blob.version = CFG_VERSION;
    blob.flags = 0u;
    blob.last_mode = (uint8_t)((in->last_mode == CFG_MODE_MANUAL) ? CFG_MODE_MANUAL : CFG_MODE_TEMP);
    blob.manual_ratio_q8_8 = in->manual_ratio_q8_8;
    blob.temp_band_offset_cx100 = in->temp_band_offset_cx100;
    blob.auto_min_cx100 = in->auto_min_cx100;
    blob.auto_max_cx100 = in->auto_max_cx100;
    uint8_t sleep_s = in->display_sleep_s;
    if (sleep_s < 5u) sleep_s = 5u;
    if (sleep_s > 60u) sleep_s = 60u;
    blob.display_sleep_s = sleep_s;
    blob.reserved0 = 0u;
    blob.checksum16 = checksum16_sum((const uint8_t *)&blob, (uint16_t)(sizeof(blob) - sizeof(blob.checksum16)));

    if (!eeprom_write_bytes(CFG_BASE_OFFSET, (const uint8_t *)&blob, sizeof blob)) {
        return false;
    }
    return true;
}
