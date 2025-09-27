#include "ch32fun.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef int32_t fix16_t;

#define FIX16_FRAC_BITS 16
#define FIX16_ONE       (1 << FIX16_FRAC_BITS)

/* Use custom 72x40 with column offset 30 to align (30,14) origin */
#undef SSD1306_72X40
#define SSD1306_CUSTOM
#define SSD1306_W      72
#define SSD1306_H      40
#define SSD1306_OFFSET 30
/* 严格对齐“起始地址偏移版”参考：纵向偏移 0x0C，列起点偏移列=28（低0x0C/高0x11） */
#define SSD1306_VOFFSET 12

/*
 * 逻辑坐标从 (0,0) 开始，实际写入时通过列偏移=30、行偏移=14 映射到玻璃。
 * 注意：为了避免重复偏移，渲染接口全部以 (0,0) 作为原点。
 */
#define DISP_ORIGIN_X 0
#define DISP_ORIGIN_Y 0

static bool g_display_error_seen = false;

/* (no extra test toggles; keep firmware minimal) */

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

static void ssd1306_flush_window(void)
{
    /* 完全复刻“起始地址偏移版”风格：
     * - 页面寻址模式（0x20,0x02）
     * - 每页先发：0xB0|page，然后列起点用低列0x0C + 高列0x11 → 列地址28
     * - 每页写100字节（28..127）。其中可见玻璃72列位于索引2..73。
     * - 垂直偏移 0x0C 由控制器内部完成，我们缓冲仍按 page=0..4 线性布局即可。
     */
    uint8_t line[100];
    for (uint8_t p = 0; p < 5; ++p) {
        /* 组装一页100列数据，逻辑72列映射到 line[2..73]，左右补0 */
        memset(line, 0, sizeof line);
        for (uint16_t xi = 0; xi < SSD1306_W; ++xi) {
            uint16_t addr = xi + (uint16_t)SSD1306_W * p; /* 直接取本页72字节 */
            uint8_t v = ssd1306_buffer[addr];
            /* 放到列28起的窗口中：28 对应 line[0]，所以 30->line[2] */
            uint16_t out_idx = (uint16_t)xi + 2; /* 0..71 → 2..73 */
            if (out_idx < sizeof line) {
                line[out_idx] = v;
            }
        }

        /* 设置页+列指针并发送本页 */
        ssd1306_cmd(0xB0 | p);
        ssd1306_cmd(0x0C); /* low col = 12 (0x0C) */
        ssd1306_cmd(0x11); /* high col = 1  (0x11) */

        uint16_t i = 0;
        while (i < sizeof line) {
            uint16_t rem = (uint16_t)sizeof line - i;
            uint8_t chunk = (rem > SSD1306_PSZ) ? SSD1306_PSZ : (uint8_t)rem;
            ssd1306_data(&line[i], chunk);
            i += chunk;
        }
    }
}

extern int printf(const char *fmt, ...);
/* 测试绘图与预热接口移除（量产不需要）。 */
static void ssd1306_init_72x40_custom(void);

int mini_snprintf(char *buffer, unsigned int buffer_len, const char *fmt, ...);

/* 无 */


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
#define PIN_STATUS_LED        PC6
#define PIN_MODE_FAST         PD2
#define PIN_MODE_SELECT       PD3
#define PIN_MODE_SLOW         PD4

/* Temperature sense (NTC on PA2 → ADC channel 2) */
#define PIN_TEMP_SENSE        PA2
#define TEMP_ADC_CHANNEL      ANALOG_0

/* NTC & divider definition (FNTC0402X103F3380FB)
 * - NTC: 10 kΩ @25°C, B≈3380 K (B25/50)
 * - Divider top resistor: R3 = 8.2 kΩ to 3V3, NTC to GND
 * - ADC: CH32V003 regular ADC, treated as 10-bit full-scale (0..1023) for computation
 *
 * Historical linear anchors kept for reference/log compatibility (10-bit codes):
 *   20°C → ≈610 counts; 40°C → ≈424 counts.
 * Note: we now compute temperature via Beta model; these anchors are no longer
 * used for control but retained for potential diagnostics.
 */
#define TEMP_ADC_BITS         10
#define TEMP_ADC_FULL_SCALE   ((1u << TEMP_ADC_BITS) - 1u) /* 1023 for 10-bit */
#define TEMP_ADC_CODE_20C     610
#define TEMP_ADC_CODE_40C     424

/* NTC model parameters for Beta equation */
#define NTC_R25_OHMS          10000.0f     /* 10 kΩ */
#define NTC_BETA_K            3380.0f      /* K */
#define NTC_DIVIDER_RTOP_OHMS 8200.0f      /* 8.2 kΩ */
#define KELVIN_25C            298.15f      /* 25°C in K */
#define KELVIN_0C             273.15f      /* 0°C in K */

/* -------------------------------------------------------------------------- */
/* Control constants                                                          */
/* -------------------------------------------------------------------------- */
#define FAN_PWM_PERIOD_TICKS          2400u      /* 48 MHz / 20 kHz */
#define FAN_PWM_MIN_DUTY_Q16          (FIX16_ONE / 10)         /* 0.10 */
#define FAN_PWM_MAX_DUTY_Q16          (FIX16_ONE)
#define FAN_PWM_RAMP_STEP_Q16         (FIX16_ONE / 50)         /* 0.02 */
#define FAN_ADJUST_RATE_PER_MS_Q16    (FIX16_ONE / 2000)       /* 0.0005 */
#define FAN_SOFT_START_MS             100u
#define FAN_TACH_TIMEOUT_MS           500u
#define FAN_TACH_TIMER_PRESCALER      479u      /* 48 MHz / (479+1) = 100 kHz */
#define FAN_TACH_TIMER_CLOCK_HZ       (48000000u / (FAN_TACH_TIMER_PRESCALER + 1u))
#define FAN_TACH_PULSES_PER_REV       2u
#define FAN_TACH_MAX_RPM              6000u
#define FAN_STALL_RPM_THRESHOLD       50u
#define FAN_POWER_ON_DELAY_MS         1u
#define FAN_DEFAULT_MIN_RPM           100u
#define FAN_DEFAULT_MAX_RPM           20000u
#define FAN_DEFAULT_MIN_RATIO_Q16     ((fix16_t)((((uint64_t)FAN_DEFAULT_MIN_RPM) << FIX16_FRAC_BITS) / (FAN_DEFAULT_MAX_RPM)))
#define FAN_RPM_GLITCH_MIN_BASE       300u
#define FAN_RPM_GLITCH_UPPER_RPM      4500u
#define INA226_12V_RAW_THRESHOLD      9200u   /* 11.5 V / 1.25 mV */
#define FAN_CALIBRATION_STABLE_MS     500u
#define FAN_CALIBRATION_MIN_MS        2000u
#define FAN_CALIBRATION_DELTA_RPM     25u

#ifndef INA226_I2C_ADDR
#define INA226_I2C_ADDR           0x40u
#endif
#ifndef INA226_SHUNT_MICRO_OHMS
#define INA226_SHUNT_MICRO_OHMS   10000u   /* 10 mΩ shunt */
#endif
#ifndef INA226_MAX_EXPECTED_CURRENT_MA
#define INA226_MAX_EXPECTED_CURRENT_MA 2000u
#endif
#define INA226_CURRENT_LSB_NA     ((uint32_t)(((uint64_t)INA226_MAX_EXPECTED_CURRENT_MA * 1000000ull) / 32768u))
#define INA226_POWER_LSB_NW       (25u * INA226_CURRENT_LSB_NA)
/* Match ~1 Hz effective data cadence; new data period is ~1.064 s */
#define INA226_POLL_INTERVAL_MS   1100u

#define INA226_REG_CONFIG       0x00u
#define INA226_REG_SHUNT        0x01u
#define INA226_REG_BUS          0x02u
#define INA226_REG_POWER        0x03u
#define INA226_REG_CURRENT      0x04u
#define INA226_REG_CALIBRATION  0x05u
#define INA226_REG_MASK_ENABLE  0x06u
/* 1 Hz-level, stable sampling for INA226 (datasheet bits):
 *  D15 RST=0
 *  D14..D12 = 0b010  (POR-required pattern per DS)
 *  D11..D9  AVG=0b100 → 128 samples averaged
 *  D8..D6   VBUSCT=0b110 → 4.156 ms bus conversion
 *  D5..D3   VSHCT=0b110 → 4.156 ms shunt conversion
 *  D2..D0   MODE=0b111 → Shunt+Bus continuous
 * Effective new-data period ≈ (4.156ms + 4.156ms) * 128 ≈ 1.064 s.
 */
#define INA226_CONFIG_CONTINUOUS 0x49B7u
#define INA226_ERROR_LOG_INTERVAL_MS 1000u

#define INA226_MAX_CONFIG_FAILURES     3u
#define INA226_MAX_SAMPLE_FAILURES     3u
#define INA226_PANIC_GRACE_MS          200u

#define I2C1_SHARED_BUS_TARGET_HZ 400000u

#define LOG_SAMPLE_PERIOD_MS      1000u
#define LOG_FORCE_INTERVAL_MS     5000u
/* Throttle deltas: require larger changes to log */
#define LOG_DELTA_TARGET_Q16      (FIX16_ONE / 20)  /* 0.05 */
#define LOG_DELTA_DUTY_Q16        (FIX16_ONE / 20)  /* 0.05 */
#define LOG_DELTA_RPM             150u
/* Hard minimum spacing between [fan] logs when not force-interval */
#ifndef LOG_MIN_INTERVAL_MS
#define LOG_MIN_INTERVAL_MS       250u
#endif

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
    fix16_t target_duty;
    fix16_t current_duty;
    uint32_t rpm;
    uint32_t rpm_max;
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
    int32_t bus_voltage_mv;
    int32_t shunt_voltage_uw;
    int32_t current_ma;
    int32_t power_mw;
    uint32_t poll_timer_ms;
    uint32_t config_failures;
    uint32_t sample_failures;
    uint32_t last_error_report_ms;
    uint8_t address;
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
    /* Raw and filtered ADC for PA2 thermistor divider */
    uint16_t adc_raw;
    uint16_t adc_avg;
    /* Temperature in centi-degC; sign-capable */
    int16_t temp_c_x100;
    /* Update pacing */
    uint32_t sample_timer_ms;
} temp_state_t;

typedef struct {
    uint32_t sample_accum;
    uint32_t since_last_log;
    bool have_last;
    fan_phase_t last_phase;
    fix16_t last_target;
    fix16_t last_duty;
    uint32_t last_rpm;
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
    uint32_t peak_rpm;
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

/* -------------------------------------------------------------------------- */
/* Time base                                                                  */
/* -------------------------------------------------------------------------- */
/* Keep a precise millisecond uptime derived from SysTick instead of assuming
 * a fixed loop period. This avoids drift when work in the loop varies or when
 * Delay_Ms calibration differs across clock configs. */
static uint32_t g_uptime_ms = 0;
static uint32_t g_systick_last = 0;
static uint32_t g_systick_rem_ticks = 0;

static fix16_t g_manual_target = FIX16_ONE;

static fan_state_t g_fan = {
    .phase = FAN_PHASE_SOFT_START,
    .target_duty = 0,
    .current_duty = 0,
    .rpm = 0u,
    .rpm_max = 0u,
    .rpm_valid = false,
    .soft_timer_ms = 0,
    .power_enabled = false,
    .power_settle_ms = 0,
};

static fix16_t g_fan_min_ratio = FAN_DEFAULT_MIN_RATIO_Q16;

static power_state_t g_power = {
    .vbus_valid = false,
    .ina_alert = false,
};

static ina226_state_t g_ina = {
    .configured = false,
    .valid = false,
    .online_announced = false,
    .bus_voltage_mv = 0,
    .shunt_voltage_uw = 0,
    .current_ma = 0,
    .power_mw = 0,
    .poll_timer_ms = 0,
    .config_failures = 0,
    .sample_failures = 0,
    .last_error_report_ms = 0,
    .address = INA226_I2C_ADDR,
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
    .last_target = 0,
    .last_duty = 0,
    .last_rpm = 0u,
    .last_rpm_valid = false,
    .last_vbus = false,
    .last_pd = false,
    .last_pd_present = false,
    .last_12v = false,
    .last_status = 0,
};

static fan_calibration_state_t g_fan_calibration = {
    .active = false,
    .completed = false,
    .peak_rpm = 0u,
    .last_improve_ms = 0u,
    .start_ms = 0u,
};

static temp_state_t g_temp = {
    .adc_raw = 0u,
    .adc_avg = 0u,
    .temp_c_x100 = 0,
    .sample_timer_ms = 0u,
};

typedef enum {
    CONTROL_MODE_CALIB = 0,
    CONTROL_MODE_TEMP,
    CONTROL_MODE_MANUAL
} control_mode_t;

/* 默认先测速，完成后进入温控；MODE 只在 TEMP/MANUAL 间切换 */
static control_mode_t g_mode = CONTROL_MODE_CALIB;

static void fan_calibration_reset(void)
{
    g_fan_calibration.active = true;
    g_fan_calibration.completed = false;
    g_fan_calibration.peak_rpm = 0u;
    g_fan_calibration.last_improve_ms = 0u;
    g_fan_calibration.start_ms = 0u;
    g_manual_target = FIX16_ONE;
}

/* address scan removed; INA226 is fixed at INA226_I2C_ADDR */

static bool g_display_initialized = false;
static bool g_display_probe_attempted = false;
static bool g_display_unavailable = false;
static bool g_display_disabled_logged = false;
/* g_uptime_ms moved above into the time base section */

/* (diagnostic helpers removed to keep minimal behavior) */

/* -------------------------------------------------------------------------- */
/* Utility helpers                                                            */
/* -------------------------------------------------------------------------- */
static inline fix16_t fix16_from_int(int32_t value)
{
    return (fix16_t)(value << FIX16_FRAC_BITS);
}

static inline int32_t fix16_to_int(fix16_t value)
{
    return (int32_t)(value >> FIX16_FRAC_BITS);
}

static inline fix16_t fix16_mul(fix16_t a, fix16_t b)
{
    return (fix16_t)(((int64_t)a * (int64_t)b) >> FIX16_FRAC_BITS);
}

static inline fix16_t fix16_div(fix16_t numer, fix16_t denom)
{
    if (denom == 0) {
        return 0;
    }
    return (fix16_t)(((int64_t)numer << FIX16_FRAC_BITS) / denom);
}

static inline fix16_t fix16_clamp(fix16_t value, fix16_t min_val, fix16_t max_val)
{
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

static inline fix16_t fix16_abs(fix16_t value)
{
    return (value >= 0) ? value : (fix16_t)(-value);
}

/* Fast natural log approximation for x>0 without libm.
 * Uses range reduction by powers of 2 and an atanh-like series on [0.5,2].
 * Accuracy is within ~1e-4 for our working range (0.4..2.0), sufficient for Beta calc. */
/* Precomputed ADC(10-bit) → temperature (centi-°C) table using Beta model for
 * FNTC0402X103F3380FB with Rtop=8.2k. Linear interpolation between points.
 * Points cover 0–100°C; control band is 20–40°C. */
typedef struct { uint16_t code; int16_t t_c_x100; } ntc_point_t;
static const ntc_point_t k_ntc_table[] = {
    { 793,    0   }, /*   0°C */
    { 706,  1000 }, /*  10°C */
    { 610,  2000 }, /*  20°C */
    { 562,  2500 }, /*  25°C */
    { 514,  3000 }, /*  30°C */
    { 468,  3500 }, /*  35°C */
    { 424,  4000 }, /*  40°C */
    { 383,  4500 }, /*  45°C */
    { 344,  5000 }, /*  50°C */
    { 309,  5500 }, /*  55°C */
    { 277,  6000 }, /*  60°C */
    { 221,  7000 }, /*  70°C */
    { 177,  8000 }, /*  80°C */
    { 114, 10000 }, /* 100°C */
};

static uint16_t percent_from_ratio(fix16_t value)
{
    if (value <= 0) {
        return 0u;
    }
    if (value >= FIX16_ONE) {
        return 100u;
    }
    return (uint16_t)(((int64_t)value * 100 + (FIX16_ONE / 2)) >> FIX16_FRAC_BITS);
}

static uint16_t rpm_to_u16(uint32_t rpm)
{
    if (rpm > 0xFFFFu) {
        return 0xFFFFu;
    }
    return (uint16_t)rpm;
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

    /* Minimal fix: if no debugger, skip logging to avoid blocking. */
    if (!DidDebuggerAttach()) {
        return;
    }

    while (!DebugPrintfBufferFree()) {
        poll_input();
    }
    printf("%s\r\n", line);
    while (!DebugPrintfBufferFree()) {
        poll_input();
    }
}

static void panic(const char *fmt, ...)
{
    char message[160];
    va_list args;
    va_start(args, fmt);
    int written = mini_vsnprintf(message, sizeof message, fmt, args);
    va_end(args);

    if (written < 0) {
        message[0] = '\0';
    } else if (written >= (int)sizeof message) {
        written = (int)sizeof message - 1;
        message[written] = '\0';
    } else {
        message[written] = '\0';
    }

    if (message[0] != '\0') {
        emit_log("[panic] %s", message);
    } else {
        emit_log("[panic]");
    }

    __disable_irq();
    for (;;) {
        __NOP();
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
    uint16_t target_pct = percent_from_ratio(g_manual_target);
    uint16_t duty_pct = percent_from_ratio(g_fan.current_duty);
    uint16_t rpm_now = rpm_to_u16(g_fan.rpm);
    uint16_t temp_c = (g_temp.temp_c_x100 >= 0) ? (uint16_t)g_temp.temp_c_x100 : 0u;
    uint16_t bus_raw = g_ina.valid ? g_ina.raw_bus_reg : 0u;
    int32_t current_ma = g_ina.current_ma;
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

    emit_log("[fan] p=%d t=%u d=%u r=%u tmp=%u adc=%u/%u v=%d 12=%d b=%04X i=%ld c=%u k=%u/%u",
             g_fan.phase,
             (unsigned)target_pct,
             (unsigned)duty_pct,
             (unsigned)rpm_now,
             (unsigned)temp_c,
             (unsigned)g_temp.adc_raw,
             (unsigned)g_temp.adc_avg,
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

/* i2c1_scan_bus removed: address discovery not required */

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
    uint64_t denom = (uint64_t)INA226_CURRENT_LSB_NA * (uint64_t)INA226_SHUNT_MICRO_OHMS;
    if (denom == 0u) {
        return 0u;
    }

    /* 0.00512 / (LSB_A * R_ohm)
     * 采用 LSB 单位=nA、R 单位=µΩ，则分母量纲=1e-15，
     * 因此需要乘以 1e15：0.00512 * 1e15 = 5_120_000_000_000。*/
    uint64_t cal = 5120000000000ull / denom;
    if (cal > 0xFFFFu) {
        cal = 0xFFFFu;
    }
    return (uint16_t)cal;
}

static bool ina226_write_register(uint8_t reg, uint16_t value)
{
    return i2c1_write_u16(g_ina.address, reg, value);
}

static bool ina226_read_register(uint8_t reg, uint16_t *value)
{
    return i2c1_read_u16(g_ina.address, reg, value);
}

static void ina226_report_error(const char *stage)
{
    uint32_t now = g_uptime_ms;
    if (g_ina.last_error_report_ms == 0u ||
        (now - g_ina.last_error_report_ms) >= INA226_ERROR_LOG_INTERVAL_MS) {
        emit_log("[ina]%s fail 0x%02X", stage, (unsigned)g_ina.address);
        g_ina.last_error_report_ms = (now == 0u) ? 1u : now;
    }
}

/* ina226_detect_address removed: fixed address is used */

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
    if (!g_ina.configured) {
        if (!ina226_configure()) {
            g_ina.valid = false;
            g_ina.online_announced = false;
            if (g_ina.config_failures < UINT32_MAX) {
                g_ina.config_failures++;
            }
            g_ina.raw_bus_reg = 0u;
            ina226_report_error("configure");
            if (g_ina.config_failures >= INA226_MAX_CONFIG_FAILURES &&
                g_uptime_ms >= INA226_PANIC_GRACE_MS) {
                panic("INA226 configure failed");
            }
            return;
        }
        g_ina.configured = true;
        g_ina.config_failures = 0u;
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
        /* keep fixed address; do not reset */
        g_ina.config_failures = 0u;
        g_ina.bus_voltage_mv = 0;
        g_ina.shunt_voltage_uw = 0;
        g_ina.current_ma = 0;
        g_ina.power_mw = 0;
        g_ina.raw_bus_reg = 0u;
        if (g_ina.sample_failures < UINT32_MAX) {
            g_ina.sample_failures++;
        }
        ina226_report_error("sample");
        if (g_ina.sample_failures >= INA226_MAX_SAMPLE_FAILURES &&
            g_uptime_ms >= INA226_PANIC_GRACE_MS) {
            panic("INA226 read failed");
        }
        return;
    }

    g_ina.sample_failures = 0u;
    g_ina.raw_bus_reg = raw_bus;

    int32_t bus_mv = (int32_t)(((uint64_t)raw_bus * 1250u + 500u) / 1000u);
    int32_t shunt_uw = (int32_t)((((int64_t)(int16_t)raw_shunt) * 25) / 10);
    int32_t current_ma = (int32_t)((((int64_t)(int16_t)raw_current) * INA226_CURRENT_LSB_NA + 500000) / 1000000);
    int32_t power_mw = (int32_t)((((int64_t)raw_power) * INA226_POWER_LSB_NW + 500000) / 1000000);

    g_ina.bus_voltage_mv = bus_mv;
    g_ina.shunt_voltage_uw = shunt_uw;
    g_ina.current_ma = current_ma;
    g_ina.power_mw = power_mw;
    g_ina.valid = true;
    if (!g_ina.online_announced) {
        long bus_whole = bus_mv / 1000;
        long bus_frac = bus_mv % 1000;
        emit_log("[ina]%ld.%03ldV", bus_whole, bus_frac);
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
    /* Requirement: BAT must be held low (enable) at all times. */
    if (enable) {
        funDigitalWrite(PIN_DISPLAY_PWR, FUN_LOW);   /* BAT low: enable */
        funDigitalWrite(PIN_DISPLAY_RESET, FUN_LOW); /* hold reset */
        Delay_Ms(5);
        funDigitalWrite(PIN_DISPLAY_RESET, FUN_HIGH);
    } else {
        /* Keep BAT asserted low; do not disable power. */
        funDigitalWrite(PIN_DISPLAY_PWR, FUN_LOW);
    }
}

static void display_render(void)
{
    if (!g_display_initialized) {
        return;
    }

    char line[12];
    ssd1306_setbuf(0);

    uint16_t set_pct = percent_from_ratio(g_manual_target);
    uint16_t out_pct = percent_from_ratio(g_fan.current_duty);
    uint32_t rpm_now = g_fan.rpm;

    snprintf(line, sizeof line, "Set %3u%%", (unsigned)set_pct);
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 0, line, 1);

    snprintf(line, sizeof line, "Out %3u%%", (unsigned)out_pct);
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 8, line, 1);

    snprintf(line, sizeof line, "RPM %4lu", (unsigned long)rpm_now);
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 16, line, 1);

    /* 第四行：显示控制模式（不加标签）。AUTO/Manual/Calib */
    if (g_mode == CONTROL_MODE_TEMP) {
        snprintf(line, sizeof line, "Auto");
    } else if (g_mode == CONTROL_MODE_MANUAL) {
        snprintf(line, sizeof line, "Manual");
    } else {
        snprintf(line, sizeof line, "Calib");
    }
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 24, line, 1);

    /* 第五行：显示电压电流（无标签）。格式固定为 "xx.xV yyyymA"，总长<=12。 */
    if (g_ina.valid) {
        int32_t mv = g_ina.bus_voltage_mv;
        int32_t ma = g_ina.current_ma;
        if (mv < 0) mv = 0; /* Clamp to 0 for display */
        if (ma < 0) ma = 0;
        long v_int = (long)(mv / 1000);
        long v_dec1 = (long)((mv % 1000 + 50) / 100); /* round to 0.1V */
        if (v_dec1 >= 10) { v_int += 1; v_dec1 -= 10; }
        long ma_int = (long)ma; /* already rounded to 1 mA in INA driver */
        if (ma_int < 0) ma_int = 0;
        if (ma_int > 9999) ma_int = 9999; /* clamp to fit width */
        /* e.g. "12.3V 450mA" -> fits within 12 chars */
        snprintf(line, sizeof line, "%ld.%ldV %ldmA",
                 v_int, v_dec1, ma_int);
    } else {
        /* 传感器无效时显示空值，不加任何标签 */
        snprintf(line, sizeof line, "--.-V ----mA");
    }
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 32, line, 1);

    ssd1306_flush_window();
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
    Delay_Ms(120);

    g_display_error_seen = false;
    if (ssd1306_i2c_init() == 0) {
        const uint8_t probe_cmd = 0xAE; /* Display OFF */
        int probe_result = ssd1306_pkt_send(&probe_cmd, 1, 1);
        if (!g_display_error_seen && probe_result == 0) {
            /* 使用 72x40 专用初始化序列：多路复用=0x27、COMpins=0x12、偏移Y=14、列偏移=30 */
            ssd1306_init_72x40_custom();
            ssd1306_setbuf(0);
            g_display_initialized = true;
            g_display_disabled_logged = false;
            emit_log("[disp] ok ack=%d w=%u h=%u offset=%u",
                     probe_result,
                     (unsigned)SSD1306_W,
                     (unsigned)SSD1306_H,
                     (unsigned)SSD1306_OFFSET);

            /* 初始化成功，后续由常规渲染路径 display_render() 输出内容。*/
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
/* OLED: 专用初始化 + 预热填充                                                 */
/* -------------------------------------------------------------------------- */
static void ssd1306_init_72x40_custom(void)
{
    /* 完全对齐参考示例（起始地址偏移版） */
    ssd1306_cmd(SSD1306_DISPLAYOFF);              // 0xAE
    ssd1306_cmd(SSD1306_SETDISPLAYCLOCKDIV);      // 0xD5
    ssd1306_cmd(0xF0);

    ssd1306_cmd(SSD1306_SETMULTIPLEX);            // 0xA8
    ssd1306_cmd(SSD1306_H - 1);                   // 0x27 (40 rows)

    ssd1306_cmd(SSD1306_SETDISPLAYOFFSET);        // 0xD3
    ssd1306_cmd(SSD1306_VOFFSET);                 // 0x0C

    ssd1306_cmd(SSD1306_SETSTARTLINE | 0x00);     // 0x40
    ssd1306_cmd(SSD1306_CHARGEPUMP);              // 0x8D
    ssd1306_cmd(0x14);

    ssd1306_cmd(SSD1306_MEMORYMODE);              // 0x20
    ssd1306_cmd(0x02);                             // Page Addressing Mode

    ssd1306_cmd(SSD1306_SEGREMAP | 0x01);         // 0xA1
    ssd1306_cmd(SSD1306_COMSCANDEC);              // 0xC8

    ssd1306_cmd(SSD1306_SETCOMPINS);              // 0xDA
    ssd1306_cmd(0x12);

    ssd1306_cmd(0xAD);                            // Internal IREF
    ssd1306_cmd(0x30);

    ssd1306_cmd(SSD1306_SETCONTRAST);             // 0x81
    ssd1306_cmd(0xFF);
    ssd1306_cmd(SSD1306_SETPRECHARGE);            // 0xD9
    ssd1306_cmd(0x22);
    ssd1306_cmd(SSD1306_SETVCOMDETECT);           // 0xDB
    ssd1306_cmd(0x20);

    ssd1306_cmd(SSD1306_DISPLAYALLON_RESUME);     // 0xA4
    ssd1306_cmd(SSD1306_NORMALDISPLAY);           // 0xA6
    ssd1306_cmd(SSD1306_DISPLAYON);               // 0xAF
}

/* 预热填充与测试输出已移除，保持量产固件最小化。 */

/* -------------------------------------------------------------------------- */
/* PWM helpers                                                                */
/* -------------------------------------------------------------------------- */
static void pwm_set_duty(fix16_t duty)
{
    duty = fix16_clamp(duty, 0, FIX16_ONE);
    g_fan.current_duty = duty;

    uint32_t compare = (uint32_t)((((int64_t)(FAN_PWM_PERIOD_TICKS - 1u)) * duty + (FIX16_ONE / 2)) >> FIX16_FRAC_BITS);
    if (compare > (FAN_PWM_PERIOD_TICKS - 1u)) {
        compare = FAN_PWM_PERIOD_TICKS - 1u;
    }
    TIM1->CH3CVR = (uint16_t)compare;
}

static void fan_apply_pwm(fix16_t duty, uint32_t delta_ms)
{
    duty = fix16_clamp(duty, 0, FIX16_ONE);

    if (duty <= 0) {
        funDigitalWrite(PIN_FAN_ENABLE, FUN_LOW);
        g_fan.power_enabled = false;
        g_fan.power_settle_ms = 0u;
        pwm_set_duty(0);
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
        pwm_set_duty(0);
        return;
    }

    pwm_set_duty(duty);
}

/* -------------------------------------------------------------------------- */
/* Status LED (PC6, open-drain, active-low)                                   */
/* -------------------------------------------------------------------------- */
/* LED timing (tunable) */
#ifndef LED_PULSE_ON_MS
#define LED_PULSE_ON_MS   20u
#endif
#ifndef LED_PULSE_OFF_MS
#define LED_PULSE_OFF_MS  60u
#endif
#ifndef LED_CYCLE_MS
#define LED_CYCLE_MS      4000u
#endif

typedef enum {
    LED_BOOT_SOLID = 0,   /* Solid ON during boot/bring-up */
    LED_PULSE_ON,         /* Inside a pulse: ON for LED_PULSE_ON_MS */
    LED_PULSE_OFF,        /* Inter-pulse OFF for LED_PULSE_OFF_MS */
    LED_GAP               /* Idle until LED_CYCLE_MS cycle ends */
} led_state_t;

static struct {
    led_state_t state;
    uint32_t    state_ms;
    uint32_t    cycle_ms;         /* 0..4000 */
    uint8_t     pulses_target;    /* 1..10 computed at cycle start */
    uint8_t     pulses_emitted;   /* how many pulses emitted in this cycle */
    bool        ready_started;    /* latched when pulse mode begins */
} g_led = { .state = LED_BOOT_SOLID, .state_ms = 0, .cycle_ms = 0, .pulses_target = 1, .pulses_emitted = 0, .ready_started = false };

static inline void led_on(void)  { funDigitalWrite(PIN_STATUS_LED, FUN_LOW); }
static inline void led_off(void) { funDigitalWrite(PIN_STATUS_LED, FUN_HIGH); }

static inline uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* LED 脉冲分级仅按当前占空比（Duty%）映射。*/
static inline uint8_t led_pct_from_duty(void)
{
    return (uint8_t)percent_from_ratio(g_fan.current_duty); /* 0..100 */
}

static void led_update(uint32_t delta_ms)
{
    /* Determine readiness: VBUS valid and calibration completed with at least one RPM sample. */
    bool system_ready = g_power.vbus_valid && g_fan_calibration.completed && g_fan.rpm_valid;

    if (!g_led.ready_started) {
        if (!system_ready) {
            /* Boot/bring-up: solid ON (active-low). */
            led_on();
            return;
        }
        /* Transition to pulse mode on first ready. */
        g_led.ready_started = true;
        g_led.state = LED_PULSE_ON;
        g_led.state_ms = 0;
        g_led.cycle_ms = 0;
        g_led.pulses_emitted = 0;
        /* Compute initial pulses from RPM percentage (1..10). */
        uint32_t pct = led_pct_from_duty();
        uint8_t pulses = (uint8_t)((pct * 10u + 99u) / 100u); /* 0..10 -> round up per bin */
        g_led.pulses_target = clamp_u8(pulses == 0 ? 1 : pulses, 1, 10);
        emit_log("[led]%u", (unsigned)g_led.pulses_target);
        led_on();
        return;
    }

    /* Pulse mode running */
    g_led.state_ms += delta_ms;
    g_led.cycle_ms += delta_ms;

    switch (g_led.state) {
    case LED_BOOT_SOLID:
        /* Should not stay here once ready_started is true */
        led_on();
        break;
    case LED_PULSE_ON:
        if (g_led.state_ms >= LED_PULSE_ON_MS) {
            g_led.state = LED_PULSE_OFF;
            g_led.state_ms = 0;
            led_off();
        }
        break;
    case LED_PULSE_OFF:
        if (g_led.state_ms >= LED_PULSE_OFF_MS) {
            g_led.pulses_emitted++;
            g_led.state_ms = 0;
            if (g_led.pulses_emitted < g_led.pulses_target) {
                g_led.state = LED_PULSE_ON;
                led_on();
            } else {
                g_led.state = LED_GAP;
                led_off();
            }
        }
        break;
    case LED_GAP:
        /* Wait for end of 4 s cycle. */
        if (g_led.cycle_ms >= LED_CYCLE_MS) {
            /* Start next cycle: recompute target pulses from newest RPM. */
            g_led.cycle_ms = 0;
            g_led.state = LED_PULSE_ON;
            g_led.state_ms = 0;
            g_led.pulses_emitted = 0;
            uint32_t pct = led_pct_from_duty();
            uint8_t pulses = (uint8_t)((pct * 10u + 99u) / 100u);
            g_led.pulses_target = clamp_u8(pulses == 0 ? 1 : pulses, 1, 10);
            emit_log("[led]%u", (unsigned)g_led.pulses_target);
            led_on();
        }
        break;
    }
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

    /* Status LED: PC6, open-drain, active-low. Default ON at power-up. */
    funPinMode(PIN_STATUS_LED, GPIO_CFGLR_OUT_10Mhz_OD);
    funDigitalWrite(PIN_STATUS_LED, FUN_LOW);
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

    pwm_set_duty(0);
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
    bool changed = debounce_update(&g_controls.hold_input, hold_pressed);

    /* Rising edge on MODE → 只在温控/手动间切换；测速模式下无效 */
    static bool s_last_hold = false;
    if (changed) {
        bool now = g_controls.hold_input.stable_state;
        if (now && !s_last_hold) {
            if (g_mode != CONTROL_MODE_CALIB) {
                g_mode = (g_mode == CONTROL_MODE_TEMP) ? CONTROL_MODE_MANUAL : CONTROL_MODE_TEMP;
                emit_log("[mode]%s", (g_mode == CONTROL_MODE_TEMP) ? "auto" : "manual");
            }
        }
        s_last_hold = now;
    }
}

/* -------------------------------------------------------------------------- */
/* Temperature                                                                */
/* -------------------------------------------------------------------------- */
#define TEMP_POLL_INTERVAL_MS   50u
#define TEMP_AVG_SHIFT          3u    /* IIR avg: 1/8 */

static uint16_t temp_adc_sample(void);
static int16_t ntc_temp_c_x100_from_adc(uint16_t code);
static fix16_t temp_target_ratio_from_temp(int16_t temp_c_x100);

static void temp_init(void)
{
    funGpioInitA();
    funPinMode(PIN_TEMP_SENSE, GPIO_CFGLR_IN_ANALOG);
    funAnalogInit();

    /* Increase ADC sample time for channel 2 to 239.5 cycles.
     * Source impedance ≈ R3||Rntc ≈ 0.5–8 kΩ; longer sampling improves accuracy.
     */
    uint32_t shift = (uint32_t)3u * (uint32_t)TEMP_ADC_CHANNEL; /* in SAMPTR2 */
    ADC1->SAMPTR2 = (ADC1->SAMPTR2 & ~(7u << shift)) | (7u << shift);

    /* 预充样本，避免上电瞬间误判 */
    uint16_t seed = temp_adc_sample();
    g_temp.adc_raw = seed;
    g_temp.adc_avg = seed;
}

static uint16_t temp_adc_sample(void)
{
    /* Single conversion, 12-bit */
    int sample = funAnalogRead(TEMP_ADC_CHANNEL);
    if (sample < 0) {
        sample = 0;
    }
    if (sample > (int)TEMP_ADC_FULL_SCALE) {
        sample = (int)TEMP_ADC_FULL_SCALE;
    }
    return (uint16_t)sample;
}

static void temp_update(uint32_t delta_ms)
{
    g_temp.sample_timer_ms += delta_ms;
    if (g_temp.sample_timer_ms < TEMP_POLL_INTERVAL_MS) {
        return;
    }
    g_temp.sample_timer_ms = 0u;

    uint16_t raw = temp_adc_sample();
    g_temp.adc_raw = raw;

    if (g_temp.adc_avg == 0u) {
        g_temp.adc_avg = raw;
    } else {
        /* IIR averaging: avg += (raw - avg) / 2^N */
    g_temp.adc_avg = (uint16_t)(g_temp.adc_avg + ((int32_t)raw - (int32_t)g_temp.adc_avg) / (1 << TEMP_AVG_SHIFT));
    }

    /* Compute temperature using Beta model for logging/control accuracy */
    g_temp.temp_c_x100 = ntc_temp_c_x100_from_adc(g_temp.adc_avg);
}

/* Convert ADC code to temperature (centi-°C) using Beta equation.
 * Steps: code→voltage ratio→Rntc→T via 1/T = 1/T25 + (1/B)*ln(R/R25)
 */
static int16_t ntc_temp_c_x100_from_adc(uint16_t code)
{
    /* Clamp code to table bounds */
    if (code >= k_ntc_table[0].code) {
        return k_ntc_table[0].t_c_x100;
    }
    const size_t N = sizeof(k_ntc_table) / sizeof(k_ntc_table[0]);
    if (code <= k_ntc_table[N - 1].code) {
        return k_ntc_table[N - 1].t_c_x100;
    }

    /* Find segment [i,i+1] where code is between table codes (descending). */
    size_t i = 0;
    for (; i + 1 < N; ++i) {
        if (code <= k_ntc_table[i].code && code >= k_ntc_table[i + 1].code) {
            break;
        }
    }
    uint16_t c_hi = k_ntc_table[i].code;
    uint16_t c_lo = k_ntc_table[i + 1].code;
    int16_t  t_hi = k_ntc_table[i].t_c_x100;
    int16_t  t_lo = k_ntc_table[i + 1].t_c_x100;

    uint16_t span = (uint16_t)(c_hi - c_lo);
    uint16_t pos  = (uint16_t)(c_hi - code);
    /* Linear interpolation: t = t_hi + (t_lo - t_hi) * pos/span */
    int32_t t = (int32_t)t_hi + (int32_t)(t_lo - t_hi) * (int32_t)pos / (span ? span : 1u);
    return (int16_t)t;
}

/* Map temperature (centi-°C) to target ratio: <20°C => 0, 20–40°C => min..1, >=40°C => 1 */
static fix16_t temp_target_ratio_from_temp(int16_t temp_c_x100)
{
    if (temp_c_x100 <= 2000) {
        return 0;
    }
    if (temp_c_x100 >= 4000) {
        return FIX16_ONE;
    }
    int32_t pos = (int32_t)temp_c_x100 - 2000; /* 0..2000 */
    fix16_t frac = fix16_div(fix16_from_int(pos), fix16_from_int(2000));
    fix16_t range = FIX16_ONE - g_fan_min_ratio;
    return g_fan_min_ratio + fix16_mul(frac, range);
}

/* -------------------------------------------------------------------------- */
/* Tach & fan state                                                           */
/* -------------------------------------------------------------------------- */
static void tach_update(uint32_t delta_ms)
{
    static uint32_t tach_timeout = 0;
    static uint32_t last_good_rpm = 0u;

    tach_timeout += delta_ms;

    if (g_tach.sample_ready) {
        uint16_t period = g_tach.period_ticks;
        g_tach.sample_ready = 0;
        if (period > 0u) {
            uint64_t numerator = (uint64_t)FAN_TACH_TIMER_CLOCK_HZ * 60u;
            uint64_t denominator = (uint64_t)period * FAN_TACH_PULSES_PER_REV;
            uint32_t rpm = (denominator == 0u) ? 0u : (uint32_t)(numerator / denominator);
            if (rpm > FAN_TACH_MAX_RPM) {
                rpm = FAN_TACH_MAX_RPM;
            }
            tach_timeout = 0;

            bool accept = true;
            if (rpm > FAN_RPM_GLITCH_UPPER_RPM) {
                accept = false;
            }

            if (accept && last_good_rpm >= FAN_RPM_GLITCH_MIN_BASE &&
                rpm < FAN_RPM_GLITCH_MIN_BASE &&
                g_manual_target > g_fan_min_ratio) {
                accept = false;
            }

            if (accept) {
                g_fan.rpm = rpm;
                g_fan.rpm_valid = true;
                last_good_rpm = rpm;
            }
        }
    }

    if (tach_timeout >= FAN_TACH_TIMEOUT_MS) {
        tach_timeout = 0;
        g_fan.rpm_valid = false;
        g_fan.rpm = 0u;
        last_good_rpm = 0u;
    }
}

static void fan_update(uint32_t delta_ms)
{
    if (!g_power.vbus_valid) {
        fan_apply_pwm(0, delta_ms);
        g_fan.phase = FAN_PHASE_SOFT_START;
        g_fan.soft_timer_ms = 0;
        g_fan.rpm_max = 0u;
        g_fan.rpm_valid = false;
        return;
    }

    controls_update();

    /* 三模式：测速→温控/手动；MODE只在温控/手动切换 */
    if (g_mode == CONTROL_MODE_CALIB) {
        g_manual_target = FIX16_ONE; /* 全速测峰值 */
    } else if (g_mode == CONTROL_MODE_TEMP) {
        fix16_t auto_target = temp_target_ratio_from_temp(g_temp.temp_c_x100);
        g_manual_target = fix16_clamp(auto_target, 0, FIX16_ONE);
    } else {
        /* 手动：按键调整 */
        fix16_t adjust = (fix16_t)((int64_t)FAN_ADJUST_RATE_PER_MS_Q16 * delta_ms);
        if (g_controls.decrease_input.stable_state) {
            g_manual_target -= adjust;
        }
        if (g_controls.increase_input.stable_state) {
            g_manual_target += adjust;
        }
        g_manual_target = fix16_clamp(g_manual_target, 0, FIX16_ONE);
        if (g_manual_target < g_fan_min_ratio) {
            g_manual_target = g_fan_min_ratio;
        }
    }

    fix16_t ratio = g_manual_target;
    fix16_t duty_span = FAN_PWM_MAX_DUTY_Q16 - FAN_PWM_MIN_DUTY_Q16;
    fix16_t duty_target = FAN_PWM_MIN_DUTY_Q16 + fix16_mul(ratio, duty_span);
    g_fan.target_duty = duty_target;

    if (g_fan.rpm_valid) {
        if (ratio > g_fan_min_ratio && g_fan.rpm < FAN_STALL_RPM_THRESHOLD) {
            g_fan_min_ratio = fix16_clamp(ratio, 0, FIX16_ONE);
        }
    }

    if (!g_fan.rpm_valid && g_fan_min_ratio > FAN_DEFAULT_MIN_RATIO_Q16) {
        g_fan_min_ratio = FAN_DEFAULT_MIN_RATIO_Q16;
    }

    if (g_fan.phase == FAN_PHASE_SOFT_START) {
        g_fan.soft_timer_ms += delta_ms;
        fix16_t progress = (g_fan.soft_timer_ms >= FAN_SOFT_START_MS)
                               ? FIX16_ONE
                               : fix16_div(fix16_from_int((int32_t)g_fan.soft_timer_ms),
                                            fix16_from_int((int32_t)FAN_SOFT_START_MS));
        fix16_t start_level = FAN_PWM_MAX_DUTY_Q16;
        fix16_t duty_delta = duty_target - start_level;
        fix16_t duty = start_level + fix16_mul(duty_delta, progress);
        fan_apply_pwm(duty, delta_ms);

        if (g_fan.soft_timer_ms >= FAN_SOFT_START_MS) {
            g_fan.phase = FAN_PHASE_RUN;
        }
    } else {
        fix16_t delta = g_fan.target_duty - g_fan.current_duty;
        if (delta > FAN_PWM_RAMP_STEP_Q16) {
            fan_apply_pwm(g_fan.current_duty + FAN_PWM_RAMP_STEP_Q16, delta_ms);
        } else if (delta < -FAN_PWM_RAMP_STEP_Q16) {
            fan_apply_pwm(g_fan.current_duty - FAN_PWM_RAMP_STEP_Q16, delta_ms);
        } else {
            fan_apply_pwm(g_fan.target_duty, delta_ms);
        }
    }

    if (g_fan.rpm_valid && g_fan.rpm > g_fan.rpm_max) {
        g_fan.rpm_max = g_fan.rpm;
    }

    if (g_fan_calibration.active) {
        /* 需要 12V；但即使当前 rpm 无效，只要曾经有效过也可完成 */
        if (!g_pd.have_12v) {
            return;
        }

        uint32_t now = g_uptime_ms;

        if (g_fan.rpm_valid) {
            uint32_t rpm_now = g_fan.rpm;
            if (g_fan_calibration.start_ms == 0u) {
                g_fan_calibration.start_ms = now;
                g_fan_calibration.last_improve_ms = now;
                g_fan_calibration.peak_rpm = rpm_now;
            }

            if (rpm_now > g_fan_calibration.peak_rpm + FAN_CALIBRATION_DELTA_RPM) {
                g_fan_calibration.peak_rpm = rpm_now;
                g_fan_calibration.last_improve_ms = now;
            }
        } else {
            /* 未拿到首个有效样本则继续等待 */
            if (g_fan_calibration.start_ms == 0u) {
                return;
            }
        }

        if ((now - g_fan_calibration.start_ms) >= FAN_CALIBRATION_MIN_MS &&
            (now - g_fan_calibration.last_improve_ms) >= FAN_CALIBRATION_STABLE_MS) {
            uint32_t peak = g_fan_calibration.peak_rpm;
            if (peak == 0u) {
                peak = 1u;
            }

            fix16_t ratio_100rpm = fix16_div(fix16_from_int(100), fix16_from_int((int32_t)peak));
            ratio_100rpm = fix16_clamp(ratio_100rpm, 0, FIX16_ONE);

            g_fan_min_ratio = fix16_clamp(ratio_100rpm, FAN_DEFAULT_MIN_RATIO_Q16, FIX16_ONE);

            fix16_t target_ratio = ratio_100rpm;
            if (target_ratio < FAN_PWM_MIN_DUTY_Q16) {
                target_ratio = FAN_PWM_MIN_DUTY_Q16;
            }
            if (target_ratio < g_fan_min_ratio) {
                target_ratio = g_fan_min_ratio;
            }

            g_manual_target = fix16_clamp(target_ratio, 0, FIX16_ONE);
            g_fan_calibration.active = false;
            g_fan_calibration.completed = true;
            emit_log("[cal]%u/%u",
                     (unsigned)rpm_to_u16(g_fan_calibration.peak_rpm),
                     (unsigned)percent_from_ratio(g_manual_target));

            /* 测速完成后切入温控 */
            g_mode = CONTROL_MODE_TEMP;
            emit_log("[mode]auto");
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
        fan_apply_pwm(0, 0u);
        g_fan.phase = FAN_PHASE_SOFT_START;
        g_fan.soft_timer_ms = 0;
        g_fan.rpm_max = 0u;
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
        } else {
            fix16_t target_delta = fix16_abs(g_manual_target - g_log.last_target);
            fix16_t duty_delta = fix16_abs(g_fan.current_duty - g_log.last_duty);
            uint32_t rpm_delta = (g_fan.rpm > g_log.last_rpm)
                                     ? (g_fan.rpm - g_log.last_rpm)
                                     : (g_log.last_rpm - g_fan.rpm);

            if (target_delta > LOG_DELTA_TARGET_Q16 ||
                duty_delta > LOG_DELTA_DUTY_Q16 ||
                rpm_delta > LOG_DELTA_RPM) {
                should_log = true;
            } else if (g_pd.last_status != g_log.last_status) {
                should_log = true;
            }
        }
    }

    /* Rate-limit spontaneous logs: if not forced by interval, require a
     * minimum spacing to avoid spamming during rapid RPM transitions. */
    if (should_log && !interval_due && g_log.sample_accum < LOG_MIN_INTERVAL_MS) {
        should_log = false;
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
    temp_init();
    ssd1306_i2c_setup();
    i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);

    (void)WaitForDebuggerToAttach(13);

    emit_log("[boot] Whisker Breeze firmware starting (%s)", __DATE__ " " __TIME__);
    power_update();
    /* 上电先进入测速模式 */
    g_mode = CONTROL_MODE_CALIB;
    fan_calibration_reset();
    emit_log("[mode]init calib");
    ch224_poll(CH224_POLL_INTERVAL_MS);
    ina226_update(INA226_POLL_INTERVAL_MS);
    log_pd_snapshot("boot");
    log_fan_snapshot();
    g_log.have_last = true;
    reset_fan_log_timer();

    printf("Whisker Breeze controller ready\r\n");

    /* Seed SysTick-based timekeeping. */
    g_systick_last = SysTick->CNT;
    g_systick_rem_ticks = 0;

    while (1) {
        /* Measure elapsed real time using SysTick and convert to ms. */
        uint32_t now = SysTick->CNT;
        uint32_t dt_ticks = now - g_systick_last;
        g_systick_last = now;

        uint32_t accum_ticks = dt_ticks + g_systick_rem_ticks;
        uint32_t delta_ms = accum_ticks / DELAY_MS_TIME;
        g_systick_rem_ticks = accum_ticks % DELAY_MS_TIME;
        if (delta_ms == 0u) {
            /* Extremely rare here (loop contains a sleep). Ensure forward progress. */
            delta_ms = 1u;
        }

        g_uptime_ms += delta_ms;

        power_update();
        tach_update(delta_ms);
        ch224_poll(delta_ms);
        ina226_update(delta_ms);
        temp_update(delta_ms);
        fan_update(delta_ms);
        poll_input();
        display_try_init();
        display_render();
        led_update(delta_ms);
        heartbeat_log(delta_ms);
        
        /* Target a nominal 10 ms cadence; the next iteration will account for
         * the actual delay via SysTick-based delta above. */
        Delay_Ms(LOOP_PERIOD_MS);
    }
}
