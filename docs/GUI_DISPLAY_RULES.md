# Venus OS GUI v2 Battery Display Rules

Which D-Bus paths the Venus OS GUI v2 renders on the battery pages.
Documented from the public `victronenergy/gui-v2` source code.

## Main battery page

The primary battery overview page reads these paths:

| Path                      | Displayed as       | Notes |
|---------------------------|-------------------|-------|
| `/Soc`                    | Large SOC gauge   | Percentage with color coding |
| `/Soh`                    | Health percentage | Shown if available |
| `/Dc/0/Voltage`           | Voltage reading   | Main voltage display |
| `/Dc/0/Current`           | Current reading   | Positive = charge, negative = discharge |
| `/Dc/0/Power`             | Power reading     | Watts |
| `/Dc/0/Temperature`       | Temperature       | With user's preferred unit |
| `/Capacity`               | Capacity          | Ah |
| `/ConsumedAmphours`       | Consumed Ah       | Shows consumption |
| `/TimeToGo`               | Time remaining    | Shown if available |
| `/State`                  | State label       | "Bulk", "Float", etc. |
| `/Mode`                   | Mode indicator    | Only if supported |
| `/ErrorCode`              | Error display     | Shown when non-zero |
| `/Alarms/Alarm`           | Alarm indicator   | Generic alarm flag |
| `/Relay/0/State`          | Relay status      | If relay present |
| `/Dc/1/Voltage`           | Starter battery   | Dual-battery monitors |
| `/Dc/0/MidVoltageDeviation` | Midpoint deviation | Battery balancers |
| `/BusVoltage` or `/BussVoltage` | Bus voltage   | System bus voltage |
| `/AirTemperature`         | Air temperature   | If sensor present |
| `/NumberOfBmses`          | BMS count         | Number of BMS modules |
| `/N2kDeviceInstance`      | NMEA 2000 ID      | For device routing |
| `/Errors/SmartLithium/Communication` | Comm error | Smart Lithium BMS |
| `/Errors/SmartLithium/Voltage` | Voltage error | Smart Lithium BMS |

## Battery Details sub-page

The details page adds cell-level and module-level data:

| Path                                  | Displayed as            | Required for display |
|---------------------------------------|-------------------------|---------------------|
| `/System/MinCellVoltage`              | Lowest cell voltage     | Yes |
| `/System/MaxCellVoltage`              | Highest cell voltage    | Yes |
| `/System/MinCellTemperature`          | Lowest cell temperature | Yes |
| `/System/MaxCellTemperature`          | Highest cell temperature | Yes |
| `/System/MinVoltageCellId`            | Cell # with lowest V    | Yes (shown alongside min) |
| `/System/MaxVoltageCellId`            | Cell # with highest V   | Yes (shown alongside max) |
| `/System/MinTemperatureCellId`        | Cell # with lowest T    | Yes |
| `/System/MaxTemperatureCellId`        | Cell # with highest T   | Yes |
| `/System/NrOfModulesOnline`           | Modules online          | Yes |
| `/System/NrOfModulesOffline`          | Modules offline         | Yes |
| `/System/NrOfModulesBlockingCharge`   | Blocking charge         | Yes |
| `/System/NrOfModulesBlockingDischarge`| Blocking discharge      | Yes |

The details page renders conditionally: it only shows sections that
have valid data. If `/System/MinCellVoltage` is null/absent, the
cell voltage section is hidden. Same for temperature.

## What the GUI does NOT display

| Path pattern              | Not rendered because |
|---------------------------|---------------------|
| `/Cell/N`                 | CAN-BMS convention, not used by GUI v2 |
| `/Voltages/CellN`         | dbus-serialbattery convention, requires GUI mod |
| `/Voltages/Sum`           | Requires GUI mod |
| `/Voltages/Diff`          | Requires GUI mod |
| `/History/ChargeCycles`   | Not shown on battery page (available via VRM) |
| `/History/TotalAhDrawn`   | Not shown on battery page |
| `/Io/AllowToCharge`       | Internal to DVCC, not displayed |
| `/Io/AllowToDischarge`    | Internal to DVCC, not displayed |
| `/Capabilities/ChargeVoltageControl` | Internal to DVCC |

## Community GUI mods

The dbus-serialbattery project defines additional paths that
community-maintained GUI mods (not stock Venus OS) display:

| Path                  | Mod displays |
|-----------------------|-------------|
| `/Voltages/Cell1-16`   | Per-cell voltage bar chart |
| `/Voltages/Sum`        | Pack voltage total |
| `/Voltages/Diff`       | Cell balance indicator |
| `/Balancing`           | Balancing status |

These paths are the reason our Python driver uses `/Voltages/CellN`
instead of `/Cell/N` — the community mods expect that convention.

## Implications for the Samsung SDI driver

Both the D-Bus path (default) and the CAN-BMS path (`--can-bms`)
need these paths to populate the GUI fully:

| Path                              | D-Bus path | CAN-BMS path |
|-----------------------------------|-----------|-------------|
| `/System/MinCellVoltage`          | Yes       | Yes (0x373 → binary) |
| `/System/MaxCellVoltage`          | Yes       | Yes (0x373 → binary) |
| `/System/MinCellTemperature`      | Yes       | Yes (0x373 → binary) |
| `/System/MaxCellTemperature`      | Yes       | Yes (0x373 → binary) |
| `/System/MinVoltageCellId`        | Yes       | Not from CAN-BMS translator |
| `/System/MaxVoltageCellId`        | Yes       | Not from CAN-BMS translator |
| `/System/*CellId` paths           | Yes       | Not from CAN-BMS translator |

The cell ID paths (`/System/MinVoltageCellId`) tell the GUI which cell
number has the extreme value. Our D-Bus path can compute these from the
per-cell voltage array. The CAN-BMS path cannot — the 0x373 frame only
carries the mV values, not the cell numbers.
