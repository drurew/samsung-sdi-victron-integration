# Victron CAN-bus BMS Protocol Specification

Community-documented specification of the Victron CAN-bus BMS protocol used by
the Venus OS `can-bus-bms` service. Verified against a stock Cerbo GX MK2
running Venus OS v3.70 by XOThermite (PR #28).

**Disclaimer:** Victron does not publish this specification publicly. It is
provided to battery manufacturers on request. This document is based on
live-hardware testing and frame-capture verification.

## Transport layer

| Parameter   | Value                     |
|-------------|---------------------------|
| Protocol    | CAN 2.0A                  |
| Bitrate     | 500 kbps                  |
| Endianness  | Little-endian (LE)        |
| Frame size  | 8 bytes per frame         |
| Interval    | 1000 ms (all frames)      |

## Frame definitions

### 0x351 -- Charge/Discharge Limits

Limits that DVCC enforces on chargers and inverters. Verified against stock
Venus OS driver on Cerbo GX MK2 (v3.70): `351 [8] 2A 02 D6 01 D6 01 C0 01`
displayed on the Parameters page as CVL 55.4 V / CCL 47.0 A.

| Bytes  | Type  | Scale   | Field                     | Notes                 |
|--------|-------|---------|---------------------------|-----------------------|
| 0-1    | U16   | 0.1     | Charge Voltage Limit      | V (CVL)               |
| 2-3    | S16   | 0.1     | Charge Current Limit      | A (CCL)               |
| 4-5    | S16   | 0.1     | Discharge Current Limit   | A (DCL)               |
| 6-7    | U16   | 0.1     | Discharge Voltage Limit   | V (DVL)               |

Note: CCL and DCL are signed 16-bit (S16). CVL and DVL are unsigned (U16).

### 0x355 -- SOC / SOH

| Bytes  | Type  | Scale   | Field              | Notes                 |
|--------|-------|---------|--------------------|-----------------------|
| 0-1    | U16   | 1       | State of Charge    | % (not 0.1%: verified on hardware) |
| 2-3    | U16   | 1       | State of Health    | %                     |
| 4-7    | --    | --      | Reserved           | Set to 0               |

### 0x356 -- Battery State

Live measurements. 6-byte frame (bytes 6-7 unused).

| Bytes  | Type  | Scale   | Field              | Notes                 |
|--------|-------|---------|--------------------|-----------------------|
| 0-1    | S16   | 0.01    | Battery voltage    | V (e.g. 4891 = 48.91 V) |
| 2-3    | S16   | 0.1     | Battery current    | A, positive = charge   |
| 4-5    | S16   | 0.1     | Temperature        | °C                    |
| 6-7    | --    | --      | Reserved           | Unused                 |

### 0x35A -- Alarms and Warnings

Wire format: **2-bit flag pairs** across all 8 bytes. Alarms in bytes 0-3,
warnings in bytes 4-7. Each pair: `01` = active, `00` = inactive. This is NOT
a plain bitmask. Verified on live hardware: injecting an over-temperature bit
with this encoding produced a correctly named "High Temperature" GX notification.

| Flag offset | Condition             | Notes                  |
|-------------|-----------------------|------------------------|
| 0           | General alarm         | Set when any protection active |
| 2           | High voltage          |                        |
| 4           | Low voltage           |                        |
| 6           | High temperature      |                        |
| 8           | Low temperature       |                        |
| 10          | High charge temperature |                      |
| 12          | Low charge temperature  |                      |
| 14          | High discharge current |                       |
| 16          | High charge current   |                        |
| 18          | Contactor             |                        |
| 20          | Short circuit         |                        |
| 22          | BMS internal failure  |                        |
| 24          | Cell imbalance        |                        |

Severity: alarms (bytes 0-3) = severity 2 in Victron convention. Warnings
(bytes 4-7) = severity 1.

### 0x35E -- Battery Name (Venus OS v2.80+)

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-7    | ASCII | Battery name       | 8 ASCII characters     |

### 0x35F -- Manufacturer Info (Venus OS v2.80+)

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-1    | U16   | Model tag          | e.g. 0x0482            |
| 2-3    | U16   | Protocol revision  | e.g. 0x0002 for Rev 0.2 |
| 4-5    | U16   | Capacity           | Ah                     |
| 6-7    | --    | Reserved           | Set to 0               |

### 0x372 -- Module Counts

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-1    | U16   | Online modules     |                       |
| 2-3    | U16   | Blocking charge    |                       |
| 4-5    | U16   | Blocking discharge |                       |
| 6-7    | U16   | Offline modules    |                       |

### 0x373 -- Cell Info (feeds GX Battery Details page)

| Bytes  | Type  | Field              | Notes                 |
|--------|-------|--------------------|-----------------------|
| 0-1    | U16   | Min cell voltage   | mV                    |
| 2-3    | U16   | Max cell voltage   | mV                    |
| 4-5    | U16   | Min temperature    | Kelvin                |
| 6-7    | U16   | Max temperature    | Kelvin                |

## What this protocol does NOT support

The Victron CAN-BMS protocol was designed for basic managed batteries and
deliberately omits diagnostics that Victron's own batteries do not provide:

- **Per-cell individual voltages** -- 0x373 provides min/max cell mV only,
  not per-cell voltages. The Samsung SDI broadcasts 14 individual cell
  voltages via 0x5F0-0x5F4; these are only available through the direct
  D-Bus path (Python driver or `samsung-sdi-bms` default mode).
- **Tray/module-level detail beyond counts** -- 0x372 provides counts only,
  no per-tray voltages or faults.
- **Heartbeat / watchdog** -- No keep-alive counter. The GX device detects
  BMS loss by frame timeout (~5 seconds).
- **SOH detail** -- Only a single integer percentage, no cycle count or
  cumulative Ah (those are D-Bus paths only).
- **Per-cell temperatures** -- Min/max only, no individual readings.

## Safety behavior

The GX device monitors frame arrival. If no frame is received from the BMS
for approximately 5 seconds:

1. DVCC charge/discharge current limits are forced to 0 A.
2. The battery is marked as disconnected.
3. The system falls back to voltage-based control if a compatible source
   (MPPT, MultiPlus) is still available.

This timeout is implemented by the GX device, not by the BMS. The BMS does
not need to send an explicit watchdog -- ceasing transmission during a fault
condition is sufficient to halt charging.

## Samsung SDI → Victron alarm mapping

Samsung SDI 0x501 bit positions (spec Rev 0.2 Table 8) to Victron 0x35A flags:

| Samsung bit | Condition             | Victron flag offset | Severity       |
|-------------|-----------------------|---------------------|----------------|
| 0           | Over-Voltage          | 2                   | alarm=2/warn=1 |
| 1           | Under-Voltage         | 4                   | alarm=2/warn=1 |
| 2           | Over-Temperature      | 6                   | alarm=2/warn=1 |
| 3           | Under-Temperature     | 8                   | alarm=2/warn=1 |
| 4           | Charge Over-Current   | 16                  | alarm=2/warn=1 |
| 5           | Discharge Over-Current| 14                  | alarm=2/warn=1 |
| 6           | FET Over-Temp         | 22                  | alarm only     |
| 7           | Tray Voltage Imbalance| 24                  | alarm=2/warn=1 |
| 8-11        | Various internal      | 22                  | alarm only     |
| 12          | UV shutdown           | 4                   | alarm only     |
| 13          | Cell imbalance        | 24                  | alarm only     |
| 14          | 2nd Over-Voltage      | 2                   | alarm only     |

Samsung alarm bits (BMS warning, FETs still closed) → Victron warnings (bytes 4-7).
Samsung protection bits (BMS has acted, FETs open) → Victron alarms (bytes 0-3).

## References

- Victron Battery Compatibility: <https://www.victronenergy.com/live/battery_compatibility:start>
- Full D-Bus battery service specification: [DBUS_BATTERY_SPEC.md](DBUS_BATTERY_SPEC.md)
- Samsung SDI → Victron mapping: [SAMSUNG_TO_VICTRON_MAPPING.md](SAMSUNG_TO_VICTRON_MAPPING.md)
- Samsung SDI ELPM482-00005 Product Specification Rev 0.2
- Verified against Venus OS v3.70 on Cerbo GX MK2 via frame capture and injection
  testing (PR #28, XOThermite).
