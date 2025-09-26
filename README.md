# Whisker Breeze Fan Controller

Whisker Breeze is a CH32V003-based fan speed controller, bootstrapped with
[ch32fun](https://github.com/cnlohr/ch32fun). The initial firmware is a small
"hello world": it drives a status LED on PC6 (open‑drain, active‑low) and prints
a heartbeat string via the ch32fun debug printf channel. After full
initialization and fan tach calibration, the LED encodes fan speed every 4 s
using 1–10 pulses (100 ms on, 200 ms off per pulse).

## Hardware

- Schematic (SVG, inline preview):

  <img src="pcb/whisker-breeze-sch.svg" alt="Whisker Breeze schematic" width="900" />

- EasyEDA Pro project:
  [pcb/whisker-breeze.epro](pcb/whisker-breeze.epro)

## Repository Layout

- `firmware/` - application sources and build scripts.
- `ch32fun/` - ch32fun submodule with minimal HAL and tooling (minichlink).
- `pcb/` - PCB resources: schematic SVG and EasyEDA Pro project (see above).
- `docs/` - documentation and hardware notes. Images in `docs/assets/`.

## Documentation

- CH32V003 Reference Manual (Markdown): `docs/ch32v003-reference-manual.md`
- CH32V003 Datasheet (Markdown): `docs/ch32v003-datasheet.md`

Docs conventions

- Place Markdown directly under `docs/`.
- Put images under `docs/assets/` and reference them as `assets/<filename>`.
- Avoid committing intermediate artefacts (e.g. `downloads/`).

## Getting Started

Prerequisites

- RISC-V GCC toolchain with `riscv64-unknown-elf-gcc` on `PATH`.
- Git submodules initialized (ch32fun):

  ```sh
  git submodule update --init --recursive
  ```

Build & Flash (from repository root)

```sh
make        # rebuilds firmware and flashes (proxy to firmware/)
```

Useful targets

- `make flash` - forces rebuild then programs the board via minichlink.
- `make clean` - removes build artefacts in `firmware/`.
- `make attach` - opens a persistent minichlink debug session.

Notes

- Default target MCU is `CH32V003` (see `firmware/Makefile`).
- Export `MINICHLINK` if using a custom minichlink binary location.

## License

The Whisker Breeze sources are provided under the MIT license. The `ch32fun`
submodule retains its original licensing.
