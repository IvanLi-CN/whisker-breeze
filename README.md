# Whisker Breeze Fan Controller

Whisker Breeze is a CH32V003-based fan speed controller project bootstrapped with [ch32fun](https://github.com/cnlohr/ch32fun). The initial firmware is a "hello world" that periodically toggles a status LED on PC0 and prints a heartbeat string through the ch32fun debug printf channel.

## Repository Layout

- `firmware/` – application sources and build scripts.
- `ch32fun/` – ch32fun submodule providing the minimal hardware support code and tooling.
- `docs/` – project documentation and hardware notes. All images live in `docs/assets`.

## Documentation

- CH32V003 Reference Manual (Markdown)
  - `docs/ch32v003-reference-manual.md`
- CH32V003 Datasheet (Markdown)
  - `docs/ch32v003-datasheet.md`

Conventions
- Place Markdown documents directly under `docs/`.
- Place all images under `docs/assets/` and reference them as `assets/<filename>` from Markdown.
- Do not commit intermediate conversion output (e.g., `downloads/`, extracted PDFs, or per-doc image folders).

## Getting Started

1. Install a RISC-V GCC toolchain that provides `riscv64-unknown-elf-gcc` (or another supported prefix).
2. Initialize submodules after cloning the repository:
   ```sh
   git submodule update --init
   ```
3. Build the firmware and flash it with minichlink:
   ```sh
   cd firmware
   make        # builds whisker_breeze.bin/hex/elf artefacts
   make flash  # flashes via ch32fun's minichlink helper
   ```

The default build targets the CH32V003. Adjust `TARGET_MCU` in `firmware/Makefile` if you decide to migrate to a different chip.

## License

The Whisker Breeze sources are provided under the MIT license. The ch32fun submodule retains its original licensing.
