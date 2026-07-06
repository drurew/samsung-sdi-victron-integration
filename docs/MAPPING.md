# Samsung SDI → Victron CAN-bus BMS: Field Mapping (src/samsung-sdi-bms.c)

Source: Samsung SDI ELPM482-00005 Product Specification Rev 0.2, Table 8
(CAN 2.0A, 500 kbps, little-endian, 500 ms broadcast period).
Target: Victron CAN-bus BMS profile ("CAN-bus BMS (500 kbit/s)"), the
protocol used by Pylontech/BYD/Freedom Won. Both sides little-endian.
Translator TX cadence: 1000 ms, only while Samsung data is fresh.

## Frame map

| Victron | Field | Bytes | Scale | Source (Samsung) | Notes |
|---|---|---|---|---|---|
| 0x351 | Charge Voltage Limit | 0-1 U16 | 0.1 V | 0x502 b0-1 (0.1 V) | **min(BMS CVL, --cvl-max)**, default ceiling 55.4 V |
| 0x351 | Charge Current Limit | 2-3 S16 | 0.1 A | 0x502 b2-3 (0.1 A) | forced 0 A if any 0x501 protection bit set |
| 0x351 | Discharge Current Limit | 4-5 S16 | 0.1 A | 0x502 b4-5 (0.1 A) | forced 0 A if any 0x501 protection bit set |
| 0x351 | Discharge Voltage Limit | 6-7 U16 | 0.1 V | 0x502 b6-7 (0.1 V) | pass-through (typ. 44.8 V) |
| 0x355 | SOC | 0-1 U16 | 1 % | 0x500 b4 (U8) | |
| 0x355 | SOH | 2-3 U16 | 1 % | 0x500 b5 (U8) | |
| 0x356 | Battery voltage | 0-1 S16 | 0.01 V | 0x500 b0-1 (0.01 V) | |
| 0x356 | Battery current | 2-3 S16 | 0.1 A | 0x500 b2-3 (1 A, S16) | ×10. Sign: + = charging on both sides — **verify with clamp meter on bench**. 1 A source resolution; see note below |
| 0x356 | Temperature | 4-5 S16 | 0.1 °C | 0x504 b4 (S8, avg cell) | ×10 |
| 0x35A | Alarms (bytes 0-3) | 2-bit fields | — | 0x501 b2-3 protection bits | BMS has acted (FETs open / limits zeroed) |
| 0x35A | Warnings (bytes 4-7) | 2-bit fields | — | 0x501 b0-1 alarm bits | BMS warning, FETs still closed |
| 0x372 | Modules online | 0-1 U16 | — | 0x501 b5 (normal trays) | |
| 0x372 | Modules offline | 6-7 U16 | — | 0x501 b6 (fault trays) | |
| 0x373 | Min cell voltage | 0-1 U16 | 1 mV | 0x503 b4-5 (1 mV) | |
| 0x373 | Max cell voltage | 2-3 U16 | 1 mV | 0x503 b2-3 (1 mV) | |
| 0x373 | Min temperature | 4-5 U16 | 1 K | 0x504 b6 (S8 °C) | +273 |
| 0x373 | Max temperature | 6-7 U16 | 1 K | 0x504 b5 (S8 °C) | +273 |
| 0x35E | Product name | 0-7 ASCII | — | constant "SDI 482 " | |
| 0x35F | Model / rev / capacity | U16×3 | — | constants, cap = 94 Ah | |

## Samsung fields intentionally NOT translated

| Samsung | Field | Reason |
|---|---|---|
| 0x500 b6-7 | Heartbeat | no Victron equivalent; the freshness gate supersedes it |
| 0x503 b0-1 | Avg cell voltage | Victron shows min/max only; avg visible in the live viewer |
| 0x503 b6-7 / 0x504 b0-3 | Avg/max/min tray voltage | single-module system; pack voltage covers it |
| 0x505 | Protocol version | static |
| 0x510/0x511 | Per-tray records | single tray; viewer displays them |
| 0x5F0–0x5F4 | Individual cell voltages | outside the Victron BMS protocol; live viewer displays all 14 |

## Alarm bit mapping (0x501 → 0x35A)

Samsung bit (byte 0 of alarm/protection field) → Victron 2-bit flag:

| Samsung bit | Meaning | Victron flag |
|---|---|---|
| 0 | Over-Voltage | High battery voltage |
| 1 | Under-Voltage | Low battery voltage |
| 2 | Over-Temperature | High temperature |
| 3 | Under-Temperature | Low temperature (warnings only; no protection bit exists) |
| 4 | Charge Over-Current | High charge current |
| 5 | Discharge Over-Current | High (discharge) current |
| 6 | FET Over-Temperature | BMS internal |
| 7 | Tray Voltage Imbalance | Cell imbalance |
| 8-14 (protection high byte) | Tray-ID / PCS comm / FET failure / FET OT failure / UV shutdown / cell imbalance / 2nd OV | BMS internal + General alarm |

Any protection bit also sets the **General alarm** flag and forces
CCL = DCL = 0 A in 0x351.

## Encoding conventions to verify on the bench (Phase 3)

1. **0x35A bit-pair convention** — implemented as `01 = active,
   00 = inactive` per the prevailing Pylontech-compatible convention.
   Verify: with the system idle, dbus-spy on the GX battery service
   must show all alarms 0; then force one (or `cansend` a crafted
   0x501) and confirm the matching alarm text appears. The flag
   positions are centralised in one enum in the source if a correction
   is needed.
2. **Current sign** — charge a few amps, confirm GX shows positive
   current and SOC rising.
3. **Current resolution** — 0x500 current is 1 A per the spec. If
   bench readings confirm the per-tray frame 0x510 (0.01 A) is present
   and sane, a later revision can prefer it on single-module systems.
4. **Keep-alive** — Venus OS transmits 0x305 to the battery; the
   translator neither needs nor answers it (the Samsung BMS broadcasts
   unconditionally). Confirm no side effects on the bus.
