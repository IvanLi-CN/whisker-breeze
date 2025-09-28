# Whisker Breeze Project Overview
- **Purpose**: Firmware and hardware assets for the “Whisker Breeze” fan controller built around a WCH CH32V003F4U6 MCU. Firmware manages I2C peripherals (SSD1306 OLED, INA226 current sensor, CH224Q PD controller, external EEPROM), fan PWM, debounce logic, and persistence.
- **Tech Stack**: Bare-metal C firmware compiled with `riscv64-unknown-elf-gcc`, leveraging the `ch32fun` HAL submodule plus project-specific sources under `firmware/`.
- **Repo Structure**:
  - `firmware/`: main application (`whisker_breeze.c`), Makefile, build artefacts; `funconfig.h` drives HAL config.
  - `ch32fun/`: upstream HAL + utilities (submodule).
  - `docs/`: Markdown hardware notes, datasheets, wiring documentation.
  - `pcb/`, `downloads/`: hardware design collateral.
- **Entrypoints**: Firmware entry is `firmware/whisker_breeze.c`, linking against `ch32fun/ch32fun.c`.
- **Platform Notes**: Development host is macOS (Darwin) with `zsh`; toolchain expected on PATH (`riscv64-unknown-elf-*`).