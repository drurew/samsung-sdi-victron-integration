# Samsung SDI Victron Integration

A complete Victron Venus OS integration for Samsung SDI ELPM482-00005 lithium-ion battery modules.

## Overview

This project provides seamless integration between Samsung SDI battery systems and Victron Energy equipment (MultiPlus, Cerbo GX, etc.) through the Venus OS D-Bus interface.

### Features

- **CAN Communication**: Direct CAN 2.0A communication at 500kbps with Samsung SDI modules
- **Real-time Monitoring**: System voltage, current, SOC, SOH, temperature, and alarm status
- **Intelligent Aggregation**: Battery aggregator module for MultiPlus ESS compatibility
- **Charge Current Management**: Respects Samsung SDI charge/discharge limits
- **D-Bus Integration**: Full Victron D-Bus service implementation
- **ESS Compatible**: Optimized for Energy Storage System operation

## Hardware Requirements

- **Samsung SDI ELPM482-00005**: 4.84kWh lithium-ion battery module
- **Victron Cerbo GX or Venus GX**: For Venus OS platform
- **CAN Interface**: Properly configured CAN bus (can0) at 500kbps
- **MultiPlus Inverter/Charger**: For ESS operation (optional)

## Installation

### 1. Hardware Setup

1. Connect Samsung SDI module to CAN bus
2. Configure CAN interface for 500kbps operation
3. Ensure proper CAN termination

### 2. Software Installation

```bash
# Copy files to Venus OS
scp -r samsung_sdi_victron_integration/ root@venus:/data/samsung-sdi/

# Install Python dependencies (if needed)
opkg update
opkg install python3-can python3-dbus
```

### 3. Configuration

Edit `/data/samsung-sdi/config.ini`:

```ini
[CAN]
interface = can0
bitrate = 500000
system_ids = 1

[Victron]
service_name_prefix = com.victronenergy.battery.samsung_sdi
device_instance_start = 280
product_name = Samsung SDI ELPM482-00005
update_interval = 1.0

[Battery]
capacity = 4840.0
max_charge_current = 50.0
max_discharge_current = 50.0
```

### 4. Service Setup

Create systemd service `/etc/systemd/system/samsung-sdi-bms.service`:

```ini
[Unit]
Description=Samsung SDI BMS Integration
After=network.target dbus.target

[Service]
Type=simple
User=root
ExecStart=/usr/bin/python3 /data/samsung-sdi/samsung_sdi_bms_service.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable and start the service:

```bash
systemctl daemon-reload
systemctl enable samsung-sdi-bms
systemctl start samsung-sdi-bms
```

## CAN Message Specification

Based on Samsung SDI Product Specification ELPM482-00005 Rev 0.2:

| CAN ID | Description | Key Parameters |
|--------|-------------|----------------|
| 0x500 | System Status | Voltage, Current, SOC, SOH, Alarms |
| 0x501 | System Config | Tray counts, Protection status |
| 0x502 | Charge/Discharge Limits | Charge/Discharge current limits |
| 0x503 | Cell Summary | Min/Max/Avg cell voltages |
| 0x504 | Temperature Summary | Min/Max/Avg temperatures |

## D-Bus Services

### Individual System Service
- **Service**: `com.victronenergy.battery.samsung_sdi_system1`
- **Paths**:
  - `/Soc`: State of charge (%)
  - `/Dc/0/Voltage`: System voltage (V)
  - `/Dc/0/Current`: System current (A)
  - `/Dc/0/Temperature`: Average temperature (°C)
  - `/Capacity`: Total capacity (Wh)

### Aggregated Service
- **Service**: `com.victronenergy.battery.samsung_sdi_aggregated`
- **Paths**:
  - `/Soc`: Aggregated SOC (%)
  - `/Info/MaxChargeCurrent`: Maximum charge current (A)
  - `/Dc/0/Voltage`: Aggregated voltage (V)

## Troubleshooting

### Diagnostic Script

Run the diagnostic script to check system status:

```bash
cd /data/samsung-sdi/
python3 diagnose_charging.py
```

### Common Issues

1. **CAN Communication**: Check CAN interface configuration
   ```bash
   ip link set can0 up type can bitrate 500000
   ```

2. **D-Bus Services**: Verify services are running
   ```bash
   dbus -y com.victronenergy.battery.samsung_sdi_system1 /Soc GetValue
   ```

3. **MultiPlus Charging**: Check ESS configuration
   - Set mode to "Optimized with BatteryLife"
   - Verify DVCC MaxChargeCurrent is unlimited

### Logs

Check service logs:

```bash
journalctl -u samsung-sdi-bms -f
```

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

## Configuration Options

### CAN Settings
- `interface`: CAN interface (default: can0)
- `system_ids`: Comma-separated system IDs (default: 1)

### Battery Settings
- `capacity`: Battery capacity in Wh (default: 4840.0)
- `max_charge_current`: Maximum charge current in A (default: 50.0)
- `max_discharge_current`: Maximum discharge current in A (default: 50.0)

### Aggregation Settings
- `multiplus_min_power`: Minimum discharge power for MultiPlus (default: 1200.0)

## Compatibility

- **Venus OS**: v2.8x and later
- **MultiPlus**: All models with ESS support
- **Cerbo GX**: All firmware versions
- **CAN Interface**: SocketCAN compatible

## License

This project is provided as-is for educational and integration purposes.

## Support

For issues specific to Samsung SDI hardware, consult the official Samsung SDI documentation.

For Victron integration issues, refer to the Victron community forums and documentation.