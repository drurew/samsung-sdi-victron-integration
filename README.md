# Samsung SDI Victron Integration

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Python 3.7+](https://img.shields.io/badge/python-3.7+-blue.svg)](https://www.python.org/downloads/)

A complete Victron Venus OS integration for Samsung SDI ELPM482-00005 lithium-ion battery modules. Seamlessly integrates Samsung SDI battery systems with Victron Energy equipment including MultiPlus, Cerbo GX, and other Venus OS devices.

## Quick Start

**For complete installation instructions, see [INSTALL.md](docs/INSTALL.md)**

### Method 1: SetupHelper (Package Manager) - Recommended
1.  **Online Install**: Add the repository URL to SetupHelper and install from the menu.
2.  **Offline/USB Install (Windows/Mac/Linux)**:
    *   Download the verified release package (`samsung-sdi-victron-integration-vX.X.X.tar.gz`) from the **[GitHub Releases](https://github.com/drurew/samsung-sdi-victron-integration/releases)** page.
    *   **DO NOT EXTRACT** the file. Copy the `.tar.gz` file directly to a USB stick.
    *   Insert into Cerbo GX and install via SetupHelper's "Install from USB/Storage" option.
    *   *(Advanced Users)*: You can also build it yourself using `./create_package.sh` on Linux/Mac.

### Method 2: Manual Install

```bash
# 1. Download/Clone this repository
wget https://github.com/drurew/samsung-sdi-victron-integration/archive/refs/heads/main.zip
unzip main.zip
mv samsung-sdi-victron-integration-main /data/samsung-sdi-victron-integration

# 2. Run setup
cd /data/samsung-sdi-victron-integration
chmod +x setup
./setup install
```

## Features

- ** CAN Communication**: Direct CAN 2.0A communication at 500kbps with Samsung SDI modules
- ** Real-time Monitoring**: System voltage, current, SOC, SOH, temperature, and alarm status
- ** Intelligent Aggregation**: Battery aggregator module for MultiPlus ESS compatibility
- ** Smart Charging**: Respects Samsung SDI charge/discharge limits and MultiPlus capabilities
- ** D-Bus Integration**: Full Victron D-Bus service implementation
- ** ESS Compatible**: Optimized for Energy Storage System operation
- ** Easy Configuration**: Simple INI file configuration
- ** Monitoring Tools**: Built-in diagnostic and monitoring scripts

## Hardware Requirements

- **Samsung SDI ELPM482-00005**: 4.84kWh lithium-ion battery module
- **Victron Cerbo GX or Venus GX**: Venus OS platform (v2.8x+)
- **CAN Interface**: Properly configured CAN bus (can0) at 500kbps
- **MultiPlus Inverter/Charger**: For ESS operation (optional but recommended)

## CAN Message Support

Based on Samsung SDI Product Specification ELPM482-00005 Rev 0.2:

| CAN ID | Description | Key Parameters |
|--------|-------------|----------------|
| 0x500 | System Status | Voltage, Current, SOC, SOH, Alarms |
| 0x501 | System Config | Tray counts, Protection status |
| 0x502 | Charge/Discharge Limits | Charge/Discharge current limits |
| 0x503 | Cell Summary | Min/Max/Avg cell voltages |
| 0x504 | Temperature Summary | Min/Max/Avg temperatures |

## Architecture

```
Samsung SDI Module ── CAN 2.0A (500kbps) ── SamsungSDICANClient
                                                          │
                                                          ▼
SamsungSDIMonitor ── D-Bus ── Venus OS ── BatteryAggregator
                                                          │
                                                          ▼
MultiPlus / Cerbo GX ── ESS Integration ── Grid/Charging Control
```

## Project Structure

```
samsung-sdi-victron-integration/
├── samsung_sdi_can_client.py          # Samsung SDI CAN communication client
├── samsung_sdi_bms_service.py         # Main D-Bus service
├── battery_aggregator/                # Intelligent battery aggregation module
│   ├── __init__.py                    # Battery aggregator core
│   └── delegates/                     # ESS compatibility delegates
├── config.ini                         # Configuration file
├── install.sh                         # Automated installation script
├── diagnose_charging.py               # Diagnostic and monitoring script
├── test_samsung_sdi.py               # Test and validation script
└── docs/                             # Documentation
    ├── INSTALL.md                     # Detailed installation guide
    ├── TROUBLESHOOTING.md            # Troubleshooting guide
    └── README.md                      # Additional documentation
```

## Configuration

assure you use the correct interface. vecan0, can0 etc. 

Edit `config.ini` to match your setup:

```ini
[CAN]
interface = can0
bitrate = 500000
system_ids = 1

[Victron]
service_name_prefix = com.victronenergy.battery.samsung_sdi
device_instance_start = 280
product_name = Samsung SDI ELPM482-00005

[Battery]
capacity = 4840.0
max_charge_current = 50.0
max_discharge_current = 50.0
```

## D-Bus Services

### Individual System Service
- **Service**: `com.victronenergy.battery.samsung_sdi_system1`
- **Paths**:
  - `/Soc`: State of charge (%)
  - `/Dc/0/Voltage`: System voltage (V)
  - `/Dc/0/Current`: System current (A)
  - `/Dc/0/Temperature`: Average temperature (°C)

### Aggregated Service
- **Service**: `com.victronenergy.battery.samsung_sdi_aggregated`
- **Paths**:
  - `/Soc`: Aggregated SOC (%)
  - `/Info/MaxChargeCurrent`: Maximum charge current (A)

##  Troubleshooting

**For detailed troubleshooting, see [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)**

Quick diagnostics:
```bash
cd /data/samsung-sdi
python3 diagnose_charging.py
```

##  Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

##  License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

## Disclaimer

This software is provided as-is for educational and integration purposes. Users are responsible for ensuring compatibility with their specific hardware and use cases. Always follow proper safety procedures when working with electrical systems.

##  Support

- **Issues**: [GitHub Issues](https://github.com/drurew/samsung-sdi-victron-integration/issues)
- **Discussions**: [GitHub Discussions](https://github.com/drurew/samsung-sdi-victron-integration/discussions)

---

**Made with ❤ for the Victron & Samsung SDI communities**
