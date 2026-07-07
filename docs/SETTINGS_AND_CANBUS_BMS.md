# Venus OS Settings Service Reference

Key D-Bus settings paths for battery integration and DVCC control.
Documented from live observation on Venus OS v3.71.

## Service

```
com.victronenergy.settings
```

Settings are read/write BusItems. Use `GetValue` to read and `SetValue`
to write. Many settings require a GX device reboot or service restart
to take effect.

## System Setup

| Path                                              | Type   | Live    | Description |
|---------------------------------------------------|--------|---------|-------------|
| `/Settings/SystemSetup/BatteryService`             | string | `com.victronenergy.battery/277` | Selected battery service for DVCC |
| `/Settings/SystemSetup/BmsInstance`                | int32  | -1      | BMS instance (-1 = auto-select) |
| `/Settings/SystemSetup/SharedVoltageSense`         | int32  | 1       | Share vebus voltage to all chargers |
| `/Settings/SystemSetup/SharedTemperatureSense`     | int32  | 1       | Share vebus temperature to all chargers |
| `/Settings/SystemSetup/AcInput1`                   | int32  | 1       | AC input 1 enabled |
| `/Settings/SystemSetup/AcInput2`                   | int32  | 0       | AC input 2 enabled |

## Battery Life (ESS)

| Path                                              | Type   | Live  | Description |
|---------------------------------------------------|--------|-------|-------------|
| `/Settings/CGwacs/BatteryLife/State`               | int32  | 2     | BatteryLife mode |
| `/Settings/CGwacs/BatteryLife/SocLimit`            | double | 30.0  | Minimum SOC (%) |
| `/Settings/CGwacs/MaxChargePercentage`             | double | 100.0 | Scale factor for BMS CCL |
| `/Settings/CGwacs/MaxDischargePercentage`          | double | 100.0 | Scale factor for BMS DCL |

### BatteryLife State values

| Value | Mode              | Description |
|-------|-------------------|-------------|
| 1     | Optimized         | Self-consumption optimization |
| 2     | Keep Batteries Charged | Maintain full SOC |
| 9     | Optimized with BatteryLife | Active SOC management |

## DVCC

| Path                                              | Type   | Live  | Description |
|---------------------------------------------------|--------|-------|-------------|
| `/Settings/Dvcc/ChargeCurrentLimit`               | double | --    | User override for CCL (A) |

Note: this path was not set on the observed system, meaning no user
override — the BMS CCL passes through unmodified.

## CAN bus

| Path                                              | Type   | Live | Description |
|---------------------------------------------------|--------|------|-------------|
| `/Settings/Canbus/can0/Profile`                   | int32  | --   | CAN profile (4 = CAN-bus BMS 500kbps) |
| `/Settings/Canbus/can0/Bitrate`                   | int32  | --   | CAN bitrate |

### CAN Profile values

| Value | Profile                    |
|-------|----------------------------|
| 0     | Disabled                   |
| 1     | VE.Can 250 kbps            |
| 2     | VE.Can 500 kbps            |
| 3     | CAN-bus BMS 250 kbps       |
| 4     | CAN-bus BMS 500 kbps       |
| 5     | RV-C                       |
| 6     | NMEA 2000                  |
| 7     | VE.Can + NMEA 2000         |

## GUI

| Path                                              | Type   | Description |
|---------------------------------------------------|--------|-------------|
| `/Settings/Gui/Gauges/Dc/System/Power/Max`         | double | Y-axis max for DC system power gauge |
| `/Settings/Gui/Gauges/Pv/Power/Max`                | double | Y-axis max for PV power gauge |

These are written dynamically by the GUI based on observed values.

---

# Victron CAN-bus BMS Service Internals

The Victron CAN-bus BMS service is the bridge between the CAN frames
(0x351/0x355/0x356/0x35A/0x372/0x373/0x35E/0x35F) and D-Bus. It lives
at `/opt/victronenergy/can-bus-bms/can-bus-bms` on Venus OS.

## Binary analysis (v3.71, 112,912 bytes)

Strings extracted from the binary reveal the full set of D-Bus paths
the service publishes, going beyond what community documentation
has previously recorded.

## D-Bus paths published by can-bus-bms

### Battery measurements

| Path                          | Source frame | Description |
|-------------------------------|-------------|-------------|
| `/Dc/0/Voltage`               | 0x356       | Battery voltage |
| `/Dc/0/Current`               | 0x356       | Battery current |
| `/Dc/0/Temperature`           | 0x356       | Battery temperature |
| `/Soc`                        | 0x355       | State of charge |
| `/Connected`                  | (timeout)   | CAN link status |

### DVCC limits

| Path                          | Source frame | Description |
|-------------------------------|-------------|-------------|
| `/Info/MaxChargeCurrent`      | 0x351       | Charge current limit |
| `/Info/MaxDischargeCurrent`   | 0x351       | Discharge current limit |
| `/Info/MaxChargeVoltage`      | 0x351       | Charge voltage limit |
| `/Info/BatteryLowVoltage`     | 0x351       | Low voltage threshold |

### Capabilities

| Path                                  | Description |
|---------------------------------------|-------------|
| `/Capabilities/ChargeVoltageControl`  | 1 (BMS controls charge voltage) |

### Alarms

| Path                            | Source frame | Description |
|---------------------------------|-------------|-------------|
| `/Alarms/LowVoltage`            | 0x35A       | Under-voltage |
| `/Alarms/HighVoltage`           | 0x35A       | Over-voltage |
| `/Alarms/LowTemperature`        | 0x35A       | Under-temperature |
| `/Alarms/HighTemperature`       | 0x35A       | Over-temperature |
| `/Alarms/LowChargeTemperature`  | 0x35A       | Low charge temperature |
| `/Alarms/HighChargeTemperature` | 0x35A       | High charge temperature |
| `/Alarms/HighChargeCurrent`     | 0x35A       | Charge over-current |
| `/Alarms/HighDischargeCurrent`  | 0x35A       | Discharge over-current |
| `/Alarms/CellImbalance`         | 0x35A       | Cell voltage imbalance |
| `/Alarms/InternalFailure`       | 0x35A       | Internal BMS fault |

### Cell-level data (previously undocumented)

These paths were discovered via binary string analysis. They confirm the
CAN-BMS service supports per-cell monitoring:

| Path                              | Likely source | Description |
|-----------------------------------|---------------|-------------|
| `/Cell/%zu`                       | Extended frame | Per-cell voltage (formatted with cell number) |
| `/System/MinCellVoltage`          | Extended frame | Minimum cell voltage |
| `/System/MaxCellVoltage`          | Extended frame | Maximum cell voltage |
| `/System/MinCellTemperature`      | Extended frame | Minimum cell temperature |
| `/System/MaxCellTemperature`      | Extended frame | Maximum cell temperature |
| `/System/MinVoltageCellId`        | Extended frame | Cell ID with lowest voltage |
| `/System/MaxVoltageCellId`        | Extended frame | Cell ID with highest voltage |
| `/System/MinTemperatureCellId`    | Extended frame | Cell ID with lowest temperature |
| `/System/MaxTemperatureCellId`    | Extended frame | Cell ID with highest temperature |

The `%zu` format in `/Cell/%zu` means per-cell voltages are published
as individual paths like `/Cell/1`, `/Cell/2`, etc. — different from
the dbus-serialbattery convention of `/Voltages/Cell1`.

### Alarm forwarding from Lynx BMS modules

| Path                                    | Description |
|-----------------------------------------|-------------|
| `/Alarms/Blown`                         | Lynx BMS blown fuse alarm |
| `/Alarms/ConnectionLost`                | Lynx BMS module lost |
| `/CellImbalance`                        | (standalone path, may be alternate) |
| `/MaxBatteryCurrent`                    | (standalone path) |
| `/MaxBatteryVoltage`                    | (standalone path) |
| `/MaxPvVoltage`                         | (standalone path) |
| `/MaxVoltage`                           | (standalone path) |
| `/MinBatteryVoltage`                    | (standalone path) |

### Additional sense paths

| Path                          | Description |
|-------------------------------|-------------|
| `/Sense/Voltage`              | BMS voltage sense |
| `/Sense/Current`              | BMS current sense |
| `/Sense/Soc`                  | BMS SOC sense |
| `/Sense/Temperature`          | BMS temperature sense |

### Module diagnostics

| Path                                    | Description |
|-----------------------------------------|-------------|
| `/Diagnostics/Module%d/Alarms`          | Per-module alarm status |
| `/Diagnostics/Module%d/HighCellVoltage` | Per-module high cell voltage alarm |

## Service architecture

```
CAN frames (0x351/0x355/0x356/0x35A/0x372/0x373/0x35E/0x35F)
        │
        ▼
/opt/victronenergy/can-bus-bms/can-bus-bms
        │  - Opens socketcan:vecan1
        │  - Parses Victron CAN-BMS frames
        │  - Registers com.victronenergy.battery.socketcan_<iface>
        │  - Publishes to D-Bus BusItem paths
        │
        ▼
    systemcalc reads /Info/MaxCharge*
    for DVCC control
```

The service uses `-vv` verbose logging and `--log-before 25 --log-after 25`
flags which capture 25 messages of context around warnings/errors.

## What we previously got wrong

Our earlier CAN_BMS_PROTOCOL.md stated the protocol "does not support
per-cell voltages." The binary strings prove the opposite: the CAN-BMS
service DOES publish `/Cell/%zu`, `/System/MinCellVoltage`, and related
cell-temperature paths. The frames that carry this data are the extended
0x372/0x373 frames (or additional frames beyond the basic four).

This means our `--can-bms` translator's 0x372/0x373 frames (added in PR #28)
are actually consumed by the stock service — the cell data IS published
to D-Bus by the Victron driver, just at `/Cell/N` rather than
`/Voltages/CellN`.

## Next steps for verification

1. Enable the can-bus-bms service on the Samsung SDI CAN interface
2. Send 0x372/0x373 frames via our translator
3. Check if `/Cell/1` through `/Cell/14` appear on D-Bus
4. Verify cell temperature paths populate

This would confirm the full protocol capability without needing to
reverse-engineer the binary further.
