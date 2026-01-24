# Samsung SDI Victron Integration

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Python 3.7+](https://img.shields.io/badge/python-3.7+-blue.svg)](https://www.python.org/downloads/)

A complete Victron Venus OS integration for Samsung SDI ELPM482-00005 lithium-ion battery modules. Seamlessly integrates Samsung SDI battery systems with Victron Energy equipment including MultiPlus, Cerbo GX, and other Venus OS devices.

## Project Lineage

This driver was originally forked from the **Super-B Victron Integration** project and adapted for the Samsung SDI ELPM482-00005 based on factory specifications.

As the project evolved, we have integrated advanced logic from the open-source community to improve safety and performance:

*   **Protocol Logic**: Originally implemented from Samsung SDI factory datasheets.
*   **Safety Enhancements**: We have since adopted best-in-class safety algorithms (DVCC, Temperature Derating) inspired by **[Louisvdw/dbus-serialbattery](https://github.com/Louisvdw/dbus-serialbattery)**.
*   **Community Validation**: Protocol details validated against findings from **[ploys/dbus-samsung-sdi](https://github.com/ploys/dbus-samsung-sdi)**.

### Why Choose This Driver?

We have focused on **Stability, Safety, and Ease of Install**.

| Feature | This Driver | Generic / Older Drivers |
| :--- | :---: | :---: |
| **Safety Watchdog** | ✅ **Active** (Disconnects if CAN signal lost >5s) | ❌ Risk of "Frozen Values" |
| **BMS Keep-Alive** | ✅ **Auto-Heartbeat** (Prevents sleep mode) | ⚠️ Often Manual / Missing |
| **Inrush Protection** | ✅ **Soft-Start Ramp** (0-100% over 15s) | ❌ Hard ON (Trips Breakers) |
| **Cold Weather** | ✅ **Smart Derating** (Stops charge <0°C) | ⚠️ Manual Config Only |
| **Control Jitter** | ✅ **Hysteresis** (Smooths fan/relay noise) | ❌ Constant Flux (49A-50A-49A) |
| **Installation** | ✅ **"Blind Install"** (USB Plug & Play) | ❌ SSH / Command Line |
| **Updates** | ✅ **Persist-on-Update** (Survives Firmware) | ❌ Re-install required |

## Quick Start

**For complete installation instructions, see [INSTALL.md](docs/INSTALL.md)**

### Method 1: Automatic "Blind Install" (USB/SD Card)
This is the easiest method. It uses the built-in Victron auto-install mechanism.

1.  Download the **`venus-data.tar.gz`** file from the Releases page.
    *   *Note: If you build it yourself, `create_package.sh` now outputs this filename.*
2.  **DO NOT EXTRACT IT.** Copy the file exactly as-is to the **root** of a USB stick or SD card.
    *   Ensure the filename is exactly `venus-data.tar.gz`.
3.  Insert the media into your Victron GX device (Cerbo/VenusGX).
4.  **Reboot the device.**
5.  On boot, the system will automatically unpack the files to `/data` and run the installer.
6.  Check `/var/log/samsung_install.log` if you need to debug.

### Method 2: Manual Install (SSH)
If you prefer to see what's happening:

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
