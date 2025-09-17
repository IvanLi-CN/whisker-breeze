# Repository Guidelines

## Project Structure & Module Organization

- `firmware/` holds the application sources, CH32 target configuration, and the primary `Makefile`. Build artefacts (`.elf`, `.hex`, `.bin`, `.map`) are generated here; keep the directory clean before committing.
- `ch32fun/` is a git submodule supplying the hardware abstraction, build scripts, and `minichlink` flashing utility. Run `git submodule update --init` after cloning and before updating toolchain files.
- `docs/` stores wiring notes, display datasheets, and ancillary design material. Update these when introducing new hardware modes or peripherals.

## Build, Test, and Development Commands

```sh
make            # build + flash via firmware/Makefile (shortcut to `make -C firmware flash`)
make flash      # rebuild firmware and program the board with minichlink
make clean      # remove firmware artefacts to guarantee full rebuilds
make attach     # open a persistent minichlink session for interactive debugging
```

Ensure `riscv64-unknown-elf-gcc` (or another supported prefix) is on your `PATH`, and export `MINICHLINK` if using a non-default location.

## Coding Style & Naming Conventions

- C sources follow the repository `.clang-format` (Allman braces, 4-space alignment, 120-column limit). Run `clang-format -i <file.c>` before committing.
- Use `snake_case` for functions and variables, uppercase for hardware macros and constants (`PIN_DISPLAY_PWR`, `FAN_PWM_PERIOD_TICKS`).
- Keep hardware bindings grouped at the top of each module and document new GPIO assignments inline.

## Testing Guidelines

Automated unit tests are not yet defined. Exercise new logic on hardware after every significant change: power-cycle the board, verify PWM ramp behaviour, and watch the UART heartbeat for regressions. Note observed RPM, duty-cycle, and I²C status when filing issues.

## Commit & Pull Request Guidelines

- Follow Conventional Commits (`feat:`, `fix:`, `docs:`) as seen in the git history.
- Reference related issues, summarize hardware verification steps (e.g., "Tested on CH32V003 @ 20 kHz PWM"), and attach scope captures or photos when behaviour changes.
- Keep PRs focused; coordinate breaking register-map changes with maintainers before merging.

## Tooling & Dependency Tips

- Update submodules with `git submodule update --remote --merge` when syncing to upstream ch32fun.
- Pin your compiler version in the PR description when introducing toolchain-dependent changes to ease reviewer reproduction.
