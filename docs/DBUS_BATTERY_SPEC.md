# Victron Venus OS Battery Service D-Bus Specification

Authoritative reference for the D-Bus interface a battery service must implement
to integrate with Victron Venus OS (tested on v3.71). Extracted from live Cerbo GX
hardware and Victron's own source code (`dbus-systemcalc-py`, `dummycanbms.py`,
`batteryservice.py`).

## Service name convention

```
com.victronenergy.battery.<identifier>
```

Where `<identifier>` is unique per battery system (e.g. `canopen_bms_node1`,
`samsung_sdi`, `socketcan_can0`).

## Interface: `com.victronenergy.BusItem`

Every path implements this interface:

| Method     | Returns | Description |
|------------|---------|-------------|
| `GetValue` | variant | Current value (int32, double, or string) |
| `GetText`  | variant | Human-readable formatted string (e.g. "13.08V") |
| `GetItems` | dict    | All paths and values as `a{sa{sv}}` |

| Signal          | Parameters       | Description |
|-----------------|------------------|-------------|
| `ItemsChanged`  | `a{sa{sv}}`      | Emitted when values change |

Values use the variant type appropriate to the path:
- `double` (DBus type `d`) for voltage, current, power, SOC, temperature
- `int32` (DBus type `i`) for counters, flags, device instance
- `string` (DBus type `s`) for names, versions

## How systemcalc identifies a BMS

Systemcalc checks whether `/Info/MaxChargeVoltage` is not `None`. If set, the
service is treated as a "smart BMS" capable of DVCC charge control. If `None`,
it is treated as a passive battery monitor.

## Path reference

### Required — monitored by systemcalc for DVCC

These paths are subscribed to by `dbus-systemcalc-py` via `batteryservice.py`:

| Path                            | Type   | Unit | Description |
|---------------------------------|--------|------|-------------|
| `/Soc`                          | double | %    | State of charge (0-100) |
| `/Dc/0/Voltage`                 | double | V    | Battery voltage |
| `/Dc/0/Current`                 | double | A    | Battery current (positive = charge, negative = discharge) |
| `/Dc/0/Power`                   | double | W    | Battery power (voltage * current) |
| `/Dc/0/Temperature`             | double | C    | Battery temperature |
| `/Info/MaxChargeCurrent`        | double | A    | DVCC charge current limit |
| `/Info/MaxDischargeCurrent`     | double | A    | DVCC discharge current limit |
| `/Info/MaxChargeVoltage`        | double | V    | DVCC charge voltage limit. **Must be set for BMS detection.** |
| `/Info/BatteryLowVoltage`       | double | V    | Low voltage threshold |
| `/DeviceInstance`               | int32  | --   | Unique instance number (0-255) |
| `/ProductId`                    | int32  | --   | Product identifier (0xB005 for lithium) |
| `/ProductName`                  | string | --   | Display name in device list |
| `/CustomName`                   | string | --   | User-editable name |
| `/InstalledCapacity`            | double | Ah   | Nominal capacity |
| `/System/MinCellVoltage`        | double | V    | Minimum cell voltage across all cells |
| `/System/MaxCellVoltage`        | double | V    | Maximum cell voltage across all cells |
| `/Capabilities/ChargeVoltageControl` | int32 | -- | Set to 1 if BMS controls charge voltage |

### Alarms

All alarm paths are `int32` with Victron convention:
- **0** = OK
- **1** = Warning
- **2** = Alarm

| Path                            | Type   | Description |
|---------------------------------|--------|-------------|
| `/Alarms/LowVoltage`            | int32  | Under-voltage |
| `/Alarms/HighVoltage`           | int32  | Over-voltage |
| `/Alarms/LowTemperature`        | int32  | Under-temperature |
| `/Alarms/HighTemperature`       | int32  | Over-temperature |
| `/Alarms/LowSoc`                | int32  | Low state of charge |
| `/Alarms/HighChargeCurrent`     | int32  | Charge over-current |
| `/Alarms/HighDischargeCurrent`  | int32  | Discharge over-current |
| `/Alarms/CellImbalance`         | int32  | Cell voltage imbalance |
| `/Alarms/InternalFailure`       | int32  | Internal BMS fault |
| `/Alarms/HighChargeTemperature` | int32  | High temperature during charge |
| `/Alarms/LowChargeTemperature`  | int32  | Low temperature during charge |

### System status

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/System/NrOfCellsPerBattery`         | int32  | Cells per module |
| `/System/NrOfModulesOnline`           | int32  | Operational modules |
| `/System/NrOfModulesOffline`          | int32  | Offline/faulted modules |
| `/System/NrOfModulesBlockingCharge`   | int32  | Modules blocking charge |
| `/System/NrOfModulesBlockingDischarge`| int32  | Modules blocking discharge |

### Connection

| Path               | Type   | Description |
|--------------------|--------|-------------|
| `/Connected`       | int32  | 1 = CAN/communication link active, 0 = disconnected |

### Charge control

| Path                            | Type   | Description |
|---------------------------------|--------|-------------|
| `/Io/AllowToCharge`             | int32  | 1 = charging permitted |
| `/Io/AllowToDischarge`          | int32  | 1 = discharging permitted |
| `/SystemSwitch`                 | int32  | 1 = system online |

### History

| Path                          | Type   | Unit | Description |
|-------------------------------|--------|------|-------------|
| `/History/ChargeCycles`       | int32  | --   | Cumulative charge cycles |
| `/History/TotalAhDrawn`       | double | Ah   | Total amp-hours drawn |
| `/History/DeepestDischarge`   | double | Ah   | Deepest discharge recorded |
| `/ConsumedAmphours`           | double | Ah   | Consumed amp-hours since full |

### Metadata

| Path                       | Type   | Description |
|----------------------------|--------|-------------|
| `/FirmwareVersion`         | string | BMS firmware version |
| `/HardwareVersion`         | string | Hardware identifier |
| `/Mgmt/ProcessName`        | string | Executable path |
| `/Mgmt/ProcessVersion`     | string | Driver version |
| `/Mgmt/Connection`         | string | Connection description (e.g. "CAN PDO 500kbps") |

### Per-cell voltages (dbus-serialbattery convention)

These paths are not consumed by systemcalc directly, but are displayed by the
GUI (GUI v2) when present. Based on the community `dbus-serialbattery` project:

| Path                  | Type   | Description |
|-----------------------|--------|-------------|
| `/Voltages/Cell1`     | double | Cell 1 voltage in V |
| `/Voltages/Cell2`     | double | Cell 2 voltage |
| ...                   | ...    | ... |
| `/Voltages/Cell14`    | double | Cell 14 voltage |
| `/Voltages/Sum`       | double | Sum of all cell voltages |
| `/Voltages/Diff`      | double | Max cell voltage - min cell voltage |

### Diagnostics (from Victron dummycanbms.py)

| Path                         | Type   | Description |
|------------------------------|--------|-------------|
| `/Diagnostics/Module0/Id`    | string | Module 0 identifier |
| `/Diagnostics/Module1/Id`    | string | Module 1 identifier |
| `/Diagnostics/Module2/Id`    | string | Module 2 identifier |
| `/Diagnostics/Module3/Id`    | string | Module 3 identifier |

## BMS detection flow

1. Systemcalc monitors `com.victronenergy.battery.*` services
2. When a new service appears, it reads `/Info/MaxChargeVoltage`
3. If not None, the service is classified as a BMS
4. Systemcalc subscribes to the 16 paths listed under "Required"
5. DVCC uses `/Info/MaxChargeCurrent`, `/Info/MaxDischargeCurrent`, and
   `/Info/MaxChargeVoltage` to control the inverter/charger

## CAN-BMS to D-Bus mapping

The Victron CAN-BMS protocol frames (0x351/0x355/0x356/0x35A) are consumed by
the `can-bus-bms` service (`/opt/victronenergy/can-bus-bms/can-bus-bms` on Venus
OS) which translates them to these D-Bus paths. The mapping is:

| CAN Frame | D-Bus Paths |
|-----------|-------------|
| 0x351     | `/Dc/0/Voltage`, `/Dc/0/Current`, `/Dc/0/Temperature`, `/Soc`, `/Connected` |
| 0x355     | `/Soc` (precision), `/Connected` |
| 0x356     | `/Info/MaxChargeVoltage`, `/Info/MaxChargeCurrent`, `/Info/MaxDischargeCurrent` |
| 0x35A     | All 11 `/Alarms/*` paths |

Paths NOT filled by CAN-BMS frames (must be set at service registration or
derived):
- `/Dc/0/Power` — computed from V * I
- `/ConsumedAmphours` — computed from capacity and SOC
- `/System/*` — set from config
- `/History/*` — accumulated by service
- `/Info/BatteryLowVoltage` — set from config
- `/Voltages/*` — per-cell data, not part of CAN-BMS protocol
- `/DeviceInstance`, `/ProductId`, `/ProductName` — set at registration
- `/Capabilities/ChargeVoltageControl` — set by service

## References

- Venus OS source: `victronenergy/dbus-systemcalc-py` (public GitHub)
  - `delegates/batteryservice.py` — path subscription list
  - `scripts/dummycanbms.py` — CAN-BMS test harness with full path set
  - `scripts/proxybattery.py` — battery proxy service
- Live Cerbo GX running Venus OS v3.71 with Super-B Epsilon V2 BMS
- VE.Can registers public document: `VE.Can-registers-public.pdf` (victronenergy.com)
