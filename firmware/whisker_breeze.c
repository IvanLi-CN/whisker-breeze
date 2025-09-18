#include "ch32fun.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define SSD1306_128X64

static bool g_display_error_seen = false;

static int ssd1306_quiet_printf(const char *fmt, ...)
{
    (void)fmt;
    va_list args;
    va_start(args, fmt);
    va_end(args);
    g_display_error_seen = true;
    return 0;
}

#define printf(...) ssd1306_quiet_printf(__VA_ARGS__)
#include "../ch32fun/extralibs/ssd1306_i2c.h"
#undef printf
#include "../ch32fun/extralibs/ssd1306.h"

extern int printf(const char *fmt, ...);
int mini_snprintf(char *buffer, unsigned int buffer_len, const char *fmt, ...);

/* -------------------------------------------------------------------------- */
/* Hardware bindings                                                          */
/* -------------------------------------------------------------------------- */
#define PIN_DISPLAY_PWR       PA1
#define PIN_DISPLAY_RESET     PD0
#define PIN_I2C_SDA           PC1
#define PIN_I2C_SCL           PC2
#define PIN_FAN_PWM           PC3
#define PIN_FAN_ENABLE        PC5
#define PIN_FAN_TACH          PC4
#define PIN_POWER_GOOD        PC7
#define PIN_INA_INT           PC0
#define PIN_MODE_FAST         PD2
#define PIN_MODE_SELECT       PD3
#define PIN_MODE_SLOW         PD4

/* -------------------------------------------------------------------------- */
/* Control constants                                                          */
/* -------------------------------------------------------------------------- */
#define FAN_PWM_PERIOD_TICKS      2400u      /* 48 MHz / 20 kHz */
#define FAN_PWM_MIN_DUTY          0.10f
#define FAN_PWM_MAX_DUTY          1.00f
#define FAN_PWM_RAMP_STEP         0.02f
#define FAN_ADJUST_RATE_PER_MS    0.0005f
#define FAN_SOFT_START_MS         100u
#define FAN_TACH_TIMEOUT_MS       500u
#define FAN_TACH_TIMER_PRESCALER  479u      /* 48 MHz / (479+1) = 100 kHz */
#define FAN_TACH_TIMER_CLOCK_HZ   (48000000u / (FAN_TACH_TIMER_PRESCALER + 1u))
#define FAN_TACH_PULSES_PER_REV    2u
#define FAN_TACH_MAX_RPM           6000.0f
#define FAN_STALL_RPM_THRESHOLD    50.0f
#define FAN_POWER_ON_DELAY_MS       1u
#define FAN_DEFAULT_MIN_RPM         100.0f
#define FAN_DEFAULT_MAX_RPM         20000.0f
#define FAN_DEFAULT_MIN_RATIO       (FAN_DEFAULT_MIN_RPM / FAN_DEFAULT_MAX_RPM)
#define FAN_RPM_GLITCH_MIN_BASE     300.0f
#define FAN_RPM_GLITCH_UPPER_RPM    4500.0f
#define INA226_12V_RAW_THRESHOLD    9200u   /* 11.5 V / 1.25 mV */
#define FAN_CALIBRATION_STABLE_MS   500u
#define FAN_CALIBRATION_MIN_MS      2000u
#define FAN_CALIBRATION_DELTA_RPM   25.0f

#ifndef INA226_I2C_ADDR
#define INA226_I2C_ADDR           0x40u
#endif
#ifndef INA226_SHUNT_OHMS
#define INA226_SHUNT_OHMS         0.010f  /* 10 mΩ shunt on Whisker Breeze rev */
#endif
#ifndef INA226_MAX_EXPECTED_CURRENT_A
#define INA226_MAX_EXPECTED_CURRENT_A 2.0f
#endif
#define INA226_CURRENT_LSB        (INA226_MAX_EXPECTED_CURRENT_A / 32768.0f)
#define INA226_POWER_LSB          (25.0f * INA226_CURRENT_LSB)
#define INA226_POLL_INTERVAL_MS   200u

#define INA226_REG_CONFIG       0x00u
#define INA226_REG_SHUNT        0x01u
#define INA226_REG_BUS          0x02u
#define INA226_REG_POWER        0x03u
#define INA226_REG_CURRENT      0x04u
#define INA226_REG_CALIBRATION  0x05u
#define INA226_REG_MASK_ENABLE  0x06u
#define INA226_ADDR_MIN         0x40u
#define INA226_ADDR_MAX         0x4Fu
#define INA226_CONFIG_CONTINUOUS 0x4127u

#define INA226_ERROR_LOG_INTERVAL_MS 1000u

#define I2C1_SHARED_BUS_TARGET_HZ 400000u

#define LOG_SAMPLE_PERIOD_MS      1000u
#define LOG_FORCE_INTERVAL_MS     5000u
#define LOG_DELTA_TARGET          0.02f
#define LOG_DELTA_DUTY            0.02f
#define LOG_DELTA_RPM             50.0f

#define MODE_DEBOUNCE_TICKS       3u
#define LOOP_PERIOD_MS            10u

#define INA_INT_ACTIVE_LOW        1
#define POWER_GOOD_ACTIVE_LOW     1

#define CH224_I2C_ADDR            0x23
#define CH224_REG_STATUS          0x09
#define CH224_REG_VSEL            0x0A
#define CH224_REG_CUR_STATUS      0x0B
#define CH224_VSEL_5V             0x00
#define CH224_VSEL_9V             0x01
#define CH224_VSEL_12V            0x02
#define CH224_STATUS_PD_ACT       (1u << 3)

#define CH224_POLL_INTERVAL_MS    50u

/* -------------------------------------------------------------------------- */
/* Type declarations                                                          */
/* -------------------------------------------------------------------------- */
typedef struct {
    bool stable_state;
    uint8_t counter;
} debounce_t;

typedef struct {
    debounce_t decrease_input; /* SLOW  → 减速 */
    debounce_t increase_input; /* FAST  → 加速 */
    debounce_t hold_input;     /* MODE  → 保持/预留 */
} controls_state_t;

typedef enum {
    FAN_PHASE_SOFT_START = 0,
    FAN_PHASE_RUN
} fan_phase_t;

typedef struct {
    fan_phase_t phase;
    float target_duty;
    float current_duty;
    float rpm;
    float rpm_max;
    bool rpm_valid;
    uint32_t soft_timer_ms;
    bool power_enabled;
    uint32_t power_settle_ms;
} fan_state_t;

typedef struct {
    bool vbus_valid;
    bool ina_alert;
} power_state_t;

typedef struct {
    bool configured;
    bool valid;
    bool online_announced;
    float bus_voltage_v;
    float shunt_voltage_v;
    float current_a;
    float power_w;
    uint32_t poll_timer_ms;
    uint32_t config_failures;
    uint32_t sample_failures;
    uint32_t last_error_report_ms;
    uint8_t address;
    bool address_valid;
    uint16_t raw_bus_reg;
} ina226_state_t;

typedef struct {
    bool present;
    bool pd_active;
    bool have_12v;
    uint8_t last_status;
    uint32_t poll_timer_ms;
} pd_state_t;

typedef struct {
    volatile uint16_t last_capture;
    volatile uint16_t period_ticks;
    volatile uint8_t sample_ready;
    volatile uint8_t capture_valid;
} tach_state_t;

typedef struct {
    uint32_t sample_accum;
    uint32_t since_last_log;
    bool have_last;
    fan_phase_t last_phase;
    float last_target;
    float last_duty;
    float last_rpm;
    bool last_rpm_valid;
    bool last_vbus;
    bool last_pd;
    bool last_pd_present;
    bool last_12v;
    uint8_t last_status;
} log_state_t;

typedef struct {
    bool active;
    bool completed;
    float peak_rpm;
    uint32_t last_improve_ms;
    uint32_t start_ms;
} fan_calibration_state_t;

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */
static controls_state_t g_controls = {
    .decrease_input = { .stable_state = false, .counter = 0 },
    .increase_input = { .stable_state = false, .counter = 0 },
    .hold_input = { .stable_state = false, .counter = 0 },
};

static uint8_t g_controls_raw_mask = 0u;

static float g_manual_target = 1.0f;

static fan_state_t g_fan = {
    .phase = FAN_PHASE_SOFT_START,
    .target_duty = 0.0f,
    .current_duty = 0.0f,
    .rpm = 0.0f,
    .rpm_max = 0.0f,
    .rpm_valid = false,
    .soft_timer_ms = 0,
    .power_enabled = false,
    .power_settle_ms = 0,
};

static float g_fan_min_ratio = FAN_DEFAULT_MIN_RATIO;

static power_state_t g_power = {
    .vbus_valid = false,
    .ina_alert = false,
};

static ina226_state_t g_ina = {
    .configured = false,
    .valid = false,
    .online_announced = false,
    .bus_voltage_v = 0.0f,
    .shunt_voltage_v = 0.0f,
    .current_a = 0.0f,
    .power_w = 0.0f,
    .poll_timer_ms = 0,
    .config_failures = 0,
    .sample_failures = 0,
    .last_error_report_ms = 0,
    .address = INA226_I2C_ADDR,
    .address_valid = false,
    .raw_bus_reg = 0,
};

static pd_state_t g_pd = {
    .present = false,
    .pd_active = false,
    .have_12v = false,
    .last_status = 0,
    .poll_timer_ms = 0,
};

static tach_state_t g_tach = {
    .last_capture = 0,
    .period_ticks = 0,
    .sample_ready = 0,
    .capture_valid = 0,
};

static log_state_t g_log = {
    .sample_accum = 0,
    .since_last_log = 1000,
    .have_last = false,
    .last_phase = FAN_PHASE_SOFT_START,
    .last_target = 0.0f,
    .last_duty = 0.0f,
    .last_rpm = 0.0f,
    .last_rpm_valid = false,
    .last_vbus = false,
    .last_pd = false,
    .last_pd_present = false,
    .last_12v = false,
    .last_status = 0,
};

static fan_calibration_state_t g_fan_calibration = {
    .active = true,
    .completed = false,
    .peak_rpm = 0.0f,
    .last_improve_ms = 0u,
    .start_ms = 0u,
};

static void fan_calibration_reset(void)
{
    g_fan_calibration.active = true;
    g_fan_calibration.completed = false;
    g_fan_calibration.peak_rpm = 0.0f;
    g_fan_calibration.last_improve_ms = 0u;
    g_fan_calibration.start_ms = 0u;
    g_manual_target = 1.0f;
}

static uint32_t g_i2c_scan_last_ms = 0u;

static bool g_display_initialized = false;
static bool g_display_probe_attempted = false;
static bool g_display_unavailable = false;
static bool g_display_disabled_logged = false;
static uint32_t g_uptime_ms = 0;

/* -------------------------------------------------------------------------- */
/* Utility helpers                                                            */
/* -------------------------------------------------------------------------- */
static float fabsf_local(float x)
{
    return (x >= 0.0f) ? x : -x;
}

static float clampf(float value, float min_val, float max_val)
{
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

static int32_t round_to_i32(float value)
{
    return (int32_t)(value + ((value >= 0.0f) ? 0.5f : -0.5f));
}

static uint16_t percent_from_unit(float value)
{
    float scaled = value * 100.0f;
    scaled = clampf(scaled, 0.0f, 100.0f);
    return (uint16_t)round_to_i32(scaled);
}

static uint16_t rpm_from_float(float rpm)
{
    float clamped = clampf(rpm, 0.0f, FAN_TACH_MAX_RPM);
    return (uint16_t)round_to_i32(clamped);
}

static void reset_fan_log_timer(void)
{
    g_log.sample_accum = 0;
    g_log.since_last_log = LOG_FORCE_INTERVAL_MS;
}

/* -------------------------------------------------------------------------- */
/* Logging helpers                                                            */
/* -------------------------------------------------------------------------- */
static void emit_log(const char *fmt, ...)
{
    char buffer[160];
    va_list args;
    va_start(args, fmt);
    int written = mini_vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    if (written >= (int)sizeof buffer) {
        written = (int)sizeof buffer - 1;
        buffer[written] = '\0';
    }

    buffer[written] = '\0';

    char line[200];
    uint32_t seconds = g_uptime_ms / 1000u;
    uint32_t millis = g_uptime_ms % 1000u;
    mini_snprintf(line, sizeof line, "[%05lu.%03lu] %s",
                  (unsigned long)seconds,
                  (unsigned long)millis,
                  buffer);

    while (!DebugPrintfBufferFree()) {
        poll_input();
    }
    printf("%s\r\n", line);
    while (!DebugPrintfBufferFree()) {
        poll_input();
    }
}

static void log_pd_snapshot(const char *reason)
{
    emit_log("[pd]%s v=%d 12=%d",
             reason,
             g_power.vbus_valid ? 1 : 0,
             g_pd.have_12v ? 1 : 0);
}

static void log_fan_snapshot(void)
{
    uint16_t target_pct = percent_from_unit(g_manual_target);
    uint16_t duty_pct = percent_from_unit(g_fan.current_duty);
    uint16_t rpm_now = rpm_from_float(g_fan.rpm);
    uint16_t bus_raw = g_ina.valid ? g_ina.raw_bus_reg : 0u;
    float current_ma = g_ina.current_a * 1000.0f;
    uint16_t compare_ticks = TIM1->CH3CVR;
    uint8_t btn_raw_mask = g_controls_raw_mask;
    uint8_t btn_stable_mask = 0u;

    if (g_controls.decrease_input.stable_state) {
        btn_stable_mask |= 0x1u;
    }
    if (g_controls.hold_input.stable_state) {
        btn_stable_mask |= 0x2u;
    }
    if (g_controls.increase_input.stable_state) {
        btn_stable_mask |= 0x4u;
    }

    emit_log("[fan] p=%d t=%u d=%u r=%u v=%d 12=%d b=%04X i=%ld c=%u k=%u/%u",
             g_fan.phase,
             (unsigned)target_pct,
             (unsigned)duty_pct,
             (unsigned)rpm_now,
             g_power.vbus_valid ? 1 : 0,
             g_pd.have_12v ? 1 : 0,
             (unsigned)bus_raw,
             (long)current_ma,
             (unsigned)compare_ticks,
             (unsigned)btn_raw_mask,
             (unsigned)btn_stable_mask);
}

/* -------------------------------------------------------------------------- */
/* Debounce helpers                                                           */
/* -------------------------------------------------------------------------- */
static bool debounce_update(debounce_t *db, bool raw_level)
{
    if (raw_level == db->stable_state) {
        db->counter = 0;
        return false;
    }

    if (db->counter < MODE_DEBOUNCE_TICKS) {
        db->counter++;
        if (db->counter >= MODE_DEBOUNCE_TICKS) {
            db->stable_state = raw_level;
            db->counter = 0;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* I2C helpers (blocking)                                                     */
/* -------------------------------------------------------------------------- */
static void i2c1_configure_speed(uint32_t target_hz)
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

static bool i2c1_wait_flag(uint32_t mask)
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

static bool i2c1_wait_not_busy(void)
{
    int32_t timeout = 100000;
    while (timeout-- > 0) {
        if ((I2C1->STAR2 & I2C_STAR2_BUSY) == 0u) {
            return true;
        }
    }
    return false;
}

static bool i2c1_ping(uint8_t addr)
{
    if (!i2c1_wait_not_busy()) {
        return false;
    }

    bool ack = false;

    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        goto cleanup;
    }

    I2C1->DATAR = (uint16_t)((uint16_t)addr << 1);
    if (!i2c1_wait_flag(0x00070082u)) {
        goto cleanup;
    }

    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
    ack = true;

cleanup:
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    if ((I2C1->STAR1 & I2C_STAR1_AF) != 0u) {
        I2C1->STAR1 &= (uint16_t)~I2C_STAR1_AF;
    }
    return ack;
}

static void i2c1_scan_bus(bool force)
{
    uint32_t now = g_uptime_ms;
    if (!force && g_i2c_scan_last_ms != 0u) {
        uint32_t elapsed = now - g_i2c_scan_last_ms;
        if (elapsed < INA226_ERROR_LOG_INTERVAL_MS) {
            return;
        }
    }

    g_i2c_scan_last_ms = (now == 0u) ? 1u : now;

    emit_log("[i2c]scan");
    uint32_t found = 0u;
    for (uint8_t addr = 0x03u; addr <= 0x77u; addr++) {
        if (!i2c1_ping(addr)) {
            continue;
        }
        found++;
        emit_log("[i2c]%02X", (unsigned)addr);
    }

    if (found == 0u) {
        emit_log("[i2c]0");
    }

    emit_log("[i2c]done%lu", (unsigned long)found);
}

static bool i2c1_read_current_u8(uint8_t addr, uint8_t *value)
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

static bool i2c1_write_u8(uint8_t addr, uint8_t reg, uint8_t data)
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

static bool i2c1_write_u16(uint8_t addr, uint8_t reg, uint16_t value)
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

    timeout = 100000;
    while (((I2C1->STAR1 & I2C_STAR1_TXE) == 0u) && timeout-- > 0) {
    }
    if (timeout <= 0) {
        return false;
    }

    I2C1->DATAR = (uint8_t)(value >> 8);

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

static bool i2c1_read_u16(uint8_t addr, uint8_t reg, uint16_t *value)
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

static uint16_t ina226_compute_calibration(void)
{
    float denom = INA226_CURRENT_LSB * INA226_SHUNT_OHMS;
    if (denom <= 0.0f) {
        return 0u;
    }
    float cal = 0.00512f / denom;
    if (cal < 0.0f) {
        cal = 0.0f;
    } else if (cal > 65535.0f) {
        cal = 65535.0f;
    }
    return (uint16_t)round_to_i32(cal);
}

static bool ina226_write_register(uint8_t reg, uint16_t value)
{
    if (!g_ina.address_valid) {
        return false;
    }
    return i2c1_write_u16(g_ina.address, reg, value);
}

static bool ina226_read_register(uint8_t reg, uint16_t *value)
{
    if (!g_ina.address_valid) {
        return false;
    }
    return i2c1_read_u16(g_ina.address, reg, value);
}

static void ina226_report_error(const char *stage)
{
    uint32_t now = g_uptime_ms;
    if (g_ina.last_error_report_ms == 0u ||
        (now - g_ina.last_error_report_ms) >= INA226_ERROR_LOG_INTERVAL_MS) {
        uint8_t addr = g_ina.address_valid ? g_ina.address : INA226_I2C_ADDR;
        emit_log("[ina]%s fail 0x%02X", stage, (unsigned)addr);
        g_ina.last_error_report_ms = (now == 0u) ? 1u : now;
    }
}

static bool ina226_detect_address(void)
{
    if (g_ina.address_valid) {
        return true;
    }

    for (uint8_t addr = INA226_ADDR_MIN; addr <= INA226_ADDR_MAX; ++addr) {
        if (!i2c1_ping(addr)) {
            continue;
        }

        g_ina.address = addr;
        g_ina.address_valid = true;
        emit_log("[ina]0x%02X", (unsigned)addr);
        return true;
    }

    return false;
}

static bool ina226_configure(void)
{
    uint16_t calibration = ina226_compute_calibration();
    if (calibration == 0u) {
        return false;
    }
    if (!ina226_write_register(INA226_REG_CONFIG, INA226_CONFIG_CONTINUOUS)) {
        return false;
    }
    if (!ina226_write_register(INA226_REG_CALIBRATION, calibration)) {
        return false;
    }
    (void)ina226_write_register(INA226_REG_MASK_ENABLE, 0u);
    g_ina.last_error_report_ms = 0u;
    return true;
}

static void ina226_update(uint32_t delta_ms)
{
    if (!g_ina.address_valid) {
        if (!ina226_detect_address()) {
            g_ina.valid = false;
            g_ina.online_announced = false;
            g_ina.configured = false;
            if (g_ina.config_failures < UINT32_MAX) {
                g_ina.config_failures++;
            }
            g_ina.raw_bus_reg = 0u;
            ina226_report_error("detect");
            i2c1_scan_bus(false);
            return;
        }
    }

    if (!g_ina.configured) {
        if (!ina226_configure()) {
            g_ina.valid = false;
            g_ina.online_announced = false;
            g_ina.address_valid = false;
            if (g_ina.config_failures < UINT32_MAX) {
                g_ina.config_failures++;
            }
            g_ina.raw_bus_reg = 0u;
            ina226_report_error("configure");
            i2c1_scan_bus(false);
            return;
        }
        g_ina.configured = true;
        g_ina.poll_timer_ms = 0u;
    }

    g_ina.poll_timer_ms += delta_ms;
    if (g_ina.poll_timer_ms < INA226_POLL_INTERVAL_MS) {
        return;
    }
    g_ina.poll_timer_ms = 0u;

    uint16_t raw_bus = 0u;
    uint16_t raw_shunt = 0u;
    uint16_t raw_current = 0u;
    uint16_t raw_power = 0u;

    if (!ina226_read_register(INA226_REG_BUS, &raw_bus) ||
        !ina226_read_register(INA226_REG_SHUNT, &raw_shunt) ||
        !ina226_read_register(INA226_REG_CURRENT, &raw_current) ||
        !ina226_read_register(INA226_REG_POWER, &raw_power)) {
        g_ina.valid = false;
        g_ina.online_announced = false;
        g_ina.configured = false;
        g_ina.address_valid = false;
        g_ina.bus_voltage_v = 0.0f;
        g_ina.shunt_voltage_v = 0.0f;
        g_ina.current_a = 0.0f;
        g_ina.power_w = 0.0f;
        g_ina.raw_bus_reg = 0u;
        if (g_ina.sample_failures < UINT32_MAX) {
            g_ina.sample_failures++;
        }
        ina226_report_error("sample");
        i2c1_scan_bus(false);
        return;
    }

    g_ina.raw_bus_reg = raw_bus;
    g_ina.bus_voltage_v = (float)raw_bus * 0.00125f;
    g_ina.shunt_voltage_v = (float)((int16_t)raw_shunt) * 0.0000025f;
    g_ina.current_a = (float)((int16_t)raw_current) * INA226_CURRENT_LSB;
    g_ina.power_w = (float)raw_power * INA226_POWER_LSB;
    g_ina.valid = true;
    if (!g_ina.online_announced) {
        long bus_whole = (long)g_ina.bus_voltage_v;
        long bus_frac = (long)((g_ina.bus_voltage_v - (float)bus_whole) * 100.0f);
        emit_log("[ina]%ld.%02ldV", bus_whole, bus_frac);
        g_ina.online_announced = true;
        g_ina.last_error_report_ms = 0u;
    }
}

static bool i2c1_read_u8(uint8_t addr, uint8_t reg, uint8_t *value)
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

    return i2c1_read_current_u8(addr, value);
}

/* -------------------------------------------------------------------------- */
/* Display helpers                                                            */
/* -------------------------------------------------------------------------- */
static void display_power(bool enable)
{
    funDigitalWrite(PIN_DISPLAY_PWR, enable ? FUN_LOW : FUN_HIGH);
    funDigitalWrite(PIN_DISPLAY_RESET, enable ? FUN_LOW : FUN_HIGH);
    if (enable) {
        Delay_Ms(5);
        funDigitalWrite(PIN_DISPLAY_RESET, FUN_HIGH);
    }
}

static void display_render(void)
{
    if (!g_display_initialized) {
        return;
    }

    char line[17];
    ssd1306_setbuf(0);

    snprintf(line, sizeof line, "Set   %3.0f%%", g_manual_target * 100.0f);
    ssd1306_drawstr(0, 0, line, 1);

    snprintf(line, sizeof line, "Out   %3.0f%%", g_fan.current_duty * 100.0f);
    ssd1306_drawstr(0, 8, line, 1);

    snprintf(line, sizeof line, "RPM   %4.0f", g_fan.rpm);
    ssd1306_drawstr(0, 16, line, 1);

    snprintf(line, sizeof line, "Peak  %4.0f", g_fan.rpm_max);
    ssd1306_drawstr(0, 24, line, 1);

    snprintf(line, sizeof line, "PD %s 12V %s",
             g_pd.present ? "OK" : "--",
             g_pd.have_12v ? "OK" : "--");
    ssd1306_drawstr(0, 40, line, 1);

    ssd1306_refresh();
}

static void display_try_init(void)
{
    if (g_display_initialized) {
        return;
    }

    if (g_display_unavailable) {
        if (!g_display_disabled_logged) {
            emit_log("[disp] skip (disabled)");
            g_display_disabled_logged = true;
        }
        return;
    }

    if (!g_display_probe_attempted) {
        emit_log("[disp] probe");
    }

    g_display_probe_attempted = true;
    display_power(true);
    Delay_Ms(30);

    g_display_error_seen = false;
    if (ssd1306_i2c_init() == 0) {
        const uint8_t probe_cmd = 0xAE; /* Display OFF */
        int probe_result = ssd1306_pkt_send(&probe_cmd, 1, 1);
        if (!g_display_error_seen && probe_result == 0) {
            ssd1306_init();
            ssd1306_setbuf(0);
            g_display_initialized = true;
            g_display_disabled_logged = false;
            emit_log("[disp] ok");
            display_render();
            return;
        }
    }

    g_display_initialized = false;
    g_display_unavailable = true;
    display_power(false);
    g_display_disabled_logged = true;
    if (g_display_error_seen) {
        emit_log("[disp] SSD1306 timeout, disabled");
    } else {
        emit_log("[disp] SSD1306 missing, disabled");
    }
}

/* -------------------------------------------------------------------------- */
/* PWM helpers                                                                */
/* -------------------------------------------------------------------------- */
static void pwm_set_duty(float duty)
{
    duty = clampf(duty, 0.0f, 1.0f);
    g_fan.current_duty = duty;
    uint16_t compare = (uint16_t)round_to_i32(((float)(FAN_PWM_PERIOD_TICKS - 1u)) * duty);
    TIM1->CH3CVR = compare;
}

static void fan_apply_pwm(float duty, uint32_t delta_ms)
{
    duty = clampf(duty, 0.0f, 1.0f);

    if (duty <= 0.0f) {
        funDigitalWrite(PIN_FAN_ENABLE, FUN_LOW);
        g_fan.power_enabled = false;
        g_fan.power_settle_ms = 0u;
        pwm_set_duty(0.0f);
        return;
    }

    if (!g_fan.power_enabled) {
        funDigitalWrite(PIN_FAN_ENABLE, FUN_HIGH);
        g_fan.power_enabled = true;
        g_fan.power_settle_ms = FAN_POWER_ON_DELAY_MS;
    }

    if (g_fan.power_settle_ms > 0u) {
        if (delta_ms >= g_fan.power_settle_ms) {
            g_fan.power_settle_ms = 0u;
        } else {
            g_fan.power_settle_ms -= delta_ms;
        }
        pwm_set_duty(0.0f);
        return;
    }

    pwm_set_duty(duty);
}

/* -------------------------------------------------------------------------- */
/* Hardware init                                                              */
/* -------------------------------------------------------------------------- */
static void board_init(void)
{
    funGpioInitAll();

    funPinMode(PIN_DISPLAY_PWR, GPIO_CFGLR_OUT_10Mhz_PP);
    funPinMode(PIN_DISPLAY_RESET, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(PIN_DISPLAY_PWR, FUN_HIGH);
    funDigitalWrite(PIN_DISPLAY_RESET, FUN_HIGH);

    funPinMode(PIN_FAN_ENABLE, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(PIN_FAN_ENABLE, FUN_LOW);

    funPinMode(PIN_FAN_PWM, GPIO_CFGLR_OUT_10Mhz_AF_PP);

    funPinMode(PIN_POWER_GOOD, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_POWER_GOOD, FUN_HIGH);

    funPinMode(PIN_INA_INT, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_INA_INT, FUN_HIGH);

    funPinMode(PIN_MODE_FAST, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_MODE_FAST, FUN_HIGH);

    funPinMode(PIN_MODE_SELECT, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_MODE_SELECT, FUN_HIGH);

    funPinMode(PIN_MODE_SLOW, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_MODE_SLOW, FUN_HIGH);

    funPinMode(PIN_FAN_TACH, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_FAN_TACH, FUN_HIGH);
}

static void pwm_init(void)
{
    RCC->APB2PRSTR |= RCC_APB2Periph_TIM1;
    RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;

    TIM1->PSC = 0;
    TIM1->ATRLR = FAN_PWM_PERIOD_TICKS - 1u;
    TIM1->SWEVGR = TIM_UG;

    TIM1->CHCTLR2 &= ~(TIM_OC3M | TIM_CC3S);
    TIM1->CHCTLR2 |= (TIM_OC3M_1 | TIM_OC3M_2);

    TIM1->CCER &= ~(TIM_CC3P | TIM_CC3NP);
    TIM1->CCER |= TIM_CC3E;

    TIM1->BDTR |= TIM_MOE;
    TIM1->CTLR1 |= TIM_CEN;

    pwm_set_duty(0.0f);
}

static void tach_init(void)
{
    RCC->APB1PRSTR |= RCC_APB1Periph_TIM2;
    RCC->APB1PRSTR &= ~RCC_APB1Periph_TIM2;
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;

    TIM2->PSC = FAN_TACH_TIMER_PRESCALER;
    TIM2->ATRLR = 0xFFFFu;
    TIM2->SWEVGR = TIM_UG;
    TIM2->CNT = 0u;
    TIM2->CTLR1 |= TIM_CEN;

    RCC->APB2PCENR |= RCC_APB2Periph_AFIO;
    AFIO->EXTICR &= ~AFIO_EXTICR_EXTI4;
    AFIO->EXTICR |= AFIO_EXTICR_EXTI4_PC;

    EXTI->INTFR = EXTI_Line4;
    EXTI->INTENR |= EXTI_INTENR_MR4;
    EXTI->FTENR |= EXTI_FTENR_TR4;
    EXTI->RTENR &= ~EXTI_RTENR_TR4;

    NVIC_EnableIRQ(EXTI7_0_IRQn);
}

/* -------------------------------------------------------------------------- */
/* Controls                                                                   */
/* -------------------------------------------------------------------------- */
static void controls_update(void)
{
    bool dec_pressed = funDigitalRead(PIN_MODE_SLOW) == 0;
    bool inc_pressed = funDigitalRead(PIN_MODE_FAST) == 0;
    bool hold_pressed = funDigitalRead(PIN_MODE_SELECT) == 0;

    uint8_t raw_mask = 0u;
    if (dec_pressed) {
        raw_mask |= 0x1u;
    }
    if (hold_pressed) {
        raw_mask |= 0x2u;
    }
    if (inc_pressed) {
        raw_mask |= 0x4u;
    }
    g_controls_raw_mask = raw_mask;

    debounce_update(&g_controls.decrease_input, dec_pressed);
    debounce_update(&g_controls.increase_input, inc_pressed);
    debounce_update(&g_controls.hold_input, hold_pressed);
}

/* -------------------------------------------------------------------------- */
/* Tach & fan state                                                           */
/* -------------------------------------------------------------------------- */
static void tach_update(uint32_t delta_ms)
{
    static uint32_t tach_timeout = 0;
    static float last_good_rpm = 0.0f;

    tach_timeout += delta_ms;

    if (g_tach.sample_ready) {
        uint16_t period = g_tach.period_ticks;
        g_tach.sample_ready = 0;
        if (period > 0u) {
            float freq = (float)FAN_TACH_TIMER_CLOCK_HZ / (float)period;
            float rpm = (freq * 60.0f) / (float)FAN_TACH_PULSES_PER_REV;
            float candidate = clampf(rpm, 0.0f, FAN_TACH_MAX_RPM);
            tach_timeout = 0;

            bool accept = true;
            if (candidate > FAN_RPM_GLITCH_UPPER_RPM) {
                accept = false;
            }

            if (accept && last_good_rpm >= FAN_RPM_GLITCH_MIN_BASE &&
                candidate < FAN_RPM_GLITCH_MIN_BASE &&
                g_manual_target > g_fan_min_ratio) {
                accept = false;
            }

            if (accept) {
                g_fan.rpm = candidate;
                g_fan.rpm_valid = true;
                last_good_rpm = candidate;
            }
        }
    }

    if (tach_timeout >= FAN_TACH_TIMEOUT_MS) {
        tach_timeout = 0;
        g_fan.rpm_valid = false;
        g_fan.rpm = 0.0f;
        last_good_rpm = 0.0f;
    }
}

static void fan_update(uint32_t delta_ms)
{
    if (!g_power.vbus_valid) {
        fan_apply_pwm(0.0f, delta_ms);
        g_fan.phase = FAN_PHASE_SOFT_START;
        g_fan.soft_timer_ms = 0;
        g_fan.rpm_max = 0.0f;
        g_fan.rpm_valid = false;
        return;
    }

    controls_update();

    if (g_fan_calibration.active) {
        g_manual_target = 1.0f;
    } else {
        float adjust = FAN_ADJUST_RATE_PER_MS * (float)delta_ms;
        if (g_controls.decrease_input.stable_state) {
            g_manual_target -= adjust;
        }
        if (g_controls.increase_input.stable_state) {
            g_manual_target += adjust;
        }

        g_manual_target = clampf(g_manual_target, 0.0f, 1.0f);
        if (g_manual_target < g_fan_min_ratio) {
            g_manual_target = g_fan_min_ratio;
        }
    }

    float ratio = g_manual_target;
    float duty_target = FAN_PWM_MIN_DUTY + ratio * (FAN_PWM_MAX_DUTY - FAN_PWM_MIN_DUTY);
    g_fan.target_duty = duty_target;

    if (g_fan.rpm_valid) {
        if (ratio > g_fan_min_ratio && g_fan.rpm < FAN_STALL_RPM_THRESHOLD) {
            g_fan_min_ratio = clampf(ratio, 0.0f, 1.0f);
        }
    }

    if (!g_fan.rpm_valid && g_fan_min_ratio > FAN_DEFAULT_MIN_RATIO) {
        g_fan_min_ratio = FAN_DEFAULT_MIN_RATIO;
    }

    if (g_fan.phase == FAN_PHASE_SOFT_START) {
        g_fan.soft_timer_ms += delta_ms;
        float progress = (g_fan.soft_timer_ms >= FAN_SOFT_START_MS)
                             ? 1.0f
                             : (float)g_fan.soft_timer_ms / (float)FAN_SOFT_START_MS;
        float start_level = FAN_PWM_MAX_DUTY;
        float duty = start_level + (duty_target - start_level) * progress;
        fan_apply_pwm(duty, delta_ms);

        if (g_fan.soft_timer_ms >= FAN_SOFT_START_MS) {
            g_fan.phase = FAN_PHASE_RUN;
        }
    } else {
        float delta = g_fan.target_duty - g_fan.current_duty;
        if (delta > FAN_PWM_RAMP_STEP) {
            fan_apply_pwm(g_fan.current_duty + FAN_PWM_RAMP_STEP, delta_ms);
        } else if (delta < -FAN_PWM_RAMP_STEP) {
            fan_apply_pwm(g_fan.current_duty - FAN_PWM_RAMP_STEP, delta_ms);
        } else {
            fan_apply_pwm(g_fan.target_duty, delta_ms);
        }
    }

    if (g_fan.rpm_valid && g_fan.rpm > g_fan.rpm_max) {
        g_fan.rpm_max = g_fan.rpm;
    }

    if (g_fan_calibration.active) {
        if (!g_pd.have_12v || !g_fan.rpm_valid) {
            return;
        }

        uint32_t now = g_uptime_ms;
        float rpm_now = g_fan.rpm;

        if (g_fan_calibration.start_ms == 0u) {
            g_fan_calibration.start_ms = now;
            g_fan_calibration.last_improve_ms = now;
            g_fan_calibration.peak_rpm = rpm_now;
        }

        if (rpm_now > g_fan_calibration.peak_rpm + FAN_CALIBRATION_DELTA_RPM) {
            g_fan_calibration.peak_rpm = rpm_now;
            g_fan_calibration.last_improve_ms = now;
        }

        if ((now - g_fan_calibration.start_ms) >= FAN_CALIBRATION_MIN_MS &&
            (now - g_fan_calibration.last_improve_ms) >= FAN_CALIBRATION_STABLE_MS) {
            float peak = g_fan_calibration.peak_rpm;
            if (peak < 1.0f) {
                peak = 1.0f;
            }

            float ratio_100rpm = clampf(100.0f / peak, 0.0f, 1.0f);
            float target_ratio = (ratio_100rpm > 0.1f) ? ratio_100rpm : 0.1f;
            g_fan_min_ratio = clampf(ratio_100rpm, FAN_DEFAULT_MIN_RATIO, 1.0f);
            if (target_ratio < g_fan_min_ratio) {
                target_ratio = g_fan_min_ratio;
            }

            g_manual_target = clampf(target_ratio, 0.0f, 1.0f);
            g_fan_calibration.active = false;
            g_fan_calibration.completed = true;
            emit_log("[cal]%u/%u",
                     (unsigned)rpm_from_float(g_fan_calibration.peak_rpm),
                     (unsigned)percent_from_unit(g_manual_target));
        }
    }

}

/* -------------------------------------------------------------------------- */
/* Power & PD handling                                                        */
/* -------------------------------------------------------------------------- */
static void power_update(void)
{
    bool prev_vbus = g_power.vbus_valid;

    bool pg_raw = funDigitalRead(PIN_POWER_GOOD) == 0;
    bool ina_raw = funDigitalRead(PIN_INA_INT) == 0;

    g_power.vbus_valid = POWER_GOOD_ACTIVE_LOW ? pg_raw : !pg_raw;
    g_power.ina_alert = INA_INT_ACTIVE_LOW ? ina_raw : !ina_raw;

    if (!g_power.vbus_valid) {
        fan_apply_pwm(0.0f, 0u);
        g_fan.phase = FAN_PHASE_SOFT_START;
        g_fan.soft_timer_ms = 0;
        g_fan.rpm_max = 0.0f;
        g_fan.rpm_valid = false;
        fan_calibration_reset();
    } else if (!prev_vbus && g_power.vbus_valid) {
        g_display_probe_attempted = false;
    }
}

static void ch224_poll(uint32_t delta_ms)
{
    static bool prev_present = false;
    static bool prev_pd_active = false;
    static bool prev_have_12v = false;
    static uint8_t prev_status = 0;

#define LOG_PD_IF_CHANGED(tag)                                                      \
    do {                                                                            \
        if (prev_present != g_pd.present ||                                         \
            prev_pd_active != g_pd.pd_active ||                                     \
            prev_have_12v != g_pd.have_12v ||                                       \
            prev_status != g_pd.last_status) {                                      \
            log_pd_snapshot(tag);                                                   \
            prev_present = g_pd.present;                                            \
            prev_pd_active = g_pd.pd_active;                                        \
            prev_have_12v = g_pd.have_12v;                                          \
            prev_status = g_pd.last_status;                                         \
        }                                                                           \
    } while (0)

    bool measured_12v = (g_ina.valid && g_ina.raw_bus_reg >= INA226_12V_RAW_THRESHOLD);

    if (!g_power.vbus_valid) {
        g_pd.present = false;
        g_pd.pd_active = false;
        g_pd.have_12v = false;
        g_pd.poll_timer_ms = 0;
        LOG_PD_IF_CHANGED("vbus_off");
        return;
    }

    /* Default to INA226 的实测结果，即便 PD 协议没有协商到 12 V 也能反映真实供电。 */
    g_pd.have_12v = measured_12v;

    g_pd.poll_timer_ms += delta_ms;
    if (g_pd.poll_timer_ms < CH224_POLL_INTERVAL_MS) {
        return;
    }
    g_pd.poll_timer_ms = 0;

    uint8_t status = 0;
    if (!i2c1_read_u8(CH224_I2C_ADDR, CH224_REG_STATUS, &status)) {
        g_pd.present = false;
        g_pd.pd_active = false;
        g_pd.have_12v = measured_12v;
        LOG_PD_IF_CHANGED("read_fail");
        if (!g_pd.have_12v) {
            fan_calibration_reset();
        }
        return;
    }

    g_pd.present = true;
    g_pd.last_status = status;

    bool pd_active = (status & CH224_STATUS_PD_ACT) != 0u;
    if (!pd_active) {
        g_pd.pd_active = false;
        g_pd.have_12v = measured_12v;
        LOG_PD_IF_CHANGED("status");
        if (!g_pd.have_12v) {
            fan_calibration_reset();
        }
        return;
    }

    g_pd.pd_active = true;

    if (!g_pd.have_12v) {
        (void)i2c1_write_u8(CH224_I2C_ADDR, CH224_REG_VSEL, CH224_VSEL_12V);
    }

    uint8_t vsel = 0;
    if (i2c1_read_u8(CH224_I2C_ADDR, CH224_REG_VSEL, &vsel)) {
        bool negotiated_12v = ((vsel & 0x07u) == CH224_VSEL_12V);
        g_pd.have_12v = negotiated_12v || measured_12v;
    }

    if (measured_12v) {
        g_pd.have_12v = true;
    }

    (void)i2c1_read_u8(CH224_I2C_ADDR, CH224_REG_CUR_STATUS, &vsel);

    LOG_PD_IF_CHANGED("status");
#undef LOG_PD_IF_CHANGED
}

/* -------------------------------------------------------------------------- */
/* Logging loop                                                               */
/* -------------------------------------------------------------------------- */
static void heartbeat_log(uint32_t tick_ms)
{
    g_log.sample_accum += tick_ms;
    bool interval_due = g_log.sample_accum >= g_log.since_last_log;

    bool should_log = !g_log.have_last || interval_due;
    if (!should_log) {
        if (g_fan.phase != g_log.last_phase ||
            g_pd.pd_active != g_log.last_pd ||
            g_pd.have_12v != g_log.last_12v ||
            g_pd.present != g_log.last_pd_present ||
            g_power.vbus_valid != g_log.last_vbus ||
            g_fan.rpm_valid != g_log.last_rpm_valid) {
            should_log = true;
        } else if (fabsf_local(g_manual_target - g_log.last_target) > LOG_DELTA_TARGET ||
                   fabsf_local(g_fan.current_duty - g_log.last_duty) > LOG_DELTA_DUTY ||
                   fabsf_local(g_fan.rpm - g_log.last_rpm) > LOG_DELTA_RPM) {
            should_log = true;
        } else if (g_pd.last_status != g_log.last_status) {
            should_log = true;
        }
    }

    if (!should_log) {
        return;
    }

    log_fan_snapshot();

    g_log.have_last = true;
    reset_fan_log_timer();
    g_log.last_phase = g_fan.phase;
    g_log.last_target = g_manual_target;
    g_log.last_duty = g_fan.current_duty;
    g_log.last_rpm = g_fan.rpm;
    g_log.last_rpm_valid = g_fan.rpm_valid;
    g_log.last_vbus = g_power.vbus_valid;
    g_log.last_pd = g_pd.pd_active;
    g_log.last_pd_present = g_pd.present;
    g_log.last_12v = g_pd.have_12v;
    g_log.last_status = g_pd.last_status;
}

/* -------------------------------------------------------------------------- */
/* Interrupt handlers                                                         */
/* -------------------------------------------------------------------------- */
void EXTI7_0_IRQHandler(void) __attribute__((interrupt));

void EXTI7_0_IRQHandler(void)
{
    uint32_t pending = EXTI->INTFR & EXTI_Line4;
    if (pending == 0u) {
        return;
    }

    EXTI->INTFR = EXTI_Line4;

    uint16_t capture = TIM2->CNT;
    if (g_tach.capture_valid) {
        uint16_t delta = (uint16_t)(capture - g_tach.last_capture);
        if (delta > 0u) {
            g_tach.period_ticks = delta;
            g_tach.sample_ready = 1;
        }
    } else {
        g_tach.capture_valid = 1;
    }

    g_tach.last_capture = capture;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */
int main(void)
{
    SystemInit();
    board_init();
    pwm_init();
    tach_init();
    ssd1306_i2c_setup();
    i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);

    (void)WaitForDebuggerToAttach(2000);

    emit_log("[boot] Whisker Breeze firmware starting (%s)", __DATE__ " " __TIME__);
    power_update();
    ch224_poll(CH224_POLL_INTERVAL_MS);
    ina226_update(INA226_POLL_INTERVAL_MS);
    log_pd_snapshot("boot");
    log_fan_snapshot();
    g_log.have_last = true;
    reset_fan_log_timer();

    printf("Whisker Breeze controller ready\r\n");

    while (1) {
        g_uptime_ms += LOOP_PERIOD_MS;
        power_update();
        tach_update(LOOP_PERIOD_MS);
        ch224_poll(LOOP_PERIOD_MS);
        ina226_update(LOOP_PERIOD_MS);
        fan_update(LOOP_PERIOD_MS);
        poll_input();
        display_try_init();
        display_render();
        heartbeat_log(LOOP_PERIOD_MS);
        Delay_Ms(LOOP_PERIOD_MS);
    }
}
