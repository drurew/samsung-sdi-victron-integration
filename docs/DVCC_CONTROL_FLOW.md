# Venus OS DVCC Control Flow

How Distributed Voltage and Current Control (DVCC) propagates battery
limits through Venus OS to chargers and inverters. Documented from live
observation on Venus OS v3.71 with SuperB Epsilon V2 BMS, MultiPlus-II,
and MPPT 100/30.

## Architecture

```
     ┌───────────┐
     │    BMS    │  publishes /Info/MaxChargeCurrent, MaxChargeVoltage,
     │ (battery) │  MaxDischargeCurrent, BatteryLowVoltage
     └─────┬─────┘
           │  ItemsChanged signals
           ▼
     ┌───────────┐
     │systemcalc │  reads BMS limits, applies DVCC rules, writes targets
     │ (system)  │  to chargers and inverters
     └──┬────┬───┘
        │    │
        ▼    ▼
  ┌──────────┐  ┌───────────┐
  │ MultiPlus│  │   MPPT    │  enforce limits on their DC ports
  │ (vebus)  │  │(solarchg) │
  └──────────┘  └───────────┘
```

## Live measurement snapshot (2026-07-07, Venus OS v3.71)

### 1. BMS publishes limits

```
BMS /Info/MaxChargeCurrent      = 84.0 A   ← raw BMS broadcast
BMS /Info/MaxChargeVoltage       = 14.40 V  ← raw BMS broadcast
BMS /Info/MaxDischargeCurrent    = 120.8 A  ← raw BMS broadcast
```

### 2. Systemcalc reads and processes

```
system /ActiveBmsService        = "com.victronenergy.battery.canopen_bms_node1"
system /Control/Dvcc            = True      ← DVCC is active
system /Control/BmsParameters   = 1         ← BMS limits are being used
system /Control/SolarChargeCurrent = 1      ← solar chargers under DVCC
system /Control/BatteryVoltageSense = 1     ← shared voltage sense active
system /Control/BatteryCurrentSense = 1     ← shared current sense active
```

### 3. Systemcalc writes to MultiPlus

```
vebus /BatteryOperationalLimits/MaxChargeVoltage    = 14.20 V   ← BMS 14.40 → applied as 14.20
vebus /BatteryOperationalLimits/MaxChargeCurrent    = 50.0 A    ← BMS 84.0 → capped at 50.0
vebus /BatteryOperationalLimits/MaxDischargeCurrent = 120.7 A   ← BMS 120.8 → direct passthrough
vebus /BatteryOperationalLimits/BatteryLowVoltage   = 655.3 V   ← effectively disabled (max value)
```

The 50A cap on charge current is the MultiPlus hardware limit (set in
VE.Configure), not a DVCC rule. The BMS allows 84A but the inverter
cannot deliver more than 50A DC.

### 4. Systemcalc writes to MPPT

```
solarcharger /Link/ChargeVoltage   = 14.20 V    ← matches vebus CVL
solarcharger /Link/ChargeCurrent   = 30.0 A     ← MPPT hardware limit (100/30 model)
solarcharger /Link/NetworkMode     = 13         ← DVCC control active
solarcharger /Link/VoltageSense    = 13.17 V    ← shared voltage from vebus
solarcharger /Link/VoltageSenseActive = 1       ← remote sense enabled
```

The MPPT gets the same 14.20V charge voltage target as the MultiPlus.
The 30A limit is the MPPT's own hardware rating, not a DVCC cap.

### 5. MultiPlus internal state

```
vebus /Mode                         = 3         ← On
vebus /VebusChargeState             = 1         ← Float
vebus /State                        = 9         ← Inverting
vebus /Ac/ActiveIn/CurrentLimit     = 16.0 A    ← AC input limit (grid)
vebus /Dc/0/MaxChargeCurrent        = 37.0 A    ← actual active charge limit
vebus /ExtraBatteryCurrent          = 0.0 A     ← no external DC sources
vebus /BatterySense/Voltage         = 13.18 V   ← current battery voltage
vebus /BatterySense/Temperature     = []        ← no temp sensor
```

The real charge current the MultiPlus will use is 37A (its own internal
calculation considering AC input limits and the 50A ceiling), not the
50A from BatteryOperationalLimits nor the 84A from the BMS.

## DVCC limit chain

BMS limits flow through multiple stages:

```
Stage 1: BMS raw limits
  /Info/MaxChargeCurrent      = 84.0 A

Stage 2: DVCC user overrides (settings)
  /Settings/Dvcc/ChargeCurrentLimit  (not set → ignored)
  /Settings/CGwacs/MaxChargePercentage = 100%  (no reduction)

Stage 3: Systemcalc writes to inverter
  /BatteryOperationalLimits/MaxChargeCurrent = 50.0 A
  ↓ MultiPlus hardware cap (VE.Configure) kicks in

Stage 4: Inverter internal calculation
  /Dc/0/MaxChargeCurrent   = 37.0 A
  ↓ considers AC input limit (16A × 230V = 3680W → ~37A at 13.2V)

Stage 5: Actual charge current delivered
  /Dc/0/Current  (varies with battery state)
```

## Discharge limit chain

Discharge limits follow a simpler path because the inverter doesn't
self-limit discharge the same way:

```
BMS /Info/MaxDischargeCurrent   = 120.8 A
     → passthrough to vebus
vebus /BatteryOperationalLimits/MaxDischargeCurrent = 120.7 A
     → inverter enforces this on DC output
```

No hardware cap on discharge because the MultiPlus-II can deliver its
full rated power as discharge.

## Shared voltage and temperature sense

When `/SystemSetup/SharedVoltageSense = 1`, systemcalc reads voltage
from the primary source (usually vebus `/Dc/0/Voltage`) and writes it
to all chargers:

```
vebus /Dc/0/Voltage     = 13.18 V  → systemcalc reads
                                      → writes to MPPT /Link/VoltageSense = 13.17 V
                                      → writes to settings for VRM display
```

## Systemcalc debug offsets

These paths reveal systemcalc's internal adjustments:

```
/Debug/BatteryOperationalLimits/CurrentOffset      = 0  ← no offset applied
/Debug/BatteryOperationalLimits/SolarVoltageOffset  = 0  ← no solar correction
/Debug/BatteryOperationalLimits/VebusVoltageOffset  = 0  ← no vebus correction
```

Adding non-zero offsets is how systemcalc compensates for voltage drop
in cables or measurement differences between devices.

## BMS loss behavior

When the BMS stops updating (CAN timeout):

1. Systemcalc detects `/Connected = 0` or data staleness
2. Systemcalc writes `MaxChargeCurrent = 0` and `MaxDischargeCurrent = 0`
   to both vebus and solarcharger
3. The MultiPlus raises `/Alarms/BmsConnectionLost`
4. System falls back to voltage-based charging if a compatible source
   exists

This happens within ~5 seconds of the last BMS update.

## MultiPlus alarms that interact with DVCC

| Alarm path                            | Effect                                     |
|---------------------------------------|--------------------------------------------|
| `/Alarms/BmsConnectionLost`           | Charging halted, reverts to internal limits |
| `/Alarms/LowBattery`                  | Discharge current may be limited            |
| `/Alarms/HighTemperature`             | Charge/discharge current may be reduced     |
| `/Alarms/Overload`                    | Output may be derated                       |
| `/Alarms/GridLost`                    | Switches to inverting                       |

## Settings paths that affect DVCC

| Path                                              | Effect                                 |
|---------------------------------------------------|----------------------------------------|
| `/Settings/SystemSetup/BatteryService`             | Which battery service provides limits  |
| `/Settings/SystemSetup/SharedVoltageSense`         | 1 = share vebus voltage to all chargers|
| `/Settings/SystemSetup/SharedTemperatureSense`     | 1 = share vebus temperature            |
| `/Settings/CGwacs/BatteryLife/State`               | 2 = Keep Charged, 9 = Optimized        |
| `/Settings/CGwacs/BatteryLife/SocLimit`            | Minimum SOC for discharge (30% here)   |
| `/Settings/CGwacs/MaxChargePercentage`             | Scales CCL (100% = passthrough)        |
| `/Settings/CGwacs/MaxDischargePercentage`          | Scales DCL (100% = passthrough)        |

## Key observations

1. **CCL is NOT a passthrough.** The BMS publishes 84A but the inverter
   receives 50A (VE.Configure hardware cap) and delivers 37A (internal
   AC input limit calculation). Multiple layers reduce the BMS value.

2. **DCL IS a direct passthrough** (120.8A → 120.7A). The inverter
   doesn't self-limit discharge unless output is overloaded.

3. **CVL is slightly reduced** (14.40V → 14.20V). The 0.2V drop may be
   systemcalc compensating for cable losses, or a rounding effect.

4. **The MPPT gets the same CVL as the MultiPlus** (14.20V) but a
   different CCL (30A = hardware rating, not the BMS value).

5. **BatteryLowVoltage on vebus is 655.3V** — effectively disabled.
   This means the BMS does not publish a low-voltage threshold, or
   systemcalc sets it to max when no value is available.

## Testing DVCC without breaking anything

See this document's companion test procedure for injecting synthetic
BMS values via a Python test service and observing the propagation
through systemcalc to vebus and MPPT. This allows validating DVCC
behavior without touching the real BMS.
