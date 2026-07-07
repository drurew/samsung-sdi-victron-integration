# Venus OS D-Bus API Reference

Complete D-Bus interface reference for Victron Venus OS (tested on v3.75,
Cerbo GX). Extracted from live hardware introspection and cross-referenced
with Victron's own source code (`dbus-systemcalc-py`).

## Architecture

```
┌──────────┐   ┌──────────┐   ┌──────────┐
│ BMS      │   │ MPPT     │   │ MultiPlus│
│ (battery)│   │ (solarch)│   │ (vebus)  │
└────┬─────┘   └────┬─────┘   └────┬─────┘
     │ D-Bus         │ D-Bus       │ D-Bus
     ▼               ▼             ▼
┌─────────────────────────────────────────┐
│            dbus-systemcalc              │
│  ┌─────────────────────────────────┐   │
│  │ BatteryService delegate         │   │
│  │  - Reads battery limits         │   │
│  │  - Writes to vebus/solarcharger│   │
│  │  - Manages BMS selection        │   │
│  └─────────────────────────────────┘   │
│  ┌─────────────────────────────────┐   │
│  │ DVCC controller                 │   │
│  │  - MaxChargeCurrent             │   │
│  │  - MaxChargeVoltage             │   │
│  │  - MaxDischargeCurrent          │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
     │ D-Bus
     ▼
┌──────────┐
│ GUI v2   │ (reads all paths for display)
│ VRM       │
└──────────┘
```

Services communicate exclusively through D-Bus. There is no direct service-to-service
communication — everything flows through systemcalc.

## D-Bus conventions

### Service naming

```
com.victronenergy.<type>.<instance>
```

Examples:
- `com.victronenergy.battery.canopen_bms_node1` — BMS battery
- `com.victronenergy.vebus.ttyS4` — VE.Bus device (MultiPlus/Quattro)
- `com.victronenergy.solarcharger.ttyS7` — VE.Direct solar charger
- `com.victronenergy.system` — System calculator (singleton)
- `com.victronenergy.settings` — System settings (singleton)
- `com.victronenergy.vecan.vecan0` — VE.Can interface

### Path structure

Paths use forward-slash hierarchy. Grouped by function:

- `/Dc/0/*` — DC measurements
- `/Ac/ActiveIn/*` — AC input
- `/Ac/Out/*` — AC output
- `/Info/*` — Charge/discharge limits (BMS-controlled)
- `/Alarms/*` — Alarm states
- `/System/*` — System/module configuration
- `/History/*` — Cumulative counters
- `/Mgmt/*` — Service metadata
- `/Settings/*` — Configuration (writable)
- `/Control/*` — Systemcalc control outputs

### Types

| Venus type | DBus signature | Go type    | Description |
|------------|---------------|------------|-------------|
| double     | `d`           | float64    | Measurements (V, A, W, C, Ah) |
| int32      | `i`           | int32      | Counters, flags, enumerations |
| string     | `s`           | string     | Names, versions, identifiers |
| array      | `ai`          | []int32    | Lists (e.g. AvailableBatteries) |

### BusItem interface

Every path implements `com.victronenergy.BusItem`:

| Method       | Returns   | Description |
|-------------|-----------|-------------|
| `GetValue()` | variant   | Current value |
| `GetText()`  | string    | Formatted display string |
| `GetItems()` | a{sa{sv}} | All paths and values |
| Signal `ItemsChanged` | a{sa{sv}} | Emitted on value change |

Path text formatting conventions:
- Voltages: `"%.2fV"` (e.g. "13.08V")
- Currents: `"%.1fA"` (e.g. "-0.61A")
- Power: `"%.0fW"` (e.g. "-8W")
- Temperature: `"%.1f°C"` (e.g. "31.4°C")
- SOC: `"%.0f%%"` (e.g. "29%")

---

## 1. Battery service

**Service:** `com.victronenergy.battery.<id>`

The battery service publishes state, limits, and alarms from a battery management
system. Systemcalc reads this service for DVCC control.

### 1.1 Required paths (subscribed by systemcalc)

These 16 paths are monitored by `batteryservice.py` for BMS detection and DVCC:

| Path                            | Type   | Unit | Description |
|---------------------------------|--------|------|-------------|
| `/Soc`                          | double | %    | State of charge (0-100) |
| `/Dc/0/Voltage`                 | double | V    | Battery terminal voltage |
| `/Dc/0/Current`                 | double | A    | Battery current (+ charge, - discharge) |
| `/Dc/0/Power`                   | double | W    | Battery power (V * I) |
| `/Dc/0/Temperature`             | double | C    | Battery temperature |
| `/Info/MaxChargeCurrent`        | double | A    | DVCC charge current limit |
| `/Info/MaxDischargeCurrent`     | double | A    | DVCC discharge current limit |
| `/Info/MaxChargeVoltage`        | double | V    | DVCC charge voltage limit. **Must not be None for BMS detection.** |
| `/Info/BatteryLowVoltage`       | double | V    | Low voltage threshold |
| `/DeviceInstance`               | int32  | --   | Unique instance (0-255) |
| `/ProductId`                    | int32  | --   | Product ID (0xB005 = lithium) |
| `/ProductName`                  | string | --   | Display name |
| `/CustomName`                   | string | --   | User-editable name |
| `/InstalledCapacity`            | double | Ah   | Nominal capacity |
| `/System/MinCellVoltage`        | double | V    | Minimum cell voltage |
| `/System/MaxCellVoltage`        | double | V    | Maximum cell voltage |
| `/Capabilities/ChargeVoltageControl` | int32 | -- | 1 if BMS controls charge voltage |

**BMS detection:** Systemcalc checks `/Info/MaxChargeVoltage != None`. If set, the
service is treated as a smart BMS capable of DVCC control.

### 1.2 Alarms

All alarm paths are `int32` with three states:
- **0** = OK
- **1** = Warning (BMS warning, FETs still closed)
- **2** = Alarm (BMS has acted, FETs open)

| Path                            | Description |
|---------------------------------|-------------|
| `/Alarms/LowVoltage`            | Under-voltage |
| `/Alarms/HighVoltage`           | Over-voltage |
| `/Alarms/LowTemperature`        | Under-temperature |
| `/Alarms/HighTemperature`       | Over-temperature |
| `/Alarms/LowSoc`                | Low state of charge |
| `/Alarms/HighChargeCurrent`     | Charge over-current |
| `/Alarms/HighDischargeCurrent`  | Discharge over-current |
| `/Alarms/CellImbalance`         | Cell voltage imbalance |
| `/Alarms/InternalFailure`       | Internal BMS hardware/firmware fault |
| `/Alarms/HighChargeTemperature` | High temperature during charge |
| `/Alarms/LowChargeTemperature`  | Low temperature during charge |

### 1.3 System status

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/System/NrOfCellsPerBattery`         | int32  | Cells per module |
| `/System/NrOfModulesOnline`           | int32  | Operational modules |
| `/System/NrOfModulesOffline`          | int32  | Offline/faulted modules |
| `/System/NrOfModulesBlockingCharge`   | int32  | Modules blocking charge |
| `/System/NrOfModulesBlockingDischarge`| int32  | Modules blocking discharge |

### 1.4 Connection

| Path               | Type   | Description |
|--------------------|--------|-------------|
| `/Connected`       | int32  | 1 = data link active, 0 = disconnected/lost |

When `/Connected` transitions to 0, systemcalc forces charge/discharge limits to 0 A.

### 1.5 Charge control I/O

| Path                            | Type   | Description |
|---------------------------------|--------|-------------|
| `/Io/AllowToCharge`             | int32  | 1 = charging permitted |
| `/Io/AllowToDischarge`          | int32  | 1 = discharging permitted |
| `/SystemSwitch`                 | int32  | 1 = system online |

### 1.6 History

| Path                          | Type   | Unit | Description |
|-------------------------------|--------|------|-------------|
| `/History/ChargeCycles`       | int32  | --   | Cumulative charge cycles |
| `/History/TotalAhDrawn`       | double | Ah   | Total amp-hours drawn |
| `/History/DeepestDischarge`   | double | Ah   | Deepest discharge (Ah) |
| `/ConsumedAmphours`           | double | Ah   | Consumed since full charge |

### 1.7 Metadata

| Path                       | Type   | Description |
|----------------------------|--------|-------------|
| `/FirmwareVersion`         | string | BMS firmware version |
| `/HardwareVersion`         | string | Hardware identifier |
| `/ProductId`               | int32  | Product ID |
| `/ProductName`             | string | Product display name |
| `/CustomName`              | string | User-editable name |
| `/DeviceInstance`          | int32  | Unique instance number |
| `/Serial`                  | string | Serial number (optional) |
| `/Mgmt/ProcessName`        | string | Driver executable path |
| `/Mgmt/ProcessVersion`     | string | Driver version |
| `/Mgmt/Connection`         | string | Connection description |

### 1.8 Per-cell voltages (GUI display, optional)

These paths are not consumed by systemcalc but the GUI displays them when present.
Convention from `dbus-serialbattery`:

| Path                  | Type   | Description |
|-----------------------|--------|-------------|
| `/Voltages/Cell1`     | double | Cell 1 voltage (V) |
| ...                   | ...    | ... |
| `/Voltages/Cell14`    | double | Cell 14 voltage (V) |
| `/Voltages/Sum`       | double | Sum of all cell voltages |
| `/Voltages/Diff`      | double | Max cell V - min cell V |

---

## 2. System service (systemcalc)

**Service:** `com.victronenergy.system`

The system calculator aggregates data from all services and produces the
canonical system-wide values used by the GUI and VRM.

### 2.1 Battery aggregation

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/Dc/Battery/Voltage`                 | double | Aggregated battery voltage (V) |
| `/Dc/Battery/Current`                 | double | Aggregated battery current (A) |
| `/Dc/Battery/Power`                   | double | Aggregated battery power (W) |
| `/Dc/Battery/Soc`                     | double | Aggregated SOC (%) |
| `/Dc/Battery/Temperature`             | double | Aggregated temperature (C) |
| `/Dc/Battery/ConsumedAmphours`        | double | Aggregated consumed Ah |
| `/Dc/Battery/Capacity`                | double | Aggregated capacity (Ah) |
| `/Dc/Battery/TimeToGo`                | double | Time to empty (seconds) |
| `/Dc/Battery/State`                   | int32  | Battery state |
| `/Dc/Battery/ChargeVoltage`           | double | Active charge voltage (V) |

### 2.2 DVCC control outputs

These are the values systemcalc writes to chargers/inverters:

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/Control/Dvcc`                       | int32  | DVCC enabled (1/0) |
| `/Control/EffectiveChargeVoltage`     | double | Charge voltage sent to chargers (V) |
| `/Control/MaxChargeCurrent`           | double | Maximum charge current sent to chargers (A) |
| `/Control/SolarChargeVoltage`         | double | Charge voltage for solar chargers (V) |
| `/Control/SolarChargeCurrent`         | double | Charge current for solar chargers (A) |
| `/Control/BmsParameters`              | int32  | BMS parameters active (1/0) |
| `/Control/VebusSoc`                   | double | SOC sent to VE.Bus device |

### 2.3 BMS selection

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/ActiveBmsService`                   | string | Selected BMS service name |
| `/ActiveBmsInstance`                  | int32  | Selected BMS device instance |
| `/AvailableBmsServices`               | array  | List of available BMS services |
| `/AvailableBatteryServices`           | array  | List of all battery services |

### 2.4 System state

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/SystemState/State`                  | int32  | System state (enum) |
| `/SystemState/BatteryLife`            | int32  | BatteryLife state (enum) |
| `/SystemState/LowSoc`                 | int32  | Low SOC active (1/0) |
| `/SystemState/SlowCharge`             | int32  | Slow charge active (1/0) |
| `/SystemState/ChargeDisabled`         | int32  | Charge disabled (1/0) |
| `/SystemState/DischargeDisabled`      | int32  | Discharge disabled (1/0) |
| `/SystemState/UserChargeLimited`      | int32  | User limit on charge (1/0) |
| `/SystemState/UserDischargeLimited`   | int32  | User limit on discharge (1/0) |

### 2.5 Power flows

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/Dc/Pv/Power`                        | double | Total PV power (W) |
| `/Dc/Pv/Current`                      | double | Total PV current (A) |
| `/Dc/System/Power`                    | double | DC system power (W) |
| `/Dc/Charger/Power`                   | double | Total charger power (W) |
| `/Dc/Vebus/Power`                     | double | VE.Bus device DC power (W) |
| `/Dc/Vebus/Current`                   | double | VE.Bus device DC current (A) |
| `/Ac/Grid/L1/Power`                   | double | Grid power L1 (W) |
| `/Ac/Consumption/L1/Power`            | double | Total consumption L1 (W) |
| `/Ac/ActiveIn/L1/Power`               | double | Active AC input L1 (W) |
| `/Ac/ActiveIn/Source`                 | int32  | Active input source |

### 2.6 Alarm forwarding

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/Dc/Battery/Alarms/CircuitBreakerTripped` | int32 | Battery CB tripped |

### 2.7 Dynamic ESS

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/DynamicEss/Active`                  | int32  | DESS active |
| `/DynamicEss/Strategy`                | int32  | Current strategy |
| `/DynamicEss/TargetSoc`               | double | Target SOC (%) |
| `/DynamicEss/MinimumSoc`              | double | Minimum SOC (%)

---

## 3. VE.Bus service (MultiPlus/Quattro)

**Service:** `com.victronenergy.vebus.<port>`

The VE.Bus service represents a MultiPlus or Quattro inverter/charger.

### 3.1 Battery operational limits (written by systemcalc for DVCC)

These are the paths systemcalc writes to control the inverter/charger:

| Path                                            | Type   | Unit | Description |
|-------------------------------------------------|--------|------|-------------|
| `/BatteryOperationalLimits/MaxChargeVoltage`     | double | V    | Charge voltage limit |
| `/BatteryOperationalLimits/MaxChargeCurrent`     | double | A    | Charge current limit |
| `/BatteryOperationalLimits/MaxDischargeCurrent`  | double | A    | Discharge current limit |
| `/BatteryOperationalLimits/BatteryLowVoltage`    | double | V    | Low voltage threshold |

These are the output side of DVCC: systemcalc reads limits from the BMS and writes
them here. The MultiPlus firmware enforces them.

### 3.2 DC measurements

| Path                                  | Type   | Unit | Description |
|---------------------------------------|--------|------|-------------|
| `/Dc/0/Voltage`                       | double | V    | DC bus voltage |
| `/Dc/0/Current`                       | double | A    | DC current |
| `/Dc/0/Power`                         | double | W    | DC power |
| `/Dc/0/Temperature`                   | double | C    | Battery temperature sensor |
| `/Dc/0/MaxChargeCurrent`              | double | A    | Active charge current limit |

### 3.3 AC measurements

| Path                                  | Type   | Unit | Description |
|---------------------------------------|--------|------|-------------|
| `/Ac/ActiveIn/L1/V`                   | double | V    | AC input voltage L1 |
| `/Ac/ActiveIn/L1/I`                   | double | A    | AC input current L1 |
| `/Ac/ActiveIn/L1/P`                   | double | W    | AC input power L1 |
| `/Ac/ActiveIn/L1/F`                   | double | Hz   | AC input frequency |
| `/Ac/ActiveIn/CurrentLimit`           | double | A    | Input current limit |
| `/Ac/Out/L1/V`                        | double | V    | AC output voltage |
| `/Ac/Out/L1/I`                        | double | A    | AC output current |
| `/Ac/Out/L1/P`                        | double | W    | AC output power |

### 3.4 State

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/State`                              | int32  | Inverter/charger state (enum) |
| `/Mode`                               | int32  | Operating mode (on/off/charger-only/inverter-only) |
| `/VebusChargeState`                   | int32  | Charge state (bulk/absorption/float) |
| `/VebusMainState`                     | int32  | Main state (enum) |
| `/Soc`                                | double | SOC as reported by inverter |

### 3.5 Alarms

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/Alarms/LowBattery`                  | int32  | Low battery |
| `/Alarms/HighTemperature`             | int32  | Over-temperature |
| `/Alarms/Overload`                    | int32  | Output overload |
| `/Alarms/Ripple`                      | int32  | DC ripple |
| `/Alarms/GridLost`                    | int32  | Grid lost |
| `/Alarms/BmsConnectionLost`           | int32  | BMS communication lost |

### 3.6 Battery sense

| Path                                  | Type   | Unit | Description |
|---------------------------------------|--------|------|-------------|
| `/BatterySense/Voltage`               | double | V    | Sensed battery voltage |
| `/BatterySense/Temperature`           | double | C    | Sensed battery temperature |

---

## 4. Solar charger service (MPPT)

**Service:** `com.victronenergy.solarcharger.<port>`

Represents a VE.Direct-connected MPPT solar charge controller.

### 4.1 Measurements

| Path                                  | Type   | Unit | Description |
|---------------------------------------|--------|------|-------------|
| `/Dc/0/Voltage`                       | double | V    | Battery voltage |
| `/Dc/0/Current`                       | double | A    | Charge current |
| `/Pv/V`                               | double | V    | PV array voltage |
| `/Yield/Power`                        | double | W    | Current PV power |
| `/Yield/User`                         | double | kWh  | User-resettable yield |
| `/Yield/System`                       | double | kWh  | Total system yield |

### 4.2 State

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/State`                              | int32  | Charger state (enum) |
| `/NrOfTrackers`                       | int32  | Number of MPPT trackers |
| `/MppOperationMode`                   | int32  | MPPT mode |
| `/Mode`                               | int32  | Operating mode |
| `/ErrorCode`                          | int32  | Error code (0 = none) |

### 4.3 DVCC link

When DVCC is active, systemcalc writes to these link paths:

| Path                                  | Type   | Unit | Description |
|---------------------------------------|--------|------|-------------|
| `/Link/ChargeVoltage`                  | double | V    | DVCC charge voltage |
| `/Link/ChargeCurrent`                  | double | A    | DVCC charge current |
| `/Link/NetworkMode`                   | int32  | --   | Network control mode |
| `/Link/VoltageSenseActive`            | int32  | --   | Remote voltage sense active |

### 4.4 Settings

| Path                                  | Type   | Unit | Description |
|---------------------------------------|--------|------|-------------|
| `/Settings/ChargeCurrentLimit`        | double | A    | User charge current limit |
| `/Settings/BatteryVoltageSetting`     | int32  | --   | Battery voltage preset |
| `/Settings/BmsPresent`                | int32  | --   | BMS present flag |

---

## 5. VE.Can service

**Service:** `com.victronenergy.vecan.<interface>`

Represents a VE.Can interface. Publishes bus status and device discovery.

| Path                                  | Type   | Description |
|---------------------------------------|--------|-------------|
| `/Interface`                          | string | CAN interface name |
| `/Bitrate`                            | int32  | CAN bitrate (250000/500000) |
| `/Devices/<n>/DeviceInstance`         | int32  | Discovered device instance |
| `/Devices/<n>/ProductName`            | string | Discovered product name |

---

## 6. DVCC data flow

```
  ┌─────────┐         ┌──────────┐         ┌───────────┐
  │  BMS    │         │systemcalc│         │MultiPlus  │
  │(battery)│         │ (system) │         │  (vebus)  │
  └────┬────┘         └────┬─────┘         └─────┬─────┘
       │                   │                     │
  1. Publish limits        │                     │
     ─────────────────►    │                     │
     /Info/MaxChargeCurrent│                    │
     /Info/MaxChargeVoltage│                     │
     /Info/MaxDischarge..  │                     │
       │                   │                     │
       │              2. Read + validate         │
       │             3. Apply DVCC rules          │
       │                   │                     │
       │              4. Write to vebus          │
       │                   ─────────────────────►│
       │                   /BatteryOperational    │
       │                   Limits/MaxChargeCurrent│
       │                   Limits/MaxChargeVoltage│
       │                                         │
       │              5. Write to MPPT           │
       │                   ─────────────────────►│
       │                   /Link/ChargeVoltage    │
       │                   /Link/ChargeCurrent    │
       │                                         │
       │              6. Publish aggregated      │
       │              /Dc/Battery/*               │
       │              /Control/*                  │
       │                                         │
  7. GUI/VRM reads all services                 │
```

### DVCC safety behavior

1. BMS publishes `/Connected = 0` or stops updating paths
2. Systemcalc detects timeout (depends on BMS update interval)
3. Systemcalc writes 0 to all `/BatteryOperationalLimits/MaxCharge*` paths on vebus
4. Systemcalc writes 0 to MPPT `/Link/ChargeCurrent`

---

## 7. Settings service

**Service:** `com.victronenergy.settings`

System-wide persistent configuration. Key paths relevant to battery integration:

| Path                                              | Type   | Description |
|---------------------------------------------------|--------|-------------|
| `/Settings/SystemSetup/BatteryService`             | string | Selected battery service |
| `/Settings/SystemSetup/BmsInstance`                | int32  | Selected BMS instance (-1 = auto) |
| `/Settings/SystemSetup/SharedVoltageSense`         | int32  | Shared voltage sense (0/1) |
| `/Settings/SystemSetup/SharedTemperatureSense`     | int32  | Shared temperature sense (0/1) |
| `/Settings/CGwacs/BatteryLife/State`               | int32  | BatteryLife state |
| `/Settings/CGwacs/BatteryLife/SocLimit`            | double | Minimum SOC (%) |
| `/Settings/Dvcc/ChargeCurrentLimit`                | double | User charge current limit (A) |
| `/Settings/Canbus/can0/Profile`                    | int32  | CAN profile (4 = CAN-bus BMS 500kbps) |

---

## References

- Venus OS v3.75 on Cerbo GX, live introspection of all services
- `dbus-systemcalc-py` source: `victronenergy/dbus-systemcalc-py` on GitHub
  - `delegates/batteryservice.py` — BMS detection and path subscription
  - `delegates/dvcc.py` — DVCC control logic
  - `scripts/dummycanbms.py` — CAN-BMS test harness
- `vedbus` / `velib_python` — Victron's D-Bus helper libraries
- VE.Can registers public document: `VE.Can-registers-public.pdf`
