# Auto versioning (date + git hash)

This patch makes the app version fully automatic on every build.

## Version format
- UI string: `YYYY.MM.DD+<gitsha>` (example: `2026.01.25+a1b2c3`)
- Windows numeric: `YYYY.MM.DD.REV` (REV changes every second)

## Notes
- The build generates `generated\version.h` and `generated\version.rc2`.
- `generated\` should be ignored by git.
