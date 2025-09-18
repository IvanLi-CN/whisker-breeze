# Repository Guidelines

## Project Structure & Module Organization

- `firmware/` hosts CH32 application sources, board configuration, and the primary `Makefile`; generated artefacts (`*.elf`, `*.bin`, `*.hex`, `*.map`) should be cleaned before committing.
- `ch32fun/` is a git submodule delivering the hardware abstraction layer, build scripts, and the `minichlink` utility—run `git submodule update --init --recursive` after cloning or when toolchain files change.
- `docs/` captures wiring notes, display datasheets, and hardware experiments; update it whenever peripherals, pin maps, or measurement procedures evolve.
- The repository root `Makefile` proxies to `make -C firmware`; prefer it for quick builds from the top level.

## Build, Test, and Development Commands

- `make` — rebuilds the firmware and flashes it via `minichlink`; equivalent to `make -C firmware flash`.
- `make flash` — forces a firmware rebuild before programming the board; use after code or linker changes.
- `make clean` — removes artefacts in `firmware/` to guarantee a pristine rebuild.
- `make attach` — opens a persistent `minichlink` debug session for interactive inspection.
- Confirm `riscv64-unknown-elf-gcc` is on `PATH`, and export `MINICHLINK` if using a custom binary location.

## Coding Style & Naming Conventions

- Follow the repository `.clang-format`: Allman braces, 4-space indentation, 120-character lines.
- Adopt `snake_case` for functions and variables, while hardware macros and constants remain uppercase (e.g., `PIN_DISPLAY_PWR`).
- Group GPIO bindings and hardware notes at the top of each module; annotate non-obvious timing or register decisions inline.
- Run `clang-format -i <file.c>` before committing to keep diffs minimal.

## Testing Guidelines

- Automated unit tests are not defined; validate changes on hardware after every significant edit.
- Power-cycle the board, observe PWM ramp behaviour, and monitor UART heartbeat logs for regressions.
- Capture scope traces or RPM readings when behaviour changes, and document them in issues or PR descriptions.

## Commit & Pull Request Guidelines

- Write Conventional Commits (`feat:`, `fix:`, `docs:`) that describe the firmware-level impact.
- PRs should summarise verification steps (e.g., "Tested on CH32V003 @ 20 kHz PWM"), link related issues, and include supporting artefacts such as photos or capture dumps when introducing hardware changes.
- Keep changes focused; coordinate breaking register-map or toolchain updates before merging, and ensure submodules are updated with explicit commands in the PR narrative.
