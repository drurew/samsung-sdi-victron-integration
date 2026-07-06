# Samsung SDI to Victron CAN-BMS Field Mapping

How each Samsung SDI ELPM482-00005 CAN frame field maps (or does not map) to
the Victron CAN-bus BMS protocol. Used by the C translator (`src/samsung-sdi-bms.c`).

## Mapping table

| Samsung SDI source                   | Victron dest    | Notes                                     |
|--------------------------------------|-----------------|-------------------------------------------|
| 0x500 system_voltage (U16, 0.01V)    | 0x351 V (U16)   | Direct mapping.                            |
| 0x500 system_current (S16, 1A)       | 0x351 I (S16)   | Samsung: 1 A/bit. Victron: 0.1 A/bit. Scaled x10. |
| 0x500 system_soc (U8, %)             | 0x351 SOC (U8)  | Direct. Also sent as 0x355 precision.      |
| 0x500 system_soh (U8, %)             | 0x351 SOH (U8)  | Direct. Also sent as 0x355 precision.      |
| 0x500 system_heartbeat               | --              | Not mapped. Victron has no heartbeat frame.|
| 0x501 alarm_status (U16)             | 0x35A           | Mapped to alarm/warning bitfield.          |
| 0x501 protection_status (U16)        | 0x35A           | Samsung protection -> Victron alarm.       |
| 0x501 total_trays / normal / fault   | --              | Not mapped. Victron has no tray fields.    |
| 0x502 charge_voltage_limit           | 0x356 CVL       | Direct.                                    |
| 0x502 charge_current_limit           | 0x356 CCL       | Direct.                                    |
| 0x502 discharge_current_limit         | 0x356 DCL       | Direct.                                    |
| 0x502 discharge_voltage_limit         | 0x356 DVL       | Direct.                                    |
| 0x503 avg_cell_voltage               | --              | Not mapped.                                |
| 0x503 max_cell_voltage               | --              | Not mapped.                                |
| 0x503 min_cell_voltage               | --              | Not mapped.                                |
| 0x503 avg_tray_voltage               | --              | Not mapped.                                |
| 0x504 max_tray_voltage               | --              | Not mapped.                                |
| 0x504 min_tray_voltage               | --              | Not mapped.                                |
| 0x504 avg_cell_temp (S8, C)          | 0x351 Temp (U16)| Direct, scaled to 0.1 C/bit.              |
| 0x504 max_cell_temp                  | --              | Not mapped.                                |
| 0x504 min_cell_temp                  | --              | Not mapped.                                |
| 0x5F0-0x5F4 cell voltages (per cell) | --              | Not mapped. Victron has no per-cell frame. |

## Alarm bitfield mapping

Samsung SDI 0x501 bit positions (spec Rev 0.2 Table 8) to Victron 0x35A bits:

| Samsung bit | Condition             | Victron bit | Rule                              |
|-------------|-----------------------|-------------|-----------------------------------|
| 0           | Over-Voltage          | 0           | prot -> alarm, alarm -> warning   |
| 1           | Under-Voltage         | 1           | prot -> alarm, alarm -> warning   |
| 2           | Over-Temperature      | 2           | prot -> alarm, alarm -> warning   |
| 3           | Under-Temperature     | 3           | prot -> alarm, alarm -> warning   |
| 4           | Charge Over-Current   | 4           | prot -> alarm, alarm -> warning   |
| 5           | Discharge Over-Current| 5           | prot -> alarm, alarm -> warning   |
| 6           | FET Over-Temp         | 6           | Aggregated into Internal Failure  |
| 7           | Tray Voltage Imbalance| 7           | prot -> alarm, alarm -> warning   |
| 8           | Tray-ID error         | 6           | Aggregated into Internal Failure  |
| 9           | PCS comm error        | 6           | Aggregated into Internal Failure  |
| 10          | FET failure           | 6           | Aggregated into Internal Failure  |
| 11          | FET OT failure        | 6           | Aggregated into Internal Failure  |
| 12          | UV shutdown           | 1           | Forces Low Voltage alarm          |
| 13          | Cell voltage imbalance| 7           | Forces Cell Imbalance alarm       |
| 14          | 2nd Over-Voltage      | 0           | Forces High Voltage alarm         |

Samsung alarm bits (BMS warning, FETs still closed) map to Victron warning (severity 1).
Samsung protection bits (BMS has acted, FETs open) map to Victron alarm (severity 2).

## Summary

Data carried through the translator: voltage, current, temperature, SOC, SOH,
charge/discharge limits, and alarm/warning status. 28 fields available from the
Samsung SDI BMS, 11 carried through.

For full telemetry (per-cell voltages, cell stats, tray data, heartbeat),
use the Python driver (`samsung_sdi_bms_service.py`) which publishes all 28
fields directly to D-Bus via Victron's `velib` library.
