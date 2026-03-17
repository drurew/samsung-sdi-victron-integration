# Changelog

All notable changes to the Samsung SDI Victron Integration project will be documented in this file.

## [1.2.1] - 2026-03-17

### Fixed
- **BatteryAggregator startup crash** (#8): Fixed `AttributeError: 'BatteryAggregator' object has no attribute '_setup_dbus_service'` by properly implementing the D-Bus service initialization method.
- **Cleanup method error** (#11): Added missing `disconnect()` method to `SamsungSDICANClient` to prevent `AttributeError` on shutdown.
- **Wrong capacity units** (#12): Changed battery capacity from 4840.0 Wh to 94.0 Ah to match Victron D-Bus specification requirements.
- **Missing cell data** (#13): Added `/System/MinCellVoltage`, `/System/MaxCellVoltage`, `/System/MinCellTemperature`, `/System/MaxCellTemperature` D-Bus paths and implemented data collection from CAN messages 0x503 and 0x504.
- **Hardcoded current limits** (#10): Implemented dynamic charge/discharge current limits using temperature-adjusted values from CAN message 0x502 instead of hardcoded config values.
- **CAN protocol conflict** (#4): Fixed overlapping byte offsets in CAN message 0x503 definition for cell vs tray voltage fields.
- **Dependency installation** (#5): Updated `install.sh` to use `opkg install python3-can` for reliable dependency installation on Venus OS.

### Improved
- **Dynamic VE.Bus Discovery** (#6, #7): Implemented automatic detection of VE.Bus service names in `diagnose_charging.py` instead of hardcoding `com.victronenergy.vebus.ttyS4`. Now works across different hardware configurations (Cerbo GX, Raspberry Pi, USB, CAN).

## [1.2.0] - 2026-01-22

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
