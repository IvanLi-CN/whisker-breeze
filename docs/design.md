# Whisker Breeze Hardware and Firmware Design Specification

## System Overview

- **Microcontroller**: CH32V003F4U6 (RISC-V, WCH)
- **Project Codename**: Whisker Breeze
- **Firmware Stack**: ch32fun HAL with application firmware in C (`firmware/`)
- **Current Firmware Entrypoint**: `firmware/whisker_breeze.c`
- **Primary Power Domain**: 3V3 rail with local decoupling (C3 1 µF and C4 100 nF placed close to VDD/VSS)
- **Clock Source**: Internal RC oscillator; PA1 is repurposed for SSD1306 VBAT control and PA2 serves as an analog input; no external crystal is populated
- **Programming and Debug**: PD1/SWIO single-wire debug interface with PD7/NRST reset pin
- **Primary Peripherals**: I2C bus (SSD1306, INA226, CH224Q interrupt), PWM fan drive, discrete mode inputs, power good monitoring; UART is reserved but disabled by default

## Power and Clock Distribution

- SSD1306 VBAT is switched through a PMOS controlled by PA1 (`BAT`). Confirm gate polarity and any necessary inrush limiting.
- TPS22810DBVR load switch on PC5 (`FAN_EN`) gates the 5 V fan supply. EN/UVLO is active high and must not float. With CT = 2.2 nF and VIN = 5 V, RL = 10 Ω, the measured 10–90% rise time is ~0.2 ms. Firmware should assert FAN_EN for ≥1 ms before enabling PWM and deassert it when duty cycle returns to 0% so the internal QOD discharges the load.
- Internal RC oscillator provides the system clock; no external crystal is populated.

## Peripheral Interfaces

### Display Power and Reset

- SSD1306 reset (`RES`) is driven by PD0. VBAT switching is handled by PA1. The display uses I2C address 0x3C. A reset pulse of ≥3 µs with active-low polarity is required. Firmware should probe the display and gracefully skip display updates when absent.

### Fan Drive and Tachometer

- PWM output on PC3 (`TIM1_CH3`) should operate at 20–30 kHz to remain above audible range. Firmware applies a 0–100 ms linear ramp at power-on and then tracks the mode-derived target duty cycle.
- Tachometer input on PC4 captures open-drain pulses (two pulses per revolution typical). The pin is internally pulled up to 3V3, EXTI4 triggers on falling edges, and TIM2 free-running at 100 kHz (PSC = 479) measures the interval. Missing pulses for ≥500 ms invalidate RPM feedback. Readings above 4500 RPM or sudden drops below 300 RPM when the target duty is high are discarded as glitches. If duty > 0 and RPM remains 0, firmware increments the minimum duty baseline (default 100 RPM equivalent, capped at 20 000 RPM).

### Current and Power Monitoring

- INA226 is connected on I2C address 0x44 (7-bit). Shared bus speed is fixed at 400 kHz to satisfy INA226 and CH224Q timing. The shunt resistor is 10 mΩ. Firmware polls bus voltage, shunt current, and power every 200 ms and records results in the heartbeat log. Startup includes scanning 0x40–0x4F to confirm the active address. INA226 bus voltage readings ≥ ~11.5 V (RAW ≥ 9200) force the `12` status bit to true to indicate valid high-voltage input even if CH224Q negotiation fails.

### USB Power Delivery Monitor

- CH224Q power-good output (`VBUS_PG` on PC7) is open-drain with active-low polarity and requires an external pull-up. The current design uses 100 kΩ to 3V3. Firmware monitors this signal for PD negotiation status and power-loss handling.

### Mode and Configuration Inputs

- PD2 (`FAST`), PD3 (`MODE`), and PD4 (`SLOW`) are low-active inputs sourced from a TM-2024A multi-position switch. Each input enables internal pull-ups (~47 kΩ). Hardware omits RC filtering; firmware applies ≥5 ms debounce.

### UART Reserve

- PA5 (`TX`) and PD5 (`RX`) are reserved for UART logging and remain disabled unless explicitly enabled.

## GPIO Assignment

| MCU Pin | Net Label | Function | Notes |
| --- | --- | --- | --- |
| PD7/NRST/T2CH4 | NRST | External reset | Dedicated reset input |
| PA1/OSCI/A1 | BAT | SSD1306 VBAT switch control | Drives PMOS gate |
| PA2/OSCO/A0/T1CH2N | TS | Thermal sense input | ADC channel; external front-end pending |
| VSS | GND | Digital ground | Reference plane |
| PD0/T1CH1N/OPN1 | RES | SSD1306 reset | Active low |
| VDD | 3V3 | Supply | Primary rail |
| PC0/T2CH3/UTX_ | INT | Peripheral interrupt | Shared interrupt line |
| PC1/SDA/NSS/T2CH4 | SDA | I2C data | 4.7 kΩ pull-up to 3V3 |
| PC2/SCL/URTS/T1BKIN | SCL | I2C clock | 4.7 kΩ pull-up to 3V3 |
| PC3/T1CH3/T1CH1N_ | FAN_PWM | PWM output | TIM1 CH3 |
| PC4/A2/T1CH4/MCO | FAN_TACH | Fan tachometer | EXTI4, internal pull-up |
| PC5/SCK/T1ETR/T2CH1 | FAN_EN | Load switch enable | Controls TPS22810 |
| PC6/MOSI/T1CH1N_ | — | Reserved | Available for future use |
| PC7/MISO/T1CH2_ | VBUS_PG | Power-good from CH224Q | Open-drain, active low |
| PD1/SWIO/AETR2 | PD1 | SWIO debug | Confirm availability for GPIO reuse if needed |
| PD2/A3/T1CH1/T2CH3 | FAST | Mode input | Low-active |
| PD3/A4/T2CH2/AETR | MODE | Mode input | Low-active |
| PD4/A7/UCK/T2CH1ETR | SLOW | Mode input | Low-active |
| PA5/UTX | TX | UART TX | Disabled by default |
| PD5/URX | RX | UART RX | Disabled by default |

## Firmware Architecture

- **Startup Sequence**: `SystemInit()` configures clocking, peripheral clocks, and GPIO before entering the main loop.
- **Module Responsibilities**:
  - `board`: Consolidates pin, clock, and interrupt priority setup via `funGpioInitAll()`.
  - `drivers.i2c`: Initializes the shared I2C1 bus and arbitrates access for SSD1306, INA226, and CH224Q, including interrupt callback placeholders on PC0.
  - `drivers.display`: Handles SSD1306 initialization, reset sequencing, VBAT control, and basic graphics primitives with presence detection.
  - `drivers.pd`: Implements CH224Q sink negotiation, requests 12 V operation, and tracks PG/PD status.
  - `drivers.pwm`: Controls TIM1 CH3 duty cycle generation.
  - `drivers.adc`: Provides channel polling scaffolding (currently disabled pending thermal sensing enablement).
  - `drivers.uart`: Maintains optional UART logging and command interface (compile-time disabled).
  - `app.modes`: Translates SLOW/MODE/FAST inputs into normalized setpoints with interpolated smoothing.
  - `app.fan`: Executes soft-start, duty-cycle regulation, RPM computation via EXTI4/TIM2, and fault handling (RPM stalls, outliers).
  - `app.power`: Monitors VBUS_PG, aggregates INA226 telemetry, and publishes power-state information to the heartbeat log.
- **Logging**: UART logging remains disabled. When enabled, the heartbeat includes: `p` (phase), `t` (target %), `d` (duty %), `r` (RPM), `v` (VBUS presence flag), `12` (12 V status), `b` (INA226 bus register raw), `i` (current in mA), `c` (TIM1 CH3 compare value), `k` (debounced switch state).
- **Calibration**: On power-up, firmware performs a full-speed calibration after confirming `12 = 1`. After ≥2 s of stable operation (500 ms without RPM increase), it logs `[cal]rpm/tgt` and reverts to the greater of 10% duty or 100 RPM equivalent. Loss of PD or VBUS resets calibration.

## Operating Parameters

- PWM frequency: 20 kHz nominal (acceptable range 20–30 kHz).
- Soft-start: linear ramp from 0% to target over 0–100 ms after FAN_EN assertion.
- ADC sampling: infrastructure reserved; intended 200 ms cadence when thermal sensing is enabled.
- UART baud rate: unused.
- Mode input polarity: active low; confirm pull-ups enabled on PD2/PD3/PD4.
- VBUS_PG polarity: active low via CH224Q open-drain with external pull-up.

## Configuration Alignment

- Status LED support is removed; PC0 is strictly allocated to the shared interrupt line.
- UART initialization is suppressed unless explicitly required.
- I2C1 initialization accounts for all connected devices and enables only the features acknowledged during startup.

## Outstanding Information Required

1. Finalize the TS input front-end: confirm NTC value (10 kΩ @ 25 °C), β = 3950, bias network (3V3 via 8.2 kΩ), and any RC filtering (≈0.55 ms with 100 nF).
2. Verify INT source device, polarity (low-active), and pull-up value (4.7 kΩ to 3V3).
3. Confirm SSD1306 reset polarity (expected active low) and timing.
4. Validate TM-2024A wiring for SLOW/MODE/FAST, including optional press detection and pull-up strategy.
5. Confirm CH224Q PG pull-up implementation (100 kΩ to 3V3) and effective voltage thresholds.
6. Assess reuse eligibility for PD1 when SWIO is not in use.

## Verification Checklist

- Power-on self-test: verify firmware banner, VBUS status, PD negotiation, and fan RPM reporting.
- Mode input exercise: confirm debounce behavior and PWM scaling across SLOW/MODE/FAST positions.
- PWM sweep: validate audible performance across 10%–100% duty and correlate tachometer feedback.
- I2C validation: confirm SSD1306 initialization and interrupt handling for INA226/CH224Q.
- Programming interface: ensure SWIO/NRST accessibility without interference from adjacent circuitry.

## Reference Files

- Firmware entry: `firmware/whisker_breeze.c`
- Hardware notes repository: `docs/`
