# Task Completion Checklist
1. **Self-review**: Inspect diffs (`git diff`, `git status -sb`) to ensure only intended files changed; double-check against hardware docs.
2. **Formatting**: Run `clang-format -i` on modified C sources; verify Markdown is clean.
3. **Build/Test**: Execute `EXTRA_CFLAGS='-DDISPLAY_ORIENTATION_180=1' make -C firmware whisker_breeze.elf`. If flashing required, run `make flash` (optional hardware step).
4. **Documentation**: Update `docs/` when hardware interfaces or behaviour change; ensure assets under `docs/assets/`.
5. **Logs/Artefacts**: Do not commit generated binaries (`*.elf`, `*.bin`, `*.hex`, `*.map`); remove them after validation.
6. **Sync**: `git pull --rebase` before final commit; resolve conflicts.
7. **Commit**: Use Conventional Commit style (e.g., `docs: ...`, `fix: ...`). Avoid committing `.serena/` unless intentionally updating onboarding data.
8. **Share**: Provide hardware test notes (fan behaviour, EEPROM persistence) in PR description or comments.
9. **Clean Up**: Optionally run `make -C firmware clean` to leave tree tidy.