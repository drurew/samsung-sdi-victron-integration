# Samsung SDI to Victron CAN-BMS Field Mapping

How each Samsung SDI ELPM482-00005 CAN frame field maps to the Victron CAN-bus
BMS protocol. Used by the C driver when `--can-bms` is enabled.

Frame semantics verified against stock Venus OS driver on Cerbo GX MK2 (v3.70)
by XOThermite (PR #28).

## Mapping table

| Samsung SDI source                   | Victron dest    | Notes                                     |
|--------------------------------------|-----------------|-------------------------------------------|
| 0x500 system_voltage (U16, 0.01V)    | 0x356 V (S16)   | Samsung 0.01V/bit. Victron s16 0.01V/bit. Direct. |
| 0x500 system_current (S16, 1A)       | 0x356 I (S16)   | Samsung 1A/bit. Victron 0.1A/bit. Scaled x10. |
| 0x500 system_soc (U8, %)             | 0x355 SOC (U16) | U16 at 1%. Also visible in 0x351 via D-Bus. |
| 0x500 system_soh (U8, %)             | 0x355 SOH (U16) | U16 at 1%.                                |
| 0x500 system_heartbeat               | --              | Not mapped.                                |
| 0x501 alarm_status (U16)             | 0x35A warnings  | Samsung alarm bits → Victron warnings (bytes 4-7) |
| 0x501 protection_status (U16)        | 0x35A alarms    | Samsung protection → Victron alarms (bytes 0-3) |
| 0x501 total_trays                    | 0x372 online    | Module count                              |
| 0x501 normal_trays                   | 0x372 online    | Operational modules                        |
| 0x501 fault_trays                    | 0x372 offline   | Offline modules                            |
| 0x502 charge_voltage_limit           | 0x351 CVL (U16) | Clamped to CVL ceiling (55.4V default)    |
| 0x502 charge_current_limit           | 0x351 CCL (S16) | 0A if any protection bit active           |
| 0x502 discharge_current_limit         | 0x351 DCL (S16) | 0A if any protection bit active           |
| 0x502 discharge_voltage_limit         | 0x351 DVL (U16) | Direct                                     |
| 0x503 avg_cell_voltage               | --              | Not mapped. Use D-Bus path.               |
| 0x503 max_cell_voltage               | 0x373 max cell mV |                                        |
| 0x503 min_cell_voltage               | 0x373 min cell mV |                                        |
| 0x503 avg_tray_voltage               | --              | Not mapped.                                |
| 0x504 max_tray_voltage               | --              | Not mapped.                                |
| 0x504 min_tray_voltage               | --              | Not mapped.                                |
| 0x504 avg_cell_temp                  | 0x356 T (S16)   | Direct, 0.1°C/bit. Also 0x373 max in Kelvin. |
| 0x504 max_cell_temp                  | 0x373 max temp K | Kelvin = °C + 273.15                    |
| 0x504 min_cell_temp                  | 0x373 min temp K | Kelvin = °C + 273.15                    |
| 0x5F0-0x5F4 per-cell voltages        | --              | Not mapped to CAN-BMS. Available via D-Bus. |
| --                                   | 0x35E name      | Static: "SDI 482 "                         |
| --                                   | 0x35F info      | Model tag 0x0482, rev 0.2, capacity Ah     |

## Alarm bitfield mapping

Samsung SDI 0x501 → Victron 0x35A 2-bit flag pairs:

| Samsung bit | Samsung condition     | Victron flag | Severity       |
|-------------|-----------------------|-------------|----------------|
| 0           | Over-Voltage          | 2           | prot→alarm, alarm→warn |
| 1           | Under-Voltage         | 4           | prot→alarm, alarm→warn |
| 2           | Over-Temperature      | 6           | prot→alarm, alarm→warn |
| 3           | Under-Temperature     | 8           | prot→alarm, alarm→warn |
| 4           | Charge Over-Current   | 16          | prot→alarm, alarm→warn |
| 5           | Discharge Over-Current| 14          | prot→alarm, alarm→warn |
| 6           | FET Over-Temp         | 22          | Aggregated into BMS internal |
| 7           | Tray Voltage Imbalance| 24          | prot→alarm, alarm→warn |
| 8           | Tray-ID error         | 22          | Internal failure          |
| 9           | PCS comm error        | 22          | Internal failure          |
| 10          | FET failure           | 22          | Internal failure          |
| 11          | FET OT failure        | 22          | Internal failure          |
| 12          | UV shutdown           | 4           | Forces Low Voltage alarm  |
| 13          | Cell voltage imbalance| 24          | Forces Cell Imbalance     |
| 14          | 2nd Over-Voltage      | 2           | Forces High Voltage alarm |
| any         | Any protection bit    | 0           | General alarm flag        |

## Safety policies (--can-bms mode)

- **CVL ceiling:** Emitted CVL = min(BMS CVL, 55.4 V). The BMS broadcasts
  58.1 V (4.15 V/cell); 55.4 V = 3.957 V/cell on 14S.
- **Protection ⇒ 0 A:** Any active protection bit forces CCL = DCL = 0 A
  in the 0x351 frame, redundant with the BMS zeroing its own broadcast.
- **Freshness gate:** Translation requires 0x500 + 0x501 + 0x502 all
  within timeout (5 s). Partial failure (e.g. limits frames stopping)
  suspends transmission instead of freezing stale limits.

## Summary

Data carried through CAN-BMS translator: voltage, current, temperature, SOC,
SOH, charge/discharge limits, alarm/warning status, module counts, min/max
cell mV, min/max temperature. 28 fields available from Samsung SDI BMS,
~16 carried through CAN-BMS.

For full telemetry (per-cell individual voltages, cell voltage stats, tray
data, heartbeat), the D-Bus path (default, no `--can-bms`) publishes all
28 fields directly to D-Bus via the Victron `com.victronenergy.BusItem` interface.
