# Changelog

## [2.0.0] - 2026-05-18

### Added
- **C driver** (`src/samsung-sdi-bms.c`): Native driver linking against libc only.
  Listens for PDO broadcasts from Samsung SDI BMS. 10 KB binary, 1 MB RSS,
  near-zero CPU. Zero runtime dependencies.
- **Protocol verification**: All CAN message fields verified against Samsung
  SDI Product Specification ELPM482-00005 Rev 0.2. Little-endian extraction,
  correct scaling factors, and alarm bit mapping confirmed.
- **Makefile** for native compilation on Cerbo GX.
- **Installation guide** (`docs/INSTALL.md`) with daemontools service setup.

### Changed
- **Repository restructured**: C driver in `src/`, documentation in `docs/`,
  scripts in `scripts/`. Python reference driver retained in `src/`.
- **D-Bus service name**: Simplified to `com.victronenergy.battery.samsung_sdi`
  for single-system use.

### Fixed
- **Cell voltage parser**: Removed out-of-bounds reads at offsets 8 and 10
  (max_tray_voltage, min_tray_voltage). These fields are defined in the Samsung
  spec but exceed the 8-byte CAN 2.0A frame; they are available on per-cell
  CAN IDs 0x510-0x55C.
- **Alarm bit 7**: Correctly mapped to Victron `CellImbalance` alarm path
  (Tray Voltage Imbalance per Samsung spec).

## [1.x] - 2025-2026

### Added
- Initial Python driver with CAN communication, D-Bus publishing, battery
  aggregation, and diagnostic tools.
