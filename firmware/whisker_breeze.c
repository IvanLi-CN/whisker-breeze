#include "ch32fun.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "i2c_lowlevel.h"
#include "eeprom_font_storage.h"
#include "eeprom_config.h"

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
#define SSD1306_USE_EXTERNAL_FONT 1u /* app-side only; submodule未依赖此宏 */

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

/*
 * 应用侧自定义字库渲染：不修改子模块，改为在本文件内提供
 * ssd1306_drawstr 的等价实现，并通过宏将调用重定向到这里。
 * 依赖 eeprom_font_storage.ssd1306_font_fetch() 从外置 EEPROM 取 8x8 字形。
 */
static inline void wb_drawchar_ext(uint8_t x, uint8_t y, uint8_t chr, uint8_t color)
{
    uint8_t rows[8];
    ssd1306_font_fetch(chr, rows);
    for (uint32_t i = 0; i < 8; ++i) {
        uint8_t row = rows[i];
        for (uint32_t j = 0; j < 8; ++j) {
            /* 字形按bit7为最高位 -> 左到右 */
            uint8_t bit = (uint8_t)(0x80u >> j);
            if (row & bit) {
                ssd1306_drawPixel((uint32_t)x + j, (uint32_t)y + i, color);
            } else {
                /* 背景保持不变：不清像素，避免闪烁；行级别先清屏 */
            }
        }
    }
}

static inline void wb_drawstr_ext(uint8_t x, uint8_t y, char *str, uint8_t color)
{
    if (!str) return;
    uint8_t cx = x;
    for (const char *p = str; *p; ++p) {
        /* 简单ASCII宽度8像素，超出宽度直接裁剪 */
        if ((uint32_t)cx >= SSD1306_W) break;
        wb_drawchar_ext(cx, y, (uint8_t)*p, color);
        cx = (uint8_t)(cx + 8);
    }
}

/* 将本文件后续对 ssd1306_drawstr 的调用全部重定向到应用侧实现 */
#define ssd1306_drawstr wb_drawstr_ext

/* Orientation: do software 180° rotation at flush time for correctness. */

#if DISPLAY_ORIENTATION_180
static inline uint8_t bitrev8(uint8_t v)
{
    v = (uint8_t)(((v & 0x55u) << 1) | ((v >> 1) & 0x55u));
    v = (uint8_t)(((v & 0x33u) << 2) | ((v >> 2) & 0x33u));
    v = (uint8_t)((v << 4) | (v >> 4));
    return v;
}
#endif

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
#if DISPLAY_ORIENTATION_180
            /* software 180°: reverse columns, reverse pages, reverse bits */
            const uint16_t pages = (uint16_t)(SSD1306_H / 8u); /* 5 */
            uint16_t x_src = (uint16_t)(SSD1306_W - 1u - xi);
            uint16_t p_src = (uint16_t)(pages - 1u - p);
            uint16_t addr  = x_src + (uint16_t)SSD1306_W * p_src;
            uint8_t  v     = bitrev8(ssd1306_buffer[addr]);
#else
            uint16_t addr = xi + (uint16_t)SSD1306_W * p; /* 直接取本页72字节 */
            uint8_t  v    = ssd1306_buffer[addr];
#endif
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

static inline void fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    uint8_t y2 = (uint8_t)(y + h);
    uint8_t x2 = (uint8_t)(x + w);
    for (uint8_t yy = y; yy < y2; ++yy) {
        for (uint8_t xx = x; xx < x2; ++xx) {
            ssd1306_drawPixel((uint32_t)xx, (uint32_t)yy, 1);
        }
    }
}

static void draw_settings_item(const char *label, const char *value, uint8_t y, bool invert_line, bool invert_value)
{
    /* Two-column layout: col0=labels (left), col1=values (right) */
    /* Column widths: 66% (labels), 34% (values) */
    const uint8_t col0_w = (uint8_t)(((uint16_t)SSD1306_W * 66u) / 100u);
    const uint8_t col1_w = (uint8_t)(SSD1306_W - col0_w);
    const uint8_t x_label = DISP_ORIGIN_X;
    const uint8_t x_value = (uint8_t)(DISP_ORIGIN_X + col0_w);
    size_t v_len = strlen(value);
    /* add vertical padding ±1px around 8px font for better readability */
    uint8_t y_pad = (y > 0) ? (uint8_t)(y - 1) : 0;
    uint8_t line_h = 10; /* 8px font + 2px padding */
    if (invert_line) {
        /* Invert entire line area with padding */
        fill_rect(DISP_ORIGIN_X, y_pad, SSD1306_W, line_h);
        /* Draw full text in color=0 over white background */
        ssd1306_drawstr(x_label, y, (char *)label, 0);
        ssd1306_drawstr(x_value, y, (char *)value, 0);
    } else {
        /* Draw label normally */
        ssd1306_drawstr(x_label, y, (char *)label, 1);
        if (invert_value) {
            /* Fill value area with padding and draw value inverted */
            uint8_t vx = x_value;
            uint8_t vw = (uint8_t)((v_len * 8u) > col1_w ? col1_w : (v_len * 8u));
            fill_rect(vx, y_pad, vw, line_h);
            ssd1306_drawstr(vx, y, (char *)value, 0);
        } else {
            ssd1306_drawstr(x_value, y, (char *)value, 1);
        }
    }
}

/* Generic auto-repeat helper: returns number of extra steps (>=0) to apply.
 * state: 0=idle, 1=initial (waiting), 2=repeat */
static int8_t repeat_steps(bool pressed, uint32_t delta_ms, uint32_t *hold_ms, uint8_t *state,
                           uint16_t initial_ms, uint16_t interval_ms)
{
    int8_t steps = 0;
    if (pressed) {
        if (*state == 0u) {
            *state = 1u; *hold_ms = 0u; steps = 1; /* first tap */
        } else if (*state == 1u) {
            uint32_t t = *hold_ms + delta_ms;
            if (t >= initial_ms) { *state = 2u; *hold_ms = t - initial_ms; }
            else { *hold_ms = t; }
        } else {
            uint32_t t = *hold_ms + delta_ms;
            while (t >= interval_ms) { steps++; t -= interval_ms; }
            *hold_ms = t;
        }
    } else { *state = 0u; *hold_ms = 0u; }
    return steps;
}


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

/* Tach glitch rejection: ignore edges closer than half of the theoretical
 * period at max RPM. Timer runs at FAN_TACH_TIMER_CLOCK_HZ and input has
 * FAN_TACH_PULSES_PER_REV pulses per revolution. */
#define TACH_MIN_VALID_TICKS ( (uint16_t)( ((60u * (uint32_t)FAN_TACH_TIMER_CLOCK_HZ) / ((uint32_t)FAN_TACH_PULSES_PER_REV * (uint32_t)FAN_TACH_MAX_RPM)) / 2u ) )

/* After accepting a period, ignore subsequent edges that arrive too soon
 * relative to that period (adaptive blanking). */
#define TACH_EDGE_MIN_PCT          60u  /* require >=60% of last good period */

/* RPM acceptance window relative to last good sample (percent). Values
 * outside this window require short confirmation before acceptance. */
#define RPM_ACCEPT_UP_PCT          160u  /* +60% step allowed immediately */
#define RPM_ACCEPT_DOWN_PCT        60u   /* -40% dip allowed immediately */
#define RPM_CANDIDATE_NEAR_PCT     10u   /* candidate consolidation window */
#define RPM_LP_SHIFT                2u    /* IIR: new = old + (x-old)/4 */
#define INA226_12V_RAW_THRESHOLD      9200u   /* 11.5 V / 1.25 mV */
/* Supply stability detection: consider VBUS "stable" when consecutive INA226
 * bus-voltage samples differ by no more than this delta for a small count. */
#define SUPPLY_STABLE_DELTA_MV        200     /* <=200 mV change considered stable */
#define SUPPLY_STABLE_REQUIRED_SAMPLES 2      /* require N consecutive stable samples */
/* Minimum voltage considered runnable once stable (works for 5V and above). */
#define SUPPLY_MIN_RUN_MV             4000
#define FAN_CALIBRATION_STABLE_MS     500u
#define FAN_CALIBRATION_MIN_MS        2000u
#define FAN_CALIBRATION_DELTA_RPM     25u

#ifndef INA226_I2C_ADDR
#define INA226_I2C_ADDR           0x40u
#endif

#if (INA226_I2C_ADDR != 0x40u) && (INA226_I2C_ADDR != 0x44u)
#error "INA226_I2C_ADDR must be 0x40 or 0x44"
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

/* Keep initial I2C conservative to maximize compatibility; can raise later. */
#define I2C1_SHARED_BUS_TARGET_HZ 100000u

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

#define MODE_DEBOUNCE_TICKS       2u   /* faster response for SLOW/FAST */
#define HOLD_DEBOUNCE_TICKS       1u   /* MODE尽量灵敏，轻按也能触发 */
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
#define DISPLAY_REFRESH_MS         120u   /* Limit OLED updates to reduce flicker */

/* Temperature band absolute limits (centi-°C) for settings */
#define TEMP_ABS_MIN_CX100          (-2000)
#define TEMP_ABS_MAX_CX100          (6000)

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
    uint32_t rpm_smooth;
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
    /* Supply stability tracking */
    int32_t last_bus_mv_for_stable;
    uint8_t stable_count;
    bool voltage_stable;
    bool fault;
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
    volatile uint16_t good_period_ticks;
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
    .rpm_smooth = 0u,
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
    .last_bus_mv_for_stable = 0,
    .stable_count = 0,
    .voltage_stable = false,
    .fault = false,
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
    .good_period_ticks = 0,
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

/* Persisted state restore (applied after calibration) */
static control_mode_t g_restore_mode = CONTROL_MODE_TEMP;
static fix16_t        g_restore_manual_target = FIX16_ONE;
static bool           g_restore_pending = false;

/* Config persistence throttle */
static uint32_t g_cfg_last_save_ms = 0u;
static fix16_t  g_cfg_last_saved_manual = 0; /* initialized on first save */

/* Forward decls for helpers used before their full definitions */
static void emit_log(const char *fmt, ...);
extern uint32_t g_display_sleep_timeout_ms;
/* Temperature-band offset used by AUTO mapping; full definition appears below. */
static int16_t g_temp_band_offset_cx100;
static int32_t g_temp_band_accum_cx100; /* legacy accumulator; defined later */
static int16_t g_auto_min_cx100 = 2000; /* 默认 20°C */
static int16_t g_auto_max_cx100 = 4000; /* 默认 40°C */

/* 设置界面状态 */
typedef enum {
    SETTINGS_INACTIVE = 0,
    SETTINGS_SELECT,
    SETTINGS_EDIT
} settings_mode_t;
static settings_mode_t g_settings_mode = SETTINGS_INACTIVE;
static uint8_t         g_settings_index = 0; /* 0=min,1=max,2=sleep */

__attribute__((noinline)) static void clamp_offset_to_band(void)
{
    int32_t min_off = (int32_t)TEMP_ABS_MIN_CX100 - (int32_t)g_auto_min_cx100;
    int32_t max_off = (int32_t)TEMP_ABS_MAX_CX100 - (int32_t)g_auto_max_cx100;
    if (g_temp_band_offset_cx100 < (int16_t)min_off) g_temp_band_offset_cx100 = (int16_t)min_off;
    if (g_temp_band_offset_cx100 > (int16_t)max_off) g_temp_band_offset_cx100 = (int16_t)max_off;
}

static inline uint16_t ratio_to_q8_8(fix16_t r)
{
    if (r <= 0) return 0u;
    if (r >= FIX16_ONE) return 256u;
    return (uint16_t)(((int64_t)r * 256 + (FIX16_ONE / 2)) >> FIX16_FRAC_BITS);
}

static inline fix16_t q8_8_to_ratio(uint16_t q)
{
    if (q >= 256u) return FIX16_ONE;
    return (fix16_t)(((int64_t)q << FIX16_FRAC_BITS) / 256);
}

static void persist_save_now(const char *tag)
{
    (void)tag;
    eeprom_runtime_cfg_t cfg;
    cfg.version = 2u;
    cfg.last_mode = (g_mode == CONTROL_MODE_MANUAL) ? CFG_MODE_MANUAL : CFG_MODE_TEMP;
    cfg.manual_ratio_q8_8 = ratio_to_q8_8(g_manual_target);
    cfg.temp_band_offset_cx100 = g_temp_band_offset_cx100;
    cfg.auto_min_cx100 = g_auto_min_cx100;
    cfg.auto_max_cx100 = g_auto_max_cx100;
    uint32_t sleep_s = g_display_sleep_timeout_ms / 1000u;
    if (sleep_s < 5u) sleep_s = 5u;
    if (sleep_s > 60u) sleep_s = 60u;
    cfg.display_sleep_s = (uint8_t)sleep_s;
    if (eeprom_cfg_save(&cfg)) {
        g_cfg_last_save_ms = g_uptime_ms;
        g_cfg_last_saved_manual = g_manual_target;
        
#if WB_LOG_ENABLE
        emit_log("[cfg]save");
#endif
    }
}

static void persist_save_throttled(const char *tag, uint32_t min_interval_ms)
{
    uint32_t now = g_uptime_ms;
    if ((now - g_cfg_last_save_ms) >= min_interval_ms) {
        persist_save_now(tag);
    }
}

/* -------------------------------------------------------------------------- */
/* I2C diagnostics (optional)                                                 */
/* -------------------------------------------------------------------------- */
#if I2C_DIAG_ENABLE
static void i2c_diag_log_levels(const char *stage)
{
    int sda = funDigitalRead(PIN_I2C_SDA);
    int scl = funDigitalRead(PIN_I2C_SCL);
    int busy = (I2C1->STAR2 & I2C_STAR2_BUSY) ? 1 : 0;
    emit_log("[i2c]%s,sda=%d,scl=%d,busy=%d", stage, sda, scl, busy);
}

static bool i2c_probe_addr(uint8_t addr7)
{
    if (!i2c1_wait_not_busy()) {
        return false;
    }
    I2C1->CTLR1 |= I2C_CTLR1_START;
    if (!i2c1_wait_flag(0x00030001u)) {
        return false;
    }
    I2C1->DATAR = (uint16_t)((uint16_t)addr7 << 1);
    bool ok = i2c1_wait_flag(0x00070082u);
    /* Issue STOP regardless */
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return ok;
}
#endif

/* Optional stuck-bus recovery (SDA held low at boot) */
#if I2C_RECOVER_ENABLE
static void i2c_bus_recover(void)
{
    /* Disable I2C peripheral to get direct GPIO control */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;

    /* Configure pins: SCL as OD output, SDA as input with pull-up */
    funPinMode(PIN_I2C_SCL, GPIO_CFGLR_OUT_10Mhz_OD);
    funPinMode(PIN_I2C_SDA, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_I2C_SDA, FUN_HIGH);

    /* Pulse SCL until SDA releases (max 12 clocks). */
    for (int i = 0; i < 12; ++i) {
        if (funDigitalRead(PIN_I2C_SDA)) break;
        funDigitalWrite(PIN_I2C_SCL, FUN_LOW);
        Delay_Ms(1);
        funDigitalWrite(PIN_I2C_SCL, FUN_HIGH);
        Delay_Ms(1);
    }

    /* STOP: SDA low -> SCL high -> SDA high */
    funPinMode(PIN_I2C_SDA, GPIO_CFGLR_OUT_10Mhz_OD);
    funDigitalWrite(PIN_I2C_SDA, FUN_LOW);
    Delay_Ms(1);
    funDigitalWrite(PIN_I2C_SCL, FUN_HIGH);
    Delay_Ms(1);
    funDigitalWrite(PIN_I2C_SDA, FUN_HIGH);

    /* Restore AF-OD and re-enable I2C */
    funPinMode(PIN_I2C_SDA, GPIO_CFGLR_OUT_10Mhz_AF_OD);
    funPinMode(PIN_I2C_SCL, GPIO_CFGLR_OUT_10Mhz_AF_OD);
    I2C1->CTLR1 |= I2C_CTLR1_PE;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);
}
#endif

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
/* 屏幕省电：默认亮屏，上电后一段时间无按键则熄屏；任意按键唤醒。
 * - 熄屏下，按中键仅唤醒且消耗该次事件；其它键唤醒且不消耗。
 */
static bool     g_display_awake = true;     /* 运行期亮/熄屏状态 */
static uint32_t g_display_idle_ms = 0u;     /* 距离上次有效输入的空闲时间 */
uint32_t g_display_sleep_timeout_ms = 30000u; /* 可调 5..60 s */
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
#ifndef WB_LOG_ENABLE
#define WB_LOG_ENABLE 0
#endif

static void emit_log(const char *fmt, ...)
{
#if WB_LOG_ENABLE
    if (!DidDebuggerAttach()) {
        return;
    }
    uint32_t seconds = g_uptime_ms / 1000u;
    uint32_t millis = g_uptime_ms % 1000u;
    while (!DebugPrintfBufferFree()) {
        poll_input();
    }
    printf("[%05lu.%03lu] ", (unsigned long)seconds, (unsigned long)millis);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\r\n");
    while (!DebugPrintfBufferFree()) {
        poll_input();
    }
#else
    (void)fmt;
#endif
}

/* panic() removed to save flash; all INA paths now fail-soft with LED fault. */

static void log_pd_snapshot(const char *reason)
{
    emit_log("[pd]%s,%d,%d",
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
    emit_log("[fan]%d,%u,%u,%u,%u,%u,%u,%d,%d,%04X",
             g_fan.phase,
             (unsigned)target_pct,
             (unsigned)duty_pct,
             (unsigned)rpm_now,
             (unsigned)temp_c,
             (unsigned)g_temp.adc_raw,
             (unsigned)g_temp.adc_avg,
             g_power.vbus_valid ? 1 : 0,
             g_pd.have_12v ? 1 : 0,
             (unsigned)bus_raw);
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
        
#if WB_LOG_ENABLE
        emit_log("[ina]%s,%02X", stage, (unsigned)g_ina.address);
#endif
        g_ina.last_error_report_ms = (now == 0u) ? 1u : now;
    }
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

/* Forward declaration for LED fault override. */
static void led_set_fault_mode(bool on);

static void ina226_update(uint32_t delta_ms)
{
    if (!g_ina.configured) {
        /* Configure at current address; on failure, try the alternate strap (0x40/0x44 only). */
        if (!ina226_configure()) {
            g_ina.valid = false;
            g_ina.online_announced = false;
            if (g_ina.config_failures < UINT32_MAX) {
                g_ina.config_failures++;
            }
            g_ina.raw_bus_reg = 0u;
            ina226_report_error("configure");
            /* Attempt the other strap address exactly once. */
            if (g_ina.address == 0x40u) {
                
#if WB_LOG_ENABLE
                emit_log("[ina]try,44");
#endif
                g_ina.address = 0x44u;
                if (!ina226_configure()) {
                    g_ina.fault = true;
                    led_set_fault_mode(true);
                    return;
                }
            } else {
                
#if WB_LOG_ENABLE
                emit_log("[ina]try,40");
#endif
                g_ina.address = 0x40u;
                if (!ina226_configure()) {
                    g_ina.fault = true;
                    led_set_fault_mode(true);
                    return;
                }
            }
        }
        g_ina.configured = true;
        g_ina.config_failures = 0u;
        g_ina.sample_failures = 0u;
        g_ina.poll_timer_ms = 0u;
        g_ina.fault = false;
        led_set_fault_mode(false);
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
        g_ina.config_failures = 0u;
        g_ina.bus_voltage_mv = 0;
        g_ina.shunt_voltage_uw = 0;
        g_ina.current_ma = 0;
        g_ina.power_mw = 0;
        g_ina.raw_bus_reg = 0u;
        g_ina.last_bus_mv_for_stable = 0;
        g_ina.stable_count = 0;
        g_ina.voltage_stable = false;
        if (g_ina.sample_failures < UINT32_MAX) {
            g_ina.sample_failures++;
        }
        ina226_report_error("sample");
        /* On repeated failures, the next pass will reconfigure and run address fallback.
           Engage fault LED if both addresses later fail in configure stage. */
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

    /* Update supply stability: require small delta across consecutive samples. */
    {
        bool prev_stable = g_ina.voltage_stable;
        int32_t prev = g_ina.last_bus_mv_for_stable;
        int32_t diff = bus_mv - prev;
        if (diff < 0) diff = -diff;
        if (prev != 0 && diff <= SUPPLY_STABLE_DELTA_MV) {
            if (g_ina.stable_count < 0xFF) {
                g_ina.stable_count++;
            }
        } else {
            g_ina.stable_count = 0;
            /* On first valid sample prev==0; don't mark unstable beyond reset. */
        }
        g_ina.last_bus_mv_for_stable = bus_mv;
        if (g_ina.stable_count >= SUPPLY_STABLE_REQUIRED_SAMPLES) {
            g_ina.voltage_stable = true;
        }
        if (!prev_stable && g_ina.voltage_stable) {
        emit_log("[vbus]stable,%ldmV", (long)bus_mv);
        }
    }
    if (!g_ina.online_announced) {
        long bus_whole = bus_mv / 1000;
        long bus_frac = bus_mv % 1000;
        
#if WB_LOG_ENABLE
        emit_log("[ina]%ld.%03ldV", bus_whole, bus_frac);
#endif
        g_ina.online_announced = true;
        g_ina.last_error_report_ms = 0u;
    }
}


/* -------------------------------------------------------------------------- */
/* Debounce helpers                                                           */
/* -------------------------------------------------------------------------- */
static bool debounce_update_ticks(debounce_t *db, bool raw_level, uint8_t ticks)
{
    if (raw_level == db->stable_state) {
        db->counter = 0;
        return false;
    }

    if (db->counter < ticks) {
        db->counter++;
        if (db->counter >= ticks) {
            db->stable_state = raw_level;
            db->counter = 0;
            return true;
        }
    }
    return false;
}

/* （移除旧的兼容包装，节省空间） */

/* -------------------------------------------------------------------------- */
/* Display helpers                                                            */
/* -------------------------------------------------------------------------- */
static void display_power(bool enable)
{
    /* BAT 低=使能，BAT 高=断电省电。 */
    if (enable) {
        funDigitalWrite(PIN_DISPLAY_PWR, FUN_LOW);   /* 使能供电 */
        funDigitalWrite(PIN_DISPLAY_RESET, FUN_LOW); /* 先保持复位 */
        Delay_Ms(5);
        funDigitalWrite(PIN_DISPLAY_RESET, FUN_HIGH);
    } else {
        /* 断电 OLED 以降低功耗。 */
        funDigitalWrite(PIN_DISPLAY_PWR, FUN_HIGH);
    }
}

/* 进入/退出熄屏：用 0xAE/0xAF 控制控制器显示输出 */
static void display_set_awake(bool awake)
{
    if (g_display_awake == awake) {
        return;
    }
    g_display_awake = awake;
    if (awake) {
        /* 将在 display_try_init() 中重新上电并初始化 */
        g_display_probe_attempted = false;
        g_display_initialized = false;
    } else {
        /* 若当前仍上电，先逻辑关屏，再断电以省电 */
        if (g_display_initialized) {
            (void)ssd1306_cmd(SSD1306_DISPLAYOFF);
        }
        display_power(false); /* BAT 拉高省电 */
        g_display_initialized = false;
    }
}

/* 根据空闲时间自动熄屏/保持亮屏 */
static void display_idle_update(uint32_t delta_ms)
{
    /* 任一稳定按键按下 → 认为有持续输入，重置空闲计时 */
    if (g_controls.decrease_input.stable_state ||
        g_controls.increase_input.stable_state ||
        g_controls.hold_input.stable_state) {
        g_display_idle_ms = 0u;
    } else {
        if (g_display_idle_ms < UINT32_MAX - delta_ms)
            g_display_idle_ms += delta_ms;
        else
            g_display_idle_ms = UINT32_MAX;
    }

    uint32_t timeout = g_display_sleep_timeout_ms;
    if (timeout < 5000u) timeout = 5000u;
    if (timeout > 60000u) timeout = 60000u;
    if (g_display_awake && g_display_idle_ms >= timeout) {
        display_set_awake(false);
    }
}

static void display_render(void)
{
    if (!g_display_initialized || !g_display_awake) {
        return;
    }
    /* Throttle refresh to avoid visible flicker when numbers fluctuate. */
    static uint32_t s_last_ms = 0u;
    uint32_t now = g_uptime_ms;
    if (s_last_ms != 0u) {
        uint32_t dt = now - s_last_ms;
        if (dt < DISPLAY_REFRESH_MS) {
            return;
        }
    }
    s_last_ms = now;

    char line[12];
    ssd1306_setbuf(0);

    /* 设置界面渲染优先 */
    if (g_settings_mode != SETTINGS_INACTIVE) {
        /* (No header to save flash) */

        char v0[8], v1[8], v2[8];
        /* Values: display as integer °C and seconds */
        int min_c = g_auto_min_cx100 / 100;
        int max_c = g_auto_max_cx100 / 100;
        uint32_t sleep_s = g_display_sleep_timeout_ms / 1000u;
        snprintf(v0, sizeof v0, "%d", min_c);
        snprintf(v1, sizeof v1, "%d", max_c);
        snprintf(v2, sizeof v2, "%lu", (unsigned long)sleep_s);

        bool editing = (g_settings_mode == SETTINGS_EDIT);
        /* Line positions with extra line-height: y=6,18,30 */
        draw_settings_item("Min", v0, (uint8_t)(DISP_ORIGIN_Y + 6),  (!editing && g_settings_index == 0), ( editing && g_settings_index == 0));
        draw_settings_item("Max", v1, (uint8_t)(DISP_ORIGIN_Y + 18), (!editing && g_settings_index == 1), ( editing && g_settings_index == 1));
        draw_settings_item("Sleep", v2, (uint8_t)(DISP_ORIGIN_Y + 30), (!editing && g_settings_index == 2), ( editing && g_settings_index == 2));

        ssd1306_flush_window();
        return;
    }

    uint16_t set_pct = percent_from_ratio(g_manual_target);
    uint16_t out_pct = percent_from_ratio(g_fan.current_duty);
    uint32_t rpm_now = (g_fan.rpm_smooth != 0u) ? g_fan.rpm_smooth : g_fan.rpm;

    snprintf(line, sizeof line, "Set %3u%%", (unsigned)set_pct);
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 0, line, 1);

    snprintf(line, sizeof line, "Out %3u%%", (unsigned)out_pct);
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 8, line, 1);

    snprintf(line, sizeof line, "RPM %4lu", (unsigned long)rpm_now);
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 16, line, 1);

    /* 第四行：显示控制模式（不加标签）。AUTO/Manual/Calib */
    if (g_mode == CONTROL_MODE_TEMP) { snprintf(line, sizeof line, "Auto"); }
    else if (g_mode == CONTROL_MODE_MANUAL) { snprintf(line, sizeof line, "Manual"); }
    else { snprintf(line, sizeof line, "Calib"); }
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 24, line, 1);

    /* 第五行：显示电压电流（无标签）。格式固定为 "xx.xV yyyymA"，总长<=12。 */
    if (g_ina.valid) {
        int32_t mv = g_ina.bus_voltage_mv;
        int32_t ma = g_ina.current_ma;
        if (mv < 0) mv = 0; /* Clamp to 0 for display */
        if (ma < 0) ma = 0;
        long v_int = (long)(mv / 1000);
        long ma_int = (long)ma; /* already rounded to 1 mA in INA driver */
        if (ma_int < 0) ma_int = 0;
        if (ma_int > 9999) ma_int = 9999; /* clamp to fit width */
        /* Compact: integer volts to save space */
        snprintf(line, sizeof line, "V%ld %ldmA",
                 v_int, ma_int);
    } else {
        /* 传感器无效时显示空值，不加任何标签 */
        snprintf(line, sizeof line, "--.-V ----mA");
    }
    ssd1306_drawstr(DISP_ORIGIN_X, DISP_ORIGIN_Y + 32, line, 1);

    ssd1306_flush_window();
}

static void display_try_init(void)
{
    if (!g_display_awake) {
        return; /* 熄屏状态不尝试初始化（保持 BAT 高电平） */
    }
    if (g_display_initialized) {
        return;
    }

    if (g_display_unavailable) {
        if (!g_display_disabled_logged) {
#if WB_LOG_ENABLE
            emit_log("[disp] skip (disabled)");
#endif
            g_display_disabled_logged = true;
        }
        return;
    }

    if (!g_display_probe_attempted) {
#if WB_LOG_ENABLE
        emit_log("[disp] probe");
#endif
    }

    g_display_probe_attempted = true;
    display_power(true);
    Delay_Ms(120);

    g_display_error_seen = false;
    if (ssd1306_i2c_init() == 0) {
        /* Ensure shared bus timing is applied before any transaction. */
        i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);
        const uint8_t probe_cmd = 0xAE; /* Display OFF */
        int probe_result = ssd1306_pkt_send(&probe_cmd, 1, 1);
        if (!g_display_error_seen && probe_result == 0) {
            /* 使用 72x40 专用初始化序列：多路复用=0x27、COMpins=0x12、偏移Y=14、列偏移=30 */
            ssd1306_init_72x40_custom();
            ssd1306_setbuf(0);
            g_display_initialized = true;
            g_display_disabled_logged = false;
#if WB_LOG_ENABLE
            emit_log("[disp]ok");
#endif
            /* IMPORTANT: ssd1306_i2c_init() forces I2C1 to ~1MHz. Restore shared bus speed. */
            i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);

            /* 初始化成功，按当前逻辑状态决定是否点亮。*/
            if (!g_display_awake) {
                (void)ssd1306_cmd(SSD1306_DISPLAYOFF);
            }
            /* 后续由常规渲染路径 display_render() 输出内容。*/
            return;
        }
    }

    g_display_initialized = false;
    g_display_unavailable = true;
    display_power(false);
    g_display_disabled_logged = true;
    if (g_display_error_seen) {
#if WB_LOG_ENABLE
        emit_log("[disp] timeout");
#endif
    } else {
#if WB_LOG_ENABLE
        emit_log("[disp] missing");
#endif
    }
    /* Restore (or enforce) I2C speed even on failure. */
    i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);
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
    ssd1306_cmd(SSD1306_VOFFSET);                 // vertical offset (fixed)

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

/* Fault override: when true, LED blinks at 0.5 Hz (1s ON, 1s OFF). */
static bool g_led_fault_mode = false;
static uint32_t g_led_fault_ms = 0u;
static bool g_led_fault_on = false;

static void led_set_fault_mode(bool on)
{
    if (g_led_fault_mode == on) {
        return;
    }
    g_led_fault_mode = on;
    g_led_fault_ms = 0u;
    g_led_fault_on = false;
    if (!on) {
        led_off();
    }
}

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
    if (g_led_fault_mode) {
        /* 0.5 Hz blink: toggle every 1000 ms. */
        g_led_fault_ms += delta_ms;
        if (g_led_fault_ms >= 1000u) {
            g_led_fault_ms -= 1000u;
            g_led_fault_on = !g_led_fault_on;
            if (g_led_fault_on) {
                led_on();
            } else {
                led_off();
            }
        }
        return;
    }

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
        /* Verbose LED pulse count log can be enabled if needed. */
        /* emit_log("[led]%u", (unsigned)g_led.pulses_target); */
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

    /* Ensure I2C1 clock and pins are configured up-front so any early
     * EEPROM/INA access has valid AF-OD on PC1/PC2. */
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
    funPinMode(PIN_I2C_SDA, GPIO_CFGLR_OUT_10Mhz_AF_OD);
    funPinMode(PIN_I2C_SCL, GPIO_CFGLR_OUT_10Mhz_AF_OD);

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

    /* SLOW/FAST 使用默认去抖，MODE 使用更快去抖以提高轻按识别率 */
    debounce_update_ticks(&g_controls.decrease_input, dec_pressed, MODE_DEBOUNCE_TICKS);
    debounce_update_ticks(&g_controls.increase_input, inc_pressed, MODE_DEBOUNCE_TICKS);
    debounce_update_ticks(&g_controls.hold_input, hold_pressed, HOLD_DEBOUNCE_TICKS);

    /* 基于稳定态计算上升沿，用于唤醒与模式切换判定 */
    static bool s_last_dec = false;
    static bool s_last_inc = false;
    static bool s_last_hold = false;

    bool dec_now = g_controls.decrease_input.stable_state;
    bool inc_now = g_controls.increase_input.stable_state;
    bool hold_now = g_controls.hold_input.stable_state;

    bool dec_rise = (!s_last_dec) && dec_now;
    bool inc_rise = (!s_last_inc) && inc_now;
    bool hold_rise = (!s_last_hold) && hold_now;

    /* 记录稳定态按键掩码变化以便定位键值读取问题（0b SLOW|MODE|FAST）。*/
    static uint8_t s_last_stable_mask = 0u;
    uint8_t stable_mask = (dec_now ? 0x1u : 0u) | (hold_now ? 0x2u : 0u) | (inc_now ? 0x4u : 0u);
    if (stable_mask != s_last_stable_mask) {
        s_last_stable_mask = stable_mask;
#if WB_LOG_ENABLE
        emit_log("[key]%u", (unsigned)stable_mask);
#endif
    }

    /* 记录原始电平变化：仅关心 MODE（2）位，便于定位硬件/采样问题 */
    static uint8_t s_last_raw_mask = 0u;
    uint8_t raw_mask_now = g_controls_raw_mask;
    if (raw_mask_now != s_last_raw_mask) {
        s_last_raw_mask = raw_mask_now;
#if WB_LOG_ENABLE
        emit_log("[keyr]%u", (unsigned)raw_mask_now);
#endif
    }

    /* 熄屏下：任意键上升沿立即点亮；中键不再吞掉事件，以便可直接切换模式 */
    if (!g_display_awake && (dec_rise || inc_rise || hold_rise)) {
        display_set_awake(true);
        g_display_idle_ms = 0u;
    }

    /* MODE 的短按/长按在 ui_update() 中统一处理，这里不再直接切换模式。 */

    s_last_dec = dec_now;
    s_last_inc = inc_now;
    s_last_hold = hold_now;
}

/* -------------------------------------------------------------------------- */
/* UI state transitions (settings, long/short press)                          */
/* -------------------------------------------------------------------------- */
static void ui_update(uint32_t delta_ms)
{
    /* Track MODE press duration for long/short discrimination */
    enum { MODE_LONG_PRESS_MS = 800u };
    /* No repeat while editing values to ensure precise 1°C/1s steps */

    static uint32_t mode_hold_ms = 0u;
    static bool     last_hold = false;
    static bool     mode_long_fired = false; /* fire long event immediately upon threshold */
    static bool     prev_dec = false;
    static bool     prev_inc = false;

    bool dec_now = g_controls.decrease_input.stable_state;
    bool inc_now = g_controls.increase_input.stable_state;
    bool hold_now = g_controls.hold_input.stable_state;

    /* Long/short event detection on MODE */
    bool long_evt = false;
    bool short_evt = false;
    if (hold_now) {
        if (mode_hold_ms < UINT32_MAX - delta_ms) mode_hold_ms += delta_ms; else mode_hold_ms = UINT32_MAX;
        if (!mode_long_fired && mode_hold_ms >= MODE_LONG_PRESS_MS) {
            long_evt = true;          /* fire immediately on threshold */
            mode_long_fired = true;   /* suppress short on release */
        }
    } else if (last_hold) {
        if (!mode_long_fired && mode_hold_ms > 0u) {
            short_evt = true;         /* short only if no long was fired */
        }
        mode_hold_ms = 0u;
        mode_long_fired = false;
    }
    last_hold = hold_now;

    if (long_evt) {
        if (g_settings_mode == SETTINGS_INACTIVE) {
            /* Fold current AUTO offset into absolute min/max so Settings shows the
             * effective band. Then reset runtime offset to zero. */
            if (g_temp_band_offset_cx100 != 0) {
                int32_t new_min = (int32_t)g_auto_min_cx100 + (int32_t)g_temp_band_offset_cx100;
                int32_t new_max = (int32_t)g_auto_max_cx100 + (int32_t)g_temp_band_offset_cx100;
                /* Clamp to absolute bounds */
                if (new_min < TEMP_ABS_MIN_CX100) new_min = TEMP_ABS_MIN_CX100;
                if (new_min > TEMP_ABS_MAX_CX100) new_min = TEMP_ABS_MAX_CX100;
                if (new_max < TEMP_ABS_MIN_CX100) new_max = TEMP_ABS_MIN_CX100;
                if (new_max > TEMP_ABS_MAX_CX100) new_max = TEMP_ABS_MAX_CX100;
                /* Ensure ordering and at least 1°C span */
                if (new_max <= new_min) new_max = new_min + 100;
                if (new_max > TEMP_ABS_MAX_CX100) {
                    new_max = TEMP_ABS_MAX_CX100;
                    if (new_min >= new_max) new_min = new_max - 100;
                }
                g_auto_min_cx100 = (int16_t)new_min;
                g_auto_max_cx100 = (int16_t)new_max;
                /* Reset legacy/runtime offset and its accumulator */
                g_temp_band_offset_cx100 = 0;
                g_temp_band_accum_cx100 = 0;
                /* No immediate save required; leaving Settings will persist. */
            }
            g_settings_mode = SETTINGS_SELECT;
            g_settings_index = 0;
            g_display_idle_ms = 0u;
            display_set_awake(true);
            /* Reset nav edge tracking to avoid stale edges */
            prev_dec = g_controls.decrease_input.stable_state;
            prev_inc = g_controls.increase_input.stable_state;
        } else {
            /* Long press exits settings immediately from any substate. */
            g_settings_mode = SETTINGS_INACTIVE;
            persist_save_now("settings");
            /* Clear nav state */
            prev_dec = g_controls.decrease_input.stable_state;
            prev_inc = g_controls.increase_input.stable_state;
        }
    } else if (short_evt) {
        if (g_settings_mode == SETTINGS_INACTIVE) {
            /* Short press toggles AUTO/MANUAL (if not calibrating). */
            if (g_mode != CONTROL_MODE_CALIB) {
                g_mode = (g_mode == CONTROL_MODE_TEMP) ? CONTROL_MODE_MANUAL : CONTROL_MODE_TEMP;
                persist_save_throttled("mode", 1000u);
            }
        } else {
            /* In settings: short press toggles select <-> edit */
            if (g_settings_mode == SETTINGS_SELECT) {
                g_settings_mode = SETTINGS_EDIT;
                /* Reset nav edges to avoid accidental moves */
                prev_dec = g_controls.decrease_input.stable_state;
                prev_inc = g_controls.increase_input.stable_state;
            } else {
                g_settings_mode = SETTINGS_SELECT;
                persist_save_throttled("settings", 1000u);
                /* Reset edges */
                prev_dec = g_controls.decrease_input.stable_state;
                prev_inc = g_controls.increase_input.stable_state;
            }
        }
    }

    /* Rising edges for SLOW/FAST (debounced only; no multi-step on hold) */
    bool dec_rise = (!prev_dec) && dec_now;
    bool inc_rise = (!prev_inc) && inc_now;
    prev_dec = dec_now;
    prev_inc = inc_now;

    /* Selection movement */
    if (g_settings_mode == SETTINGS_SELECT) {
        if (dec_rise) {
            g_settings_index = (uint8_t)((g_settings_index + 3 - 1) % 3);
        }
        if (inc_rise) {
            g_settings_index = (uint8_t)((g_settings_index + 1) % 3);
        }
    }

    /* Value editing */
    if (g_settings_mode == SETTINGS_EDIT) {
        enum { EDIT_REPEAT_INITIAL_MS = 250u, EDIT_REPEAT_INTERVAL_MS = 120u };
        static uint32_t dec_hold_ms = 0u, inc_hold_ms = 0u;
        static uint8_t  dec_state = 0u,  inc_state = 0u;

        int step_units = 0; /* negative for decrease, positive for increase */
        /* first tap/hold managed by helper */
        int8_t add_dec = repeat_steps(g_controls.decrease_input.stable_state, delta_ms,
                                       &dec_hold_ms, &dec_state,
                                       EDIT_REPEAT_INITIAL_MS, EDIT_REPEAT_INTERVAL_MS);
        int8_t add_inc = repeat_steps(g_controls.increase_input.stable_state, delta_ms,
                                       &inc_hold_ms, &inc_state,
                                       EDIT_REPEAT_INITIAL_MS, EDIT_REPEAT_INTERVAL_MS);
        step_units += (int)add_inc;
        step_units -= (int)add_dec;

        if (step_units != 0) {
            if (g_settings_index == 0) {
                int32_t v = (int32_t)g_auto_min_cx100 + (int32_t)step_units * 100;
                if (v < TEMP_ABS_MIN_CX100) v = TEMP_ABS_MIN_CX100;
                if (v > TEMP_ABS_MAX_CX100) v = TEMP_ABS_MAX_CX100;
                g_auto_min_cx100 = (int16_t)v;
                /* Ensure min < max */
                if (g_auto_min_cx100 >= g_auto_max_cx100) {
                    int32_t new_max = (int32_t)g_auto_min_cx100 + 100;
                    if (new_max > TEMP_ABS_MAX_CX100) new_max = TEMP_ABS_MAX_CX100;
                    g_auto_max_cx100 = (int16_t)new_max;
                }
                /* Clamp offset to new band limits */
                clamp_offset_to_band();
            } else if (g_settings_index == 1) {
                int32_t v = (int32_t)g_auto_max_cx100 + (int32_t)step_units * 100;
                if (v < TEMP_ABS_MIN_CX100) v = TEMP_ABS_MIN_CX100;
                if (v > TEMP_ABS_MAX_CX100) v = TEMP_ABS_MAX_CX100;
                g_auto_max_cx100 = (int16_t)v;
                /* Ensure min < max */
                if (g_auto_max_cx100 <= g_auto_min_cx100) {
                    int32_t new_min = (int32_t)g_auto_max_cx100 - 100;
                    if (new_min < TEMP_ABS_MIN_CX100) new_min = TEMP_ABS_MIN_CX100;
                    g_auto_min_cx100 = (int16_t)new_min;
                }
                /* Clamp offset to new band limits */
                clamp_offset_to_band();
            } else {
                int32_t s = (int32_t)(g_display_sleep_timeout_ms / 1000u) + (int32_t)step_units;
                if (s < 5) s = 5;
                if (s > 60) s = 60;
                g_display_sleep_timeout_ms = (uint32_t)s * 1000u;
            }
            persist_save_throttled("settings", 1000u);
            g_display_idle_ms = 0u; /* treat as activity */
        }
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

/* Map temperature (centi-°C) to target ratio using adjustable band [min,max]. */
#define TEMP_BAND_ADJ_RATE_CX100_S   50    /* 0.5 °C/s (legacy band offset) */

static int16_t g_temp_band_offset_cx100 = 0;     /* legacy-only; kept for persistence */
static int32_t g_temp_band_accum_cx100 = 0;      /* legacy accumulator */

static inline int16_t temp_band_low(void)
{
    return (int16_t)(g_auto_min_cx100 + g_temp_band_offset_cx100);
}
static inline int16_t temp_band_high(void)
{
    return (int16_t)(g_auto_max_cx100 + g_temp_band_offset_cx100);
}

static fix16_t temp_target_ratio_from_temp(int16_t temp_c_x100)
{
    int16_t lo = temp_band_low();
    int16_t hi = temp_band_high();
    if (temp_c_x100 <= lo) {
        return 0;
    }
    if (temp_c_x100 >= hi) {
        return FIX16_ONE;
    }
    int32_t span = (int32_t)hi - (int32_t)lo; /* 2000 */
    if (span <= 0) {
        return FIX16_ONE;
    }
    int32_t pos = (int32_t)temp_c_x100 - (int32_t)lo; /* 0..span */
    fix16_t frac = fix16_div(fix16_from_int(pos), fix16_from_int(span));
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
    /* Keep a small history of periods (ticks) for median filtering. */
    enum { TACH_BUF_N = 5 };
    static uint16_t period_buf[TACH_BUF_N];
    static uint8_t  period_len = 0; /* 0..5 */

    /* Candidate RPM that needs short confirmation when far from last_good_rpm */
    static uint32_t candidate_rpm = 0u;
    static uint8_t  candidate_score = 0u;

    tach_timeout += delta_ms;

    if (g_tach.sample_ready) {
        uint16_t period = g_tach.period_ticks;
        g_tach.sample_ready = 0;
        if (period >= TACH_MIN_VALID_TICKS) {
            /* Reset timeout on any plausible period to avoid false invalidation. */
            tach_timeout = 0;
            /* Push to history (keep newest at end). */
            if (period_len < TACH_BUF_N) {
                period_buf[period_len++] = period;
            } else {
                memmove(&period_buf[0], &period_buf[1], (TACH_BUF_N - 1) * sizeof(uint16_t));
                period_buf[TACH_BUF_N - 1] = period;
            }

            /* Sort small buffer (n<=5). */
            uint8_t n = period_len;
            uint16_t tmp[TACH_BUF_N];
            for (uint8_t i = 0; i < n; ++i) { tmp[i] = period_buf[i]; }
            for (uint8_t i = 1; i < n; ++i) {
                uint16_t key = tmp[i];
                int8_t j = (int8_t)i - 1;
                while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; --j; }
                tmp[j + 1] = key;
            }
            /* Trimmed mean: drop min/max when n>=5; else use median/mean fallback. */
            uint16_t period_est_ticks = 0;
            if (n >= 5) {
                uint32_t sum = 0;
                for (uint8_t i = 1; i < n - 1; ++i) sum += tmp[i];
                period_est_ticks = (uint16_t)(sum / (uint32_t)(n - 2));
            } else if (n == 4) {
                period_est_ticks = (uint16_t)((tmp[1] + tmp[2]) / 2u);
            } else if (n == 3) {
                period_est_ticks = tmp[1];
            } else if (n == 2) {
                period_est_ticks = (uint16_t)((tmp[0] + tmp[1]) / 2u);
            } else {
                period_est_ticks = tmp[0];
            }

            /* Convert to RPM using estimated period. */
            uint64_t numerator = (uint64_t)FAN_TACH_TIMER_CLOCK_HZ * 60u;
            uint64_t denominator = (uint64_t)period_est_ticks * FAN_TACH_PULSES_PER_REV;
            uint32_t rpm = (denominator == 0u) ? 0u : (uint32_t)(numerator / denominator);
            if (rpm > FAN_TACH_MAX_RPM) {
                rpm = FAN_TACH_MAX_RPM;
            }

            /* Static hard cap to kill extreme spikes */
            if (rpm > FAN_RPM_GLITCH_UPPER_RPM) {
                /* Ignore outliers above trusted range */
            } else {
                bool accept = false;
                if (last_good_rpm == 0u) {
                    accept = (rpm > 0u);
                } else {
                    /* Reject sudden unrealistically low reading when commanded speed is above min. */
                    if (last_good_rpm >= FAN_RPM_GLITCH_MIN_BASE &&
                        rpm < FAN_RPM_GLITCH_MIN_BASE &&
                        g_manual_target > g_fan_min_ratio) {
                        accept = false;
                    } else {
                        /* Relative acceptance window */
                        uint32_t up_lim   = (last_good_rpm * RPM_ACCEPT_UP_PCT) / 100u;
                        uint32_t down_lim = (last_good_rpm * RPM_ACCEPT_DOWN_PCT) / 100u;
                        if (rpm >= down_lim && rpm <= up_lim) {
                            accept = true;
                            candidate_score = 0u;
                        } else {
                            /* Require two consecutive near-equal candidates */
                            uint32_t near_lo = (rpm * (100u - RPM_CANDIDATE_NEAR_PCT)) / 100u;
                            uint32_t near_hi = (rpm * (100u + RPM_CANDIDATE_NEAR_PCT)) / 100u;
                            if (candidate_score > 0u &&
                                candidate_rpm >= near_lo && candidate_rpm <= near_hi) {
                                candidate_score++;
                            } else {
                                candidate_rpm = rpm;
                                candidate_score = 1u;
                            }
                            if (candidate_score >= 2u) {
                                accept = true;
                                candidate_score = 0u;
                            }
                        }
                    }
                }

                if (accept) {
                    g_fan.rpm = rpm;
                    g_fan.rpm_valid = true;
                    last_good_rpm = rpm;
                    g_tach.good_period_ticks = period_est_ticks;
                    /* Smooth for display/logging without affecting peak detection. */
                    if (g_fan.rpm_smooth == 0u) {
                        g_fan.rpm_smooth = rpm;
                    } else {
                        int32_t diff = (int32_t)rpm - (int32_t)g_fan.rpm_smooth;
                        g_fan.rpm_smooth = (uint32_t)((int32_t)g_fan.rpm_smooth + (diff >> RPM_LP_SHIFT));
                    }
                }
            }
        }
    }

    if (tach_timeout >= FAN_TACH_TIMEOUT_MS) {
        tach_timeout = 0;
        g_fan.rpm_valid = false;
        g_fan.rpm = 0u;
        g_fan.rpm_smooth = 0u;
        last_good_rpm = 0u;
        period_len = 0u;
        candidate_rpm = 0u;
        candidate_score = 0u;
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

    /* 三模式：测速→温控/手动；MODE只在温控/手动切换 */
    if (g_mode == CONTROL_MODE_CALIB) {
        g_manual_target = FIX16_ONE; /* 全速测峰值 */
    } else if (g_mode == CONTROL_MODE_TEMP) {
        /* AUTO: allow HOT/COLD shift using SLOW/FAST to offset the configured [min,max]. */
        int dir = 0;
        if (g_controls.decrease_input.stable_state) dir += 1;  /* SLOW → hotter */
        if (g_controls.increase_input.stable_state) dir -= 1;  /* FAST → colder */
        if (dir != 0) {
            int32_t delta = (int32_t)delta_ms * (int32_t)TEMP_BAND_ADJ_RATE_CX100_S * dir;
            g_temp_band_accum_cx100 += delta;
            while (g_temp_band_accum_cx100 >= 1000) {
                g_temp_band_offset_cx100 += 1; /* +1 centi-°C */
                g_temp_band_accum_cx100 -= 1000;
            }
            while (g_temp_band_accum_cx100 <= -1000) {
                g_temp_band_offset_cx100 -= 1;
                g_temp_band_accum_cx100 += 1000;
            }
            /* Clamp offset so that shifted [min,max] stays within absolute bounds. */
            clamp_offset_to_band();
            /* Persist occasionally */
            persist_save_throttled("band", 1000u);
        }
        /* AUTO map temp against (min+offset, max+offset) */
        fix16_t auto_target = temp_target_ratio_from_temp(g_temp.temp_c_x100);
        g_manual_target = fix16_clamp(auto_target, 0, FIX16_ONE);
    } else {
        /* 手动：1% 步进，支持连发（初始延时+重复节拍），更稳定好控 */
        const fix16_t MANUAL_STEP_Q16 = (FIX16_ONE / 100); /* 1% */
        enum { MAN_REPEAT_INITIAL_MS = 250u, MAN_REPEAT_INTERVAL_MS = 120u };
        static uint32_t man_dec_ms = 0u, man_inc_ms = 0u;
        static uint8_t  man_dec_state = 0u, man_inc_state = 0u;

        int step = 0;
        int8_t add_dec = repeat_steps(g_controls.decrease_input.stable_state, delta_ms,
                                       &man_dec_ms, &man_dec_state,
                                       MAN_REPEAT_INITIAL_MS, MAN_REPEAT_INTERVAL_MS);
        int8_t add_inc = repeat_steps(g_controls.increase_input.stable_state, delta_ms,
                                       &man_inc_ms, &man_inc_state,
                                       MAN_REPEAT_INITIAL_MS, MAN_REPEAT_INTERVAL_MS);
        step += (int)add_inc;
        step -= (int)add_dec;

        if (step != 0) {
            fix16_t delta = (fix16_t)((int64_t)MANUAL_STEP_Q16 * step);
            g_manual_target = fix16_clamp(g_manual_target + delta, 0, FIX16_ONE);
            if (g_manual_target < g_fan_min_ratio) {
                g_manual_target = g_fan_min_ratio;
            }
        }
        /* Persist manual target occasionally when it changes notably */
        fix16_t diff = fix16_abs(g_manual_target - g_cfg_last_saved_manual);
        if (diff > (FIX16_ONE / 50)) { /* >2% change */
            persist_save_throttled("manual", 2000u);
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
        /* 不再强制 12V；仅在电压稳定后开始测速 */
        if (!(g_ina.valid && g_ina.voltage_stable)) {
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
            /* Apply restored mode/target if requested */
            if (g_restore_pending) {
                if (g_restore_mode == CONTROL_MODE_MANUAL) {
                    g_manual_target = fix16_clamp(g_restore_manual_target, g_fan_min_ratio, FIX16_ONE);
                    g_mode = CONTROL_MODE_MANUAL;
                    emit_log("[mode]manual");
                }
                g_restore_pending = false;
            }
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

    /* Treat VBUS as valid if either PG pin says so, or INA226 reports
     * a stable supply above a safe minimum (e.g., >=4.0 V).
     * This removes the hard dependency on external PG wiring/12V. */
    bool pg_ok = POWER_GOOD_ACTIVE_LOW ? pg_raw : !pg_raw;
    bool ina_ok = (g_ina.valid && g_ina.voltage_stable && g_ina.bus_voltage_mv >= SUPPLY_MIN_RUN_MV);
    g_power.vbus_valid = pg_ok || ina_ok;
    g_power.ina_alert = INA_INT_ACTIVE_LOW ? ina_raw : !ina_raw;

    if (!g_power.vbus_valid) {
        /* Only on falling edge do we reset fan state and stability tracking. */
        if (prev_vbus) {
            fan_apply_pwm(0, 0u);
            g_fan.phase = FAN_PHASE_SOFT_START;
            g_fan.soft_timer_ms = 0;
            g_fan.rpm_max = 0u;
            g_fan.rpm_valid = false;
            fan_calibration_reset();
            g_ina.last_bus_mv_for_stable = 0;
            g_ina.stable_count = 0;
            g_ina.voltage_stable = false;
        }
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
        /* Only reset calibration if supply info is invalid/unstable, not based on 12V. */
        if (!(g_ina.valid && g_ina.voltage_stable)) {
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
        /* Only reset calibration if supply info is invalid/unstable, not based on 12V. */
        if (!(g_ina.valid && g_ina.voltage_stable)) {
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
        /* Ignore unrealistically short or too-early periods (contact bounce / EMI). */
        uint16_t min_ticks = TACH_MIN_VALID_TICKS;
        uint16_t good = g_tach.good_period_ticks;
        if (good > 0u) {
            uint32_t adaptive = ((uint32_t)good * TACH_EDGE_MIN_PCT) / 100u;
            if (adaptive > min_ticks) {
                min_ticks = (adaptive > 0xFFFFu) ? 0xFFFFu : (uint16_t)adaptive;
            }
        }
        if (delta >= min_ticks) {
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
    /* Ensure I2C pins (PC1/PC2) are configured to AF-OD before any EEPROM/INA access. */
    (void)ssd1306_i2c_init();
    i2c1_configure_speed(I2C1_SHARED_BUS_TARGET_HZ);
    Delay_Ms(5);
#if I2C_DIAG_ENABLE
    i2c_diag_log_levels("boot");
#endif
#if I2C_RECOVER_ENABLE
    if (!funDigitalRead(PIN_I2C_SDA)) {
        i2c_bus_recover();
    }
#endif
    /* Quick address probe only when diagnostics are enabled. */
#if I2C_DIAG_ENABLE
    emit_log("[i2c]probe,50=%d,3C=%d,40=%d,44=%d",
             i2c_probe_addr(0x50) ? 1 : 0,
             i2c_probe_addr(0x3C) ? 1 : 0,
             i2c_probe_addr(0x40) ? 1 : 0,
             i2c_probe_addr(0x44) ? 1 : 0);
#endif

    bool font_ready = eeprom_wait_ready(50u);
        if (font_ready) {
        uint8_t probe_rows[8];
        if (!font_storage_fetch_rows('0', probe_rows)) {
#if WB_LOG_ENABLE
            emit_log("[ee] font missing");
#endif
            font_ready = false;
        }
    } else {
#if WB_LOG_ENABLE
        emit_log("[ee] no ack");
#endif
    }
    /* Try load persisted control state even if font rows are missing. */
    eeprom_runtime_cfg_t cfg;
    if (eeprom_cfg_load(&cfg)) {
        g_restore_mode = (cfg.last_mode == CFG_MODE_MANUAL) ? CONTROL_MODE_MANUAL : CONTROL_MODE_TEMP;
        g_restore_manual_target = q8_8_to_ratio(cfg.manual_ratio_q8_8);
        /* 将持久化的手动目标同步到运行期的“最近保存”字段，便于随时切入 MANUAL 时恢复 */
        g_cfg_last_saved_manual = g_restore_manual_target;
        /* Legacy offset persisted for compatibility (now used as shift on configured band). */
        g_temp_band_offset_cx100 = cfg.temp_band_offset_cx100;
        /* Load adjustable band endpoints and sleep timeout */
        int32_t min_c = cfg.auto_min_cx100;
        int32_t max_c = cfg.auto_max_cx100;
        if (min_c < TEMP_ABS_MIN_CX100) min_c = TEMP_ABS_MIN_CX100;
        if (min_c > TEMP_ABS_MAX_CX100) min_c = TEMP_ABS_MAX_CX100;
        if (max_c < TEMP_ABS_MIN_CX100) max_c = TEMP_ABS_MIN_CX100;
        if (max_c > TEMP_ABS_MAX_CX100) max_c = TEMP_ABS_MAX_CX100;
        /* ensure sane ordering */
        if (max_c <= min_c) max_c = min_c + 100;
        if (max_c > TEMP_ABS_MAX_CX100) {
            max_c = TEMP_ABS_MAX_CX100;
            if (min_c >= max_c) min_c = max_c - 100;
        }
        g_auto_min_cx100 = (int16_t)min_c;
        g_auto_max_cx100 = (int16_t)max_c;
        /* Clamp offset to new band limits */
        clamp_offset_to_band();
        uint32_t sleep_s = cfg.display_sleep_s;
        if (sleep_s < 5u) sleep_s = 30u; /* default if unset */
        if (sleep_s > 60u) sleep_s = 60u;
        g_display_sleep_timeout_ms = sleep_s * 1000u;
        g_restore_pending = true;
#if WB_LOG_ENABLE
        emit_log("[cfg]ok");
#endif
    } else {
        
#if WB_LOG_ENABLE
        emit_log("[cfg]none");
#endif
    }
    if (!font_ready) {
        g_display_error_seen = true;
    }

    /* Optional: attach debugger wait removed for size. */

    
#if WB_LOG_ENABLE
    emit_log("[boot] start %s", __TIME__);
#endif
    power_update();
    /* 上电先进入测速模式 */
    g_mode = CONTROL_MODE_CALIB;
    fan_calibration_reset();
    emit_log("[mode]init calib");
    ch224_poll(CH224_POLL_INTERVAL_MS);
    /* Defer INA first contact to the main loop after I2C/display bring-up */
    log_pd_snapshot("boot");
    log_fan_snapshot();
    g_log.have_last = true;
    reset_fan_log_timer();

    /* startup banner removed to save flash */

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

        /* 先处理按键，保证在任何供电/转态下都能识别 MODE 切换与唤醒 */
        controls_update();
        ui_update(delta_ms);

        power_update();
        tach_update(delta_ms);
        ch224_poll(delta_ms);
        /* Initialize display/I2C early in the loop so the bus is configured for other devices. */
        display_try_init();
        ina226_update(delta_ms);
        temp_update(delta_ms);
        fan_update(delta_ms);
        poll_input();
        display_idle_update(delta_ms);
        display_render();
        led_update(delta_ms);
        heartbeat_log(delta_ms);
        
        /* Target a nominal 10 ms cadence; the next iteration will account for
         * the actual delay via SysTick-based delta above. */
        Delay_Ms(LOOP_PERIOD_MS);
    }
}
/* Build-time diagnostics toggle (0 reduces flash). */
#ifndef I2C_DIAG_ENABLE
#define I2C_DIAG_ENABLE 0
#endif
