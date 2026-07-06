# Samsung SDI ELPM482-00005 — Victron Cerbo GX Integration

Native CAN bus integration between Samsung SDI ELPM482-00005 lithium-ion
battery modules and Victron Cerbo GX systems. Publishes system voltage,
current, State of Charge, temperature, and dynamic charge/discharge limits
to the Victron D-Bus, enabling proper DVCC operation with MultiPlus
inverter/chargers and MPPT solar controllers.

## Features

- **PDO-based monitoring** — the Samsung SDI BMS broadcasts all data on
  fixed CAN IDs at 500 kbps. No SDO requests required; the driver simply
  listens and publishes.
- **Dynamic charge limits** — reads charge/discharge current limits and
  charge/discharge voltage limits directly from the BMS broadcasts and
  publishes them to D-Bus for DVCC control.
- **Alarm propagation** — Samsung SDI alarm bits (over/under voltage,
  over/under temperature, over-current, FET over-temperature, cell
  imbalance) are mapped to Victron alarm paths.
- **Minimal resource footprint** — the C driver uses approximately 1 MB of
  RAM and near-zero CPU, compared to approximately 27 MB for the Python
  reference implementation.
- **Zero dependencies** — the C driver links against libc only.
- **Protocol verified** — field layout confirmed against Samsung SDI
  Product Specification ELPM482-00005 Rev 0.2.

## Quick Start

```bash
# On the Cerbo GX (SSH as root)
cd /data
git clone https://github.com/drurew/samsung-sdi-victron-integration.git
cd samsung-sdi-victron-integration
make
cp samsung-sdi-bms /data/samsung-sdi/
/data/samsung-sdi/samsung-sdi-bms can0 &

# Verify
dbus -y com.victronenergy.battery.samsung_sdi /Soc GetValue
dbus -y com.victronenergy.battery.samsung_sdi /Info/MaxChargeCurrent GetValue
```

## Architecture

```
Samsung SDI ELPM482-00005 ── CAN 2.0A (500 kbps) ── samsung-sdi-bms (C)
                                                                   │
                                              Victron CAN-BMS frames
                                              (0x351, 0x355, 0x356, 0x35A)
                                                                   │
                                                         Victron can-bus-bms
                                                         (stock Venus OS driver)
                                                                   │
                                                           Victron D-Bus
                                                                   │
                                                            DVCC / SystemCalc
                                                                   │
                                              ┌────────────────────┼──────────┐
                                              ▼                    ▼          ▼
                                         MultiPlus            MPPT Solar   VRM Portal
```

The Samsung SDI BMS broadcasts all data on fixed CAN IDs at 500 ms intervals.
The translator listens passively, parses each message, and re-emits the data
as Victron CAN-bus BMS frames on the same interface. The stock Venus OS CAN-BMS
driver picks up the translated frames and handles all D-Bus publishing. No
SDO requests are sent to the battery.

## CAN Protocol

| CAN ID | Name | Key Fields |
|--------|------|------------|
| 0x500 | System Status | Voltage (U16, 0.01V), Current (S16, 1A), SoC (U8, %), SoH (U8, %), Heartbeat, Alarms |
| 0x501 | System Config | Total trays, normal trays, fault trays, protection status |
| 0x502 | Charge Limits | Charge voltage (U16, 0.1V), charge current limit (U16, 0.1A), discharge current limit (U16, 0.1A), discharge voltage (U16, 0.1V) |
| 0x503 | Cell Voltages | Avg/max/min cell voltage (U16, 0.001V), avg tray voltage (U16, 0.01V) |
| 0x504 | Temperatures | Avg/max/min cell temperature (S8, C) |

All values are little-endian. CAN 2.0A, 11-bit identifiers, 500 kbps.

Full protocol specification: [Samsung SDI ELPM482-00005 Rev 0.2](https://community.victronenergy.com/uploads/short-url/9VfS5ZSnYrAdr0OrmR3sh8O4xjk.pdf)

## Hardware Setup

The Samsung SDI battery and Victron Cerbo GX use different CAN pinouts.
A custom crossover cable is required:

| Signal | Samsung SDI (CN7/CN8) | Cerbo GX (VE.Can) |
|--------|-----------------------|-------------------|
| CAN-H  | Pin 1 | Pin 7 |
| CAN-L  | Pin 2 | Pin 8 |
| GND    | Pin 3 | Pin 3 |

## D-Bus Service

| Service | Path | Description |
|---------|------|-------------|
| `samsung_sdi` | `/Soc` | State of Charge (0-100%) |
| | `/Dc/0/Voltage` | System voltage (V) |
| | `/Dc/0/Current` | System current (A) |
| | `/Dc/0/Temperature` | Average cell temperature (C) |
| | `/Info/MaxChargeCurrent` | Charge current limit (A) |
| | `/Info/MaxDischargeCurrent` | Discharge current limit (A) |
| | `/Info/MaxChargeVoltage` | Charge voltage setpoint (V) |
| | `/Alarms/*` | Alarm status bits |

## Installation

See [docs/INSTALL.md](docs/INSTALL.md) for detailed installation instructions.

## Repository Structure

```
.
├── README.md
├── CHANGELOG.md
├── VERSION
├── LICENSE
├── Makefile
├── config.ini
├── .github/workflows/build.yml
├── src/
│   ├── samsung-sdi-bms.c           # C driver (production)
│   ├── samsung_sdi_bms_service.py  # Python driver (reference)
│   └── samsung_sdi_can_client.py   # CAN client library
├── docs/
│   ├── INSTALL.md                  # Installation guide
│   ├── TROUBLESHOOTING.md          # Troubleshooting guide
│   ├── CAN_BMS_PROTOCOL.md         # Victron CAN-BMS protocol spec
│   └── SAMSUNG_TO_VICTRON_MAPPING.md  # Samsung SDI -> Victron field mapping
└── scripts/
    ├── install-to-cerbo.sh         # One-command deployment
    ├── install.sh                  # Venus OS package installer
    ├── start_bms.sh                # Service launcher
    └── create_package.sh           # Tarball packaging
```

## Requirements

### C Driver
- Linux with SocketCAN support
- CAN interface configured at 500 kbps
- D-Bus system bus
- GCC or any C99 compiler

### Python Driver
- Python 3.7+
- python-can, dbus-python, pygobject

## Acknowledgments

- **XOThermite** — diagnosed all five defects in the C driver's D-Bus layer and
  proposed the CAN-BMS protocol translator approach (issue #15); provided the
  spec-corrected 0x504 tray-voltage and 0x5F0-0x5F4 cell-voltage definitions
  with attached patch (issue #18); contributed alarm/protection sourcing from
  0x501 (PR #25), alarm publishing to D-Bus (PR #24), stale-data disconnected
  state (PR #23), battery constant corrections (PR #26), and frame-length
  validation (PR #27). All changes tested against a real ELPM482-00005 on a
  Cerbo GX running Venus OS v3.70.
- This project traces its lineage to the original Samsung SDI CAN reverse
  engineering work by the Victron Community, the Super-B BMS integration by
  drurew, and the Venus OS driver patterns established by Louis van der Walt.

## License

MIT — see [LICENSE](LICENSE)
