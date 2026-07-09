# Samsung SDI Victron Integration — Architecture

How the Samsung SDI ELPM482-00005 battery modules integrate with Victron
Venus OS. Three integration paths, one codebase.

## Integration paths

```
┌─────────────────────────────────────────────────────────────────┐
│                  Samsung SDI ELPM482-00005                     │
│                  CAN 2.0A, 500kbps, PDO broadcast              │
└──────────────────────────┬──────────────────────────────────────┘
                           │ CAN frames (0x500-0x504, 0x5F0-0x5F4)
                           ▼
              ┌────────────────────────┐
              │   samsung-sdi-bms.c    │
              │   (single C binary)    │
              └───────────┬────────────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
   ┌─────────────┐ ┌──────────────┐ ┌──────────┐
   │  D-Bus      │ │ CAN-BMS      │ │  Both    │
   │  (default)  │ │ (--can-bms)  │ │ (flag)   │
   └──────┬──────┘ └──────┬───────┘ └──────────┘
          │               │
          ▼               ▼
   ┌─────────────┐ ┌──────────────────┐
   │ Venus OS    │ │ Victron          │
   │ systemcalc  │ │ can-bus-bms      │
   │ + GUI + VRM │ │ service          │
   └─────────────┘ └──────────────────┘
```

### Path 1: Direct D-Bus (default, recommended)

```
samsung-sdi-bms → libdbus-1 → com.victronenergy.battery.samsung_sdi
                                  │
                    ┌─────────────┼─────────────┐
                    ▼             ▼             ▼
              systemcalc      GUI v2         VRM
              (DVCC)        (overview)    (portal)
```

All 28 Samsung SDI fields published. Per-cell voltages at `/Voltages/Cell1-14`.
DVCC limits at `/Info/MaxCharge*`. Alarms mapped to Victron convention (0/1/2).
Systemcalc auto-detects via `/Info/MaxChargeVoltage != None`.

### Path 2: CAN-BMS translation (`--can-bms` flag)

```
samsung-sdi-bms → CAN frames → Victron can-bus-bms → D-Bus
                (0x351/355/   (/opt/victronenergy/  (com.victronenergy
                 356/35A/     can-bus-bms/)          .battery.socketcan_*)
                 372/373/
                 35E/35F)
```

Lighter integration. 16 of 28 fields carried. Per-cell data at `/Cell/N` (stock
service convention, GUI v2 does not render individual cells). Simpler but less
data. Use when D-Bus path is not needed or as a fallback.

### Path 3: Both (default + `--can-bms`)

Both paths active simultaneously. The stock Victron service and our D-Bus
service coexist on different service names. Users can compare data between
the two paths.

## Data flow: from CAN frame to Venus OS

```
Samsung SDI BMS
      │
      │ CAN frame 0x500 arrives every ~500ms
      ▼
parse_status() — extracts voltage, current, SOC, SOH, heartbeat
      │
      │ battery state struct (state_t) updated
      ▼
emit_changed() — builds ItemsChanged D-Bus signal
      │
      │ libdbus-1 marshalling
      ▼
systemcalc receives ItemsChanged
      │
      │ batteryservice.py reads /Info/MaxChargeCurrent,
      │ /Info/MaxChargeVoltage, /Info/MaxDischargeCurrent
      ▼
systemcalc writes to MultiPlus:
  /BatteryOperationalLimits/MaxChargeCurrent
  /BatteryOperationalLimits/MaxChargeVoltage
      │
      ▼
MultiPlus enforces limits on DC charge/discharge
```

## State management

### CAN data freshness
- `state_t.last_update` — timestamp of last valid CAN frame
- `live` flag — set when first CAN data arrives, cleared after `DATA_TIMEOUT_SEC` of silence
- On timeout: `/Connected` transitions to 0, charge/discharge limits forced to 0A

### CAN-BMS freshness gate (PR #29)
- `state_t.rx_status`, `rx_config`, `rx_limits` — per-frame timestamps
- `vcan_fresh()` requires ALL three frames (0x500, 0x501, 0x502) fresh within timeout
- Prevents startup zeroed-limits and partial-failure stale-limits

## D-Bus interface

The driver implements `com.victronenergy.BusItem` with:
- `GetValue()` — returns current value as variant (double/int32/string)
- `GetText()` — returns formatted display string
- `GetItems()` — returns all paths and values
- `ItemsChanged` signal — emitted on data change and disconnect

Paths are resolved from a static table with function-pointer dispatch for
computed values (power = V*I, consumed Ah = capacity*(100-SOC)/100, alarms).

## CAN-BMS frame mapping

| Samsung frame | Victron frame | Fields mapped |
|--------------|---------------|---------------|
| 0x500 | 0x356 STATE | V, I |
| 0x500 | 0x355 SOC/SOH | SOC, SOH |
| 0x501 | 0x35A ALARMS | Alarm + protection → flag pairs |
| 0x501 | 0x372 MODULES | Normal/fault tray counts |
| 0x502 | 0x351 LIMITS | CVL (capped), CCL, DCL, DVL |
| 0x503 | 0x373 CELLINFO | Min/max cell mV |
| 0x504 | 0x356 STATE | Temperature |
| 0x504 | 0x373 CELLINFO | Min/max temp (Kelvin) |
| -- | 0x35E NAME | Static: "SDI 482 " |
| -- | 0x35F INFO | Model 0x0482, rev 0.2, capacity |

## Safety policies

- **CVL ceiling:** Charge voltage capped at 55.4V (3.957V/cell on 14S). The BMS broadcasts 58.1V (4.15V/cell).
- **Protection → 0A:** Any active 0x501 protection bit forces CCL=DCL=0A in the 0x351 limits frame.
- **CAN timeout:** 5 seconds without CAN data marks battery disconnected, limits zeroed.
- **Freshness gate:** CAN-BMS translation requires status+config+limits all fresh.

## Build and deploy

```bash
# On the Cerbo
make                    # builds samsung-sdi-bms (needs gcc, libdbus-1-dev)
./samsung-sdi-bms can0  # D-Bus only

# CI
gcc -DUNIT_TEST -o test src/samsung-sdi-bms.c && ./test  # 27 checks, no libdbus needed
```

## Reference documents

| Document | Covers |
|----------|--------|
| `DBUS_BATTERY_SPEC.md` | Every D-Bus path with type, unit, and description |
| `DVCC_CONTROL_FLOW.md` | Limit chain from BMS to inverter with live measurements |
| `VEBUS_MPPT_REFERENCE.md` | MultiPlus and MPPT service interfaces |
| `GUI_DISPLAY_RULES.md` | What the Venus OS GUI actually renders |
| `SETTINGS_AND_CANBUS_BMS.md` | Settings paths and can-bus-bms binary analysis |
| `CAN_BMS_PROTOCOL.md` | Frame definitions, verified against stock driver |
| `SAMSUNG_TO_VICTRON_MAPPING.md` | Field-by-field Samsung to Victron mapping |
