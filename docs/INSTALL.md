# Installation Guide

## Prerequisites

- Victron Cerbo GX with SSH access
- Samsung SDI ELPM482-00005 battery module
- Custom CAN crossover cable (see pinout in README)
- CAN interface configured at 500 kbps

## 1. Configure the CAN Interface

The Cerbo GX must have a CAN interface set to 500 kbps.

### Manual Configuration

```bash
ip link set can0 type can bitrate 500000
ip link set can0 up
```

For persistent configuration, add to a startup script.

## 2. Install Build Tools (one-time)

```bash
opkg update
opkg install gcc gcc-symlinks binutils libgcc-s-dev
```

## 3. Build and Install

```bash
cd /data
git clone https://github.com/drurew/samsung-sdi-victron-integration.git
cd samsung-sdi-victron-integration
make
cp samsung-sdi-bms /data/samsung-sdi/
```

## 4. Start the Driver

### Manual Start

```bash
/data/samsung-sdi/samsung-sdi-bms can0 &
```

### Automatic Startup (daemontools)

```bash
mkdir -p /service/samsung-sdi-bms/log

cat > /service/samsung-sdi-bms/run << 'EOF'
#!/bin/sh
exec 2>&1
exec /data/samsung-sdi/samsung-sdi-bms can0
EOF

cat > /service/samsung-sdi-bms/log/run << 'EOF'
#!/bin/sh
exec multilog t s25000 n4 /var/log/samsung-sdi-bms
EOF

chmod +x /service/samsung-sdi-bms/run
chmod +x /service/samsung-sdi-bms/log/run
```

## 5. Verify Operation

```bash
ps | grep samsung-sdi-bms
dbus -y com.victronenergy.battery.samsung_sdi /Soc GetValue
dbus -y com.victronenergy.battery.samsung_sdi /Dc/0/Voltage GetValue
dbus -y com.victronenergy.battery.samsung_sdi /Info/MaxChargeCurrent GetValue
```

## 6. System Configuration

Set the Samsung SDI as the active battery service for DVCC:

```bash
dbus -y com.victronenergy.settings /Settings/SystemSetup/BatteryService \
  SetValue "com.victronenergy.battery.samsung_sdi"
```

## Troubleshooting

### No D-Bus service appears

Check CAN interface is up and receiving data:
```bash
ip link show can0
candump can0 | head -20
```

Expected output should show messages on CAN IDs 0x500-0x504 every 500 ms.

### Driver exits immediately

Run in foreground to see errors:
```bash
/data/samsung-sdi/samsung-sdi-bms can0
```
