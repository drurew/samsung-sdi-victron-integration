# Victron CAN-bus BMS Protocol Specification

Community-documented reverse-engineered specification of the Victron CAN-bus
BMS protocol used by the Venus OS `can-bus-bms` service. This protocol is how
managed batteries (BYD, Pylontech, Freedomwon, REC, MG Energy Systems, and
others) communicate with Victron GX devices over CAN at 500 kbps.

**Disclaimer:** Victron does not publish this specification publicly. It is
provided to battery manufacturers on request. This document is based on
community reverse-engineering and testing against Venus OS v3.70 on a
Cerbo GX MK2 with an ELPM482-00005 battery.

## Transport layer

| Parameter   | Value                     |
|-------------|---------------------------|
| Protocol    | CAN 2.0A                  |
| Bitrate     | 500 kbps                  |
| Endianness  | Little-endian (LE)        |
| Frame size  | 8 bytes per frame         |
| Interval    | 1000 ms (all frames)      |

## Frame definitions

### 0x351 -- Battery State

Transmitted by the BMS to the GX device.

| Bytes  | Type  | Scale   | Field              | Notes                 |
|--------|-------|---------|--------------------|-----------------------|
| 0-1    | U16   | 0.01    | Battery voltage    | Volts (e.g. 5300 = 53.00 V) |
| 2-3    | S16   | 0.1     | Battery current    | Amps, positive = charge, negative = discharge |
| 4-5    | U16   | 0.1     | Temperature        | Degrees Celsius (e.g. 250 = 25.0 C) |
| 6      | U8    | 1       | State of Charge    | Percent (0-100)       |
| 7      | U8    | 1       | State of Health    | Percent (0-100)       |

### 0x355 -- Precision SOC / SOH

| Bytes  | Type  | Scale   | Field              | Notes                 |
|--------|-------|---------|--------------------|-----------------------|
| 0-1    | U16   | 0.1     | State of Charge    | 0.1% resolution (e.g. 855 = 85.5%) |
| 2-3    | U16   | 0.1     | State of Health    | 0.1% resolution       |
| 4-7    | --    | --      | Reserved           | Set to 0xFFFF          |

### 0x356 -- Charge / Discharge Limits

| Bytes  | Type  | Scale   | Field                     | Notes                 |
|--------|-------|---------|---------------------------|-----------------------|
| 0-1    | U16   | 0.1     | Charge Voltage Limit      | Volts (CVL)           |
| 2-3    | U16   | 0.1     | Charge Current Limit      | Amps (CCL)            |
| 4-5    | U16   | 0.1     | Discharge Current Limit   | Amps (DCL)            |
| 6-7    | U16   | 0.1     | Discharge Voltage Limit   | Volts (DVL)           |

DVCC behavior:
- CVL = 0 means "do not charge".
- CCL = 0 means "do not charge" (forces charge current to zero).
- DCL = 0 means "do not discharge".
- The GX device enforces these limits through DVCC and will not override them.

### 0x35A -- Alarms and Warnings

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-1    | U16   | Alarm bits         | Severity 2 in Victron convention |
| 2-3    | U16   | Warning bits       | Severity 1 in Victron convention |
| 4-7    | --    | Reserved           | Set to 0               |

Bitfield layout (identical for alarms and warnings):

| Bit  | Condition             |
|------|-----------------------|
| 0    | High voltage          |
| 1    | Low voltage           |
| 2    | High temperature      |
| 3    | Low temperature       |
| 4    | High charge current   |
| 5    | High discharge current|
| 6    | Internal failure      |
| 7    | Cell imbalance        |
| 8-15 | Reserved              |

### 0x35E -- Battery Name (Venus OS v2.80+)

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-7    | ASCII | Battery name       | Null-terminated, up to 8 characters |

The name is displayed in the Venus OS device list.

### 0x35F -- Manufacturer Info (Venus OS v2.80+)

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-3    | ASCII | Manufacturer       | Four-character code   |
| 4-7    | ASCII | Firmware version   | e.g. "0100" for v1.00 |

## What this protocol does NOT support

The Victron CAN-BMS protocol was designed for basic managed batteries -- lead-acid
replacements with internal BMS -- and deliberately omits diagnostics that Victron's
own batteries do not provide:

- **Per-cell voltages** -- No frames for individual cell voltage reporting.
- **Per-cell temperatures** -- No frames for individual cell temperature.
- **Cell voltage statistics** -- No min/max/avg cell voltage.
- **Tray/module-level data** -- No tray count, tray voltages, tray faults.
- **Heartbeat / watchdog** -- No keep-alive counter. The GX device detects BMS
  loss by frame timeout (no frames for ~5 seconds).
- **SOH detail** -- Only a single integer percentage, no cycle count or
  cumulative Ah.

For batteries that provide this data (such as the Samsung SDI ELPM482-00005),
the Python reference driver (`samsung_sdi_bms_service.py`) publishes all
available fields directly to D-Bus using the `velib` library, bypassing the
CAN-BMS protocol entirely.

## Safety behavior

The GX device monitors frame arrival. If no frame is received from the BMS
for approximately 5 seconds:

1. DVCC charge/discharge current limits are forced to 0 A.
2. The battery is marked as disconnected.
3. The system falls back to voltage-based control if a compatible source
   (MPPT, MultiPlus) is still available.

This timeout is implemented by the GX device, not by the BMS. The BMS does
not need to send an explicit watchdog or heartbeat for this to work -- simply
ceasing transmission during a fault condition is sufficient to halt charging.

## References

- Victron Battery Compatibility documentation:
  <https://www.victronenergy.com/live/battery_compatibility:start>
- Full D-Bus battery service specification: [DBUS_BATTERY_SPEC.md](DBUS_BATTERY_SPEC.md)
- Samsung SDI ELPM482-00005 Product Specification Rev 0.2
- Community CAN-BMS reverse engineering: Victron Community forums,
  SimpBMS, REC BMS, and Pylontech integration projects.
- Verified against live Venus OS v3.71 on Cerbo GX with source-level
  confirmation from `dbus-systemcalc-py` (`dummycanbms.py`, `batteryservice.py`).
