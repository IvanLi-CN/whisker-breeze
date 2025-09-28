# Style & Conventions
- **Formatting**: Follow repository `.clang-format` (Allman braces, 4-space indent, 120-column width). Run `clang-format -i <file.c>` before committing.
- **Naming**:
  - Functions/variables: `snake_case`.
  - Hardware macros/constants: uppercase (e.g., `PIN_DISPLAY_PWR`).
- **Logging**: Prefer short bracketed tags (e.g., `[mode]`, `[pd]`); emit via project-provided `emit_log` helper.
- **Firmware Structure**: Group GPIO bindings and hardware notes near top of each module; annotate non-obvious timing/register tweaks inline.
- **Comments**: Add concise explanatory comments only where behaviour is non-obvious (e.g., timing requirements, power sequencing).
- **Documentation**: Place Markdown in `docs/`, images in `docs/assets/`, use descriptive filenames (e.g., `ch32v003-datasheet.md`). Avoid committing generated binaries or PDFs.