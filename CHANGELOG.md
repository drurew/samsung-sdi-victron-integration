# Changelog

All notable changes to the Samsung SDI Victron Integration project will be documented in this file.

## [1.2.0] - 2026-01-22

### Added
- **Safety Watchdog**: Implemented strict CAN bus monitoring. Data is cleared if communication is lost for more than configured timeout (default 10s).
- **Linear Current Limiting**: Implemented smooth current reduction based on cell voltage and temperature to prevent BMS tripping at high/low SOC.
- **Dynamic Configuration**: All safety thresholds and timeouts are now configurable via `config.ini`.
- **Fail-Safe Aggregation**: Aggregator now reports 0V/0A/0% SOC if all batteries are lost/timed out, preventing stale data persistence.
- **VRM Manifest**: Added `samsung-sdi-driver.json` for easier identification in VRM.

### Changed
- Refactored `SamsungSDICANClient` to better handle timeouts and connection status.
- Updated `BatteryAggregator` to interpolate charge limits linearly.

## [1.1.0] - 2026-01-21

### Added
- **SetupHelper Integration**: Full support for Kwindrem's Package Manager (SetupHelper).
- **Universal Setup Script**: New `setup` script handling install/uninstall/reinstall logic locally.
- **CI/CD**: Added GitHub Actions workflow to automatically build release packages.
- **Packaging**: Added `create_package.sh` for manual package creation.
- **Documentation**: Updated installation guide for USB/Offline and Package Manager methods.

## [1.0.0] - 2025-12-28

### Added
- Initial release.
- CAN 2.0A communication with Samsung SDI modules.
- D-Bus integration for Victron Venus OS.
- Basic aggregation logic.
