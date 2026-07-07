# Venus OS VE.Bus Service (MultiPlus/Quattro) Reference

D-Bus interface for Victron MultiPlus and Quattro inverter/chargers.
Documented from live observation on Venus OS v3.75, MultiPlus-II 48/3000.

## Service

```
com.victronenergy.vebus.<port>
```

Example: `com.victronenergy.vebus.ttyS4`

## Battery Operational Limits (DVCC targets)

These paths are **written by systemcalc** during DVCC operation. The BMS
publishes limits; systemcalc reads them, applies DVCC rules and user
settings, then writes the result here. The MultiPlus firmware enforces
them on its DC port.

| Path                                            | Type   | Unit | Live value | Source |
|-------------------------------------------------|--------|------|------------|--------|
| `/BatteryOperationalLimits/MaxChargeVoltage`     | double | V    | 14.20      | BMS CVL with DVCC adjustments |
| `/BatteryOperationalLimits/MaxChargeCurrent`     | double | A    | 50.0       | BMS CCL capped by VE.Configure |
| `/BatteryOperationalLimits/MaxDischargeCurrent`  | double | A    | 120.7      | BMS DCL (direct passthrough) |
| `/BatteryOperationalLimits/BatteryLowVoltage`    | double | V    | 655.3      | Disabled (max value = no limit) |

**Critical distinction:** `/BatteryOperationalLimits/MaxChargeCurrent` (50A)
is what systemcalc wants. `/Dc/0/MaxChargeCurrent` (37A) is what the
MultiPlus actually enforces after considering AC input limits and internal
derating. The inverter itself determines the final number.

## DC measurements

| Path                      | Type   | Unit | Live     |
|---------------------------|--------|------|----------|
| `/Dc/0/Voltage`           | double | V    | 13.18    |
| `/Dc/0/Current`           | double | A    | -0.80    |
| `/Dc/0/Power`             | double | W    | -10.5    |
| `/Dc/0/Temperature`       | double | C    | (varies) |
| `/Dc/0/MaxChargeCurrent`  | double | A    | 37.0     |
| `/Dc/0/Capacity`          | double | Ah   | (varies) |

`/Dc/0/Current` sign convention: negative = discharge, positive = charge.

## AC measurements

| Path                          | Type   | Unit | Live  |
|-------------------------------|--------|------|-------|
| `/Ac/ActiveIn/L1/V`           | double | V    | ~230  |
| `/Ac/ActiveIn/L1/I`           | double | A    | 0     |
| `/Ac/ActiveIn/L1/P`           | double | W    | 0     |
| `/Ac/ActiveIn/L1/F`           | double | Hz   | ~50   |
| `/Ac/ActiveIn/CurrentLimit`   | double | A    | 16.0  |
| `/Ac/ActiveIn/ActiveInput`    | int32  | --   | 240   |
| `/Ac/Out/L1/V`                | double | V    | ~230  |
| `/Ac/Out/L1/I`                | double | A    | ~0.01 |
| `/Ac/Out/L1/P`                | double | W    | 2     |
| `/Ac/Out/L1/F`                | double | Hz   | ~50   |

## State enums

### /State — Operating state

| Value | State               |
|-------|---------------------|
| 0     | Off                 |
| 1     | Low power           |
| 2     | Fault               |
| 3     | Bulk                |
| 4     | Absorption          |
| 5     | Float               |
| 6     | Storage             |
| 7     | Equalize            |
| 8     | Passthru            |
| 9     | Inverting           |
| 10    | Power assist        |
| 11    | Power supply        |
| 252   | Standby             |

Live observation: State 9 (Inverting) with VebusChargeState 1 (Float).

### /VebusChargeState — Charge phase

| Value | Phase               |
|-------|---------------------|
| 0     | Off                 |
| 1     | Float               |
| 2     | Absorption          |
| 3     | Bulk                |
| 4     | Equalize            |

### /Mode — Operating mode

| Value | Mode                |
|-------|---------------------|
| 1     | Charger Only        |
| 2     | Inverter Only       |
| 3     | On                  |
| 4     | Off                 |

## Alarms

All alarm paths are `int32`. Victron convention: 0 = OK, 1 = warning, 2 = alarm.

| Path                            | Type   | Trigger condition                    |
|---------------------------------|--------|--------------------------------------|
| `/Alarms/LowBattery`            | int32  | DC voltage below low-voltage threshold |
| `/Alarms/HighTemperature`       | int32  | Internal temperature exceeds limit    |
| `/Alarms/Overload`              | int32  | Output power exceeds rating           |
| `/Alarms/Ripple`                | int32  | DC ripple voltage too high            |
| `/Alarms/TemperatureSensor`     | int32  | Battery temp sensor fault             |
| `/Alarms/VoltageSensor`         | int32  | Battery voltage sensor fault          |
| `/Alarms/GridLost`              | int32  | AC input lost                         |
| `/Alarms/HighDcVoltage`         | int32  | DC voltage above max                  |
| `/Alarms/HighDcCurrent`         | int32  | DC current above max                  |
| `/Alarms/BmsConnectionLost`     | int32  | BMS communication timeout (~5s)       |
| `/Alarms/BmsPreAlarm`           | int32  | BMS pre-alarm warning                 |

**Live observation:** All alarms at 0 (no faults) on a healthy system.

## Alarm interaction with DVCC

| Alarm                    | DVCC effect                                     |
|--------------------------|-------------------------------------------------|
| BmsConnectionLost        | Charge current forced to 0, reverts to internal |
| HighTemperature          | Charge/discharge current derated                |
| LowBattery               | Discharge may be limited                        |
| Overload                 | Output derated, may switch to passthru           |
| GridLost                 | Switches to inverting, no grid charge            |
| BatteryOvervoltageProtection | Disables charging                            |

## Battery Sense

When shared voltage sense is active, systemcalc writes the primary
battery measurement to both vebus and all chargers.

| Path                          | Type   | Unit | Source                |
|-------------------------------|--------|------|-----------------------|
| `/BatterySense/Voltage`       | double | V    | Written by systemcalc |
| `/BatterySense/Temperature`   | double | C    | Written by systemcalc |

## Energy counters

| Path                              | Type   | Unit | Description              |
|-----------------------------------|--------|------|--------------------------|
| `/Energy/AcIn1ToInverter`         | double | kWh  | Grid to inverter         |
| `/Energy/AcIn1ToAcOut`            | double | kWh  | Grid to AC output        |
| `/Energy/InverterToAcIn1`         | double | kWh  | Inverter feeding to grid |
| `/Energy/InverterToAcOut`         | double | kWh  | Inverter to AC output    |
| `/Energy/OutToInverter`           | double | kWh  | AC output to inverter    |

## Extra Battery Current

| Path                      | Type   | Unit | Description                         |
|---------------------------|--------|------|--------------------------------------|
| `/ExtraBatteryCurrent`    | double | A    | DC sources not under DVCC (written by systemcalc) |

When DC chargers (alternators, fuel cells) feed the battery outside
DVCC control, systemcalc adds their current here so the MultiPlus
accounts for it in its charge calculations.

## Firmware features

| Path                                    | Type   | Description                        |
|-----------------------------------------|--------|------------------------------------|
| `/FirmwareFeatures/SetChargeState`      | int32  | Supports SetChargeState command    |
| `/FirmwareFeatures/IBatSOCBroadcast`    | int32  | Broadcasts SOC via VE.Bus          |
| `/FirmwareFeatures/BolFrame`            | int32  | Supports BOL frame                 |
| `/FirmwareFeatures/BolUBatAndTBatSense` | int32  | BOL frame includes V and T         |

---

# Venus OS Solar Charger Service (MPPT) Reference

D-Bus interface for Victron MPPT solar charge controllers.
Documented from live observation on Venus OS v3.75, SmartSolar MPPT 100/30.

## Service

```
com.victronenergy.solarcharger.<port>
```

Example: `com.victronenergy.solarcharger.ttyS7`

## Measurements

| Path                      | Type   | Unit | Live   |
|---------------------------|--------|------|--------|
| `/Dc/0/Voltage`           | double | V    | 13.19  |
| `/Dc/0/Current`           | double | A    | 0.0    |
| `/Pv/V`                   | double | V    | 18.53  |
| `/Yield/Power`            | double | W    | 0.67   |
| `/Yield/User`             | double | kWh  | 490.8  |
| `/Yield/System`           | double | kWh  | 490.8  |

## DVCC Link Paths

Written by systemcalc when DVCC controls the charger. These override
the charger's own charge algorithm.

| Path                          | Type   | Unit | Live   | Description |
|-------------------------------|--------|------|--------|-------------|
| `/Link/NetworkMode`           | int32  | --   | 13     | DVCC control active |
| `/Link/ChargeVoltage`         | double | V    | 14.20  | Target charge voltage |
| `/Link/ChargeCurrent`         | double | A    | 30.0   | Target charge current |
| `/Link/BatteryCurrent`        | double | A    | --     | Battery current for offset calc |
| `/Link/VoltageSense`          | double | V    | 13.17  | Shared voltage measurement |
| `/Link/VoltageSenseActive`    | int32  | --   | 1      | Remote sense enabled |
| `/Link/TemperatureSense`      | double | C    | []     | Shared temperature (unset) |
| `/Link/TemperatureSenseActive`| int32  | --   | 0      | Remote temp sense disabled |
| `/Link/NetworkStatus`         | int32  | --   | --     | Network connection status |

### NetworkMode values

| Value | Mode                      |
|-------|---------------------------|
| 0     | Standalone (no DVCC)      |
| 13    | DVCC active (observed)    |

## State

| Path                      | Type   | Live | Description |
|---------------------------|--------|------|-------------|
| `/State`                  | int32  | 252  | Charger state |
| `/ErrorCode`              | int32  | 0    | 0 = no error |
| `/Mode`                   | int32  | 1    | 1 = On |
| `/MppOperationMode`       | int32  | 2    | 2 = MPPT tracking |
| `/NrOfTrackers`           | int32  | 1    | Number of MPPT inputs |
| `/DeviceOffReason`        | int32  | 0    | 0 = not off |
| `/Load/State`             | int32  | 1    | Load output state |

### State values

| Value | State             |
|-------|-------------------|
| 0     | Off               |
| 2     | Fault             |
| 3     | Bulk              |
| 4     | Absorption        |
| 5     | Float             |
| 252   | External control  |

State 252 means the charger is under DVCC control — systemcalc is
sending it charge voltage/current via the Link paths.

### MppOperationMode values

| Value | Mode          |
|-------|---------------|
| 0     | Off           |
| 1     | PV voltage limited |
| 2     | MPPT active   |

## Settings

| Path                                  | Type   | Live  | Description |
|---------------------------------------|--------|-------|-------------|
| `/Settings/ChargeCurrentLimit`        | double | 30.0  | User current limit (A) |
| `/Settings/BatteryVoltageSetting`     | int32  | 12    | Voltage preset index |
| `/Settings/BmsPresent`                | int32  | 1     | BMS connected flag |

## Capabilities

| Path                          | Type   | Live        | Description |
|-------------------------------|--------|-------------|-------------|
| `/Capabilities/Capabilities1` | int32  | 134868854   | Bitfield of features |

## History

Daily history stored as `/History/Daily/0` through `/History/Daily/30`.

| Path                              | Type   | Unit | Description |
|-----------------------------------|--------|------|-------------|
| `/History/Daily/<n>/Yield`        | double | kWh  | Day yield |
| `/History/Daily/<n>/MaxPower`     | double | W    | Peak power |
| `/History/Daily/<n>/MaxPvVoltage` | double | V    | Peak PV voltage |
| `/History/Daily/<n>/MaxBatteryVoltage` | double | V | Peak battery voltage |
| `/History/Daily/<n>/MinBatteryVoltage` | double | V | Min battery voltage |
| `/History/Daily/<n>/TimeInBulk`    | double | s    | Time in bulk |
| `/History/Daily/<n>/TimeInAbsorption` | double | s | Time in absorption |
| `/History/Daily/<n>/TimeInFloat`   | double | s    | Time in float |
| `/History/Daily/<n>/LastError1`    | int32  | --   | Last error codes |
| `/History/Overall/DaysAvailable`   | int32  | --   | Days of history |

---

## VE.Bus ↔ MPPT interaction under DVCC

When DVCC is active (systemcalc `/Control/Dvcc = True`):

1. Systemcalc reads BMS limits from `com.victronenergy.battery.*`
2. Writes charge voltage target to both:
   - vebus `/BatteryOperationalLimits/MaxChargeVoltage`
   - solarcharger `/Link/ChargeVoltage`
3. Writes charge current target to both:
   - vebus `/BatteryOperationalLimits/MaxChargeCurrent`
   - solarcharger `/Link/ChargeCurrent`
4. Both devices use the same CVL but different CCLs:
   - MultiPlus: limited by VE.Configure hardware cap
   - MPPT: limited by its own hardware rating (30A for 100/30)
5. Shared voltage sense: vebus `/Dc/0/Voltage` is written to
   solarcharger `/Link/VoltageSense` for accurate voltage measurement
   when the MPPT is far from the battery.

When the BMS disconnects (CAN timeout), systemcalc writes 0 to all
charge/discharge limit paths on both devices simultaneously.
