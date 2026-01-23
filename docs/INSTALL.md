# Samsung SDI Victron Integration - Installation Guide

> **⚠️ IMPORTANT**: This guide assumes you have a working Venus OS setup with CAN bus configured. If you haven't set up CAN yet, see the [CAN Setup](#can-bus-setup) section first.

## 📋 Prerequisites

### Hardware Requirements
- ✅ Samsung SDI ELPM482-00005 battery module(s)
- ✅ Victron Cerbo GX or Venus GX (Venus OS v2.8x or later)
- ✅ CAN bus properly wired and terminated
- ✅ MultiPlus inverter/charger (recommended for ESS)

### Software Requirements
- ✅ Venus OS v2.8x or later
- ✅ CAN interface configured as `can0`
- ✅ SSH access to Venus OS device
- ✅ SCP/SFTP access for file transfer

### Network Requirements
- ✅ Venus OS device accessible via SSH (default: `root@192.168.x.x`)
- ✅ SCP/SFTP access for file uploads

## 🚀 Quick Installation (5 minutes)

### Step 1: Download and Transfer Files

**Option A: From GitHub (Recommended)**
```bash
# On your computer
git clone https://github.com/drurew/samsung-sdi-victron-integration.git
cd samsung-sdi-victron-integration

# Transfer to Venus OS
scp -r . root@venus-ip:/data/samsung-sdi/
```

**Option B: Direct Download**
```bash
# On Venus OS device
cd /data
wget https://github.com/drurew/samsung-sdi-victron-integration/archive/main.zip
unzip main.zip
mv samsung-sdi-victron-integration-main samsung-sdi
```

### Step 2: Install and Start Service

```bash
# SSH into Venus OS
ssh root@venus-ip

# Navigate to installation directory
cd /data/samsung-sdi

# Make scripts executable
chmod +x install.sh uninstall.sh

# Run installation
./install.sh
```

### Step 3: Verify Installation

```bash
# Check if service is running
ps aux | grep samsung_sdi

# Check D-Bus services
dbus -y com.victronenergy.battery.samsung_sdi_system1 /Soc

# Run diagnostics
python3 diagnose_charging.py
```

## 🔧 Detailed Installation Steps

### CAN Bus Setup

**Before installing this software, ensure your CAN bus is properly configured:**

1. **Connect Hardware**:
   - Wire Samsung SDI CAN_H and CAN_L to your CAN transceiver
   - Ensure proper termination (120Ω resistor at each end)
   - Connect transceiver to Venus OS CAN interface

2. **Configure CAN Interface**:
   ```bash
   # SSH into Venus OS
   ssh root@venus-ip

   # Edit CAN configuration
   vi /etc/venus/can.conf

   # Add or verify:
   interface=can0
   bitrate=500000
   ```

3. **Test CAN Interface**:
   ```bash
   # Bring up CAN interface
   /sbin/ip link set can0 up type can bitrate 500000

   # Check interface status
   ip -details link show can0

   # Monitor CAN traffic (should see Samsung SDI messages)
   candump can0
   ```

### Software Installation

#### Step 1: Prepare Venus OS

```bash
# Update system packages
opkg update
opkg install python3 python3-pip git

# Install required Python packages
pip3 install python-can dbus-python pygobject3
```

#### Step 2: Download Integration Files

```bash
# Create installation directory
mkdir -p /data/samsung-sdi
cd /data/samsung-sdi

# Clone repository
git clone https://github.com/yourusername/samsung-sdi-victron-integration.git .

# Or download ZIP and extract
wget https://github.com/yourusername/samsung-sdi-victron-integration/archive/main.zip
unzip main.zip
mv samsung-sdi-victron-integration-main/* .
rm -rf samsung-sdi-victron-integration-main main.zip
```

#### Step 3: Configure System

```bash
# Edit configuration file
vi config.ini

# Verify/modify settings:
[CAN]
interface = can0          # Your CAN interface
bitrate = 500000         # Samsung SDI bitrate
system_ids = 1           # Number of Samsung SDI systems

[Victron]
service_name_prefix = com.victronenergy.battery.samsung_sdi
device_instance_start = 280  # Unique device instance numbers
product_name = Samsung SDI ELPM482-00005

[Battery]
capacity = 4840.0        # Battery capacity in Wh
max_charge_current = 50.0    # Respect MultiPlus limits
max_discharge_current = 50.0 # Respect MultiPlus limits
```

#### Step 4: Install Service

```bash
# Make installation script executable
chmod +x install.sh

# Run installation
./install.sh

# Expected output:
# Installing Samsung SDI Victron Integration...
# Creating service file...
# Starting service...
# Service started successfully!
# Installation complete!
```

#### Step 5: Verify Operation

```bash
# Check service status
systemctl status samsung-sdi-bms

# View service logs
journalctl -u samsung-sdi-bms -f

# Test D-Bus interface
dbus -y com.victronenergy.battery.samsung_sdi_system1 /Soc
dbus -y com.victronenergy.battery.samsung_sdi_system1 /Dc/0/Voltage
dbus -y com.victronenergy.battery.samsung_sdi_system1 /Dc/0/Current
```

## 🖥️ Victron Configuration

### Cerbo GX / Venus GX Setup

1. **Access Venus OS Web Interface**:
   - Open browser to `http://venus-ip`
   - Login with your credentials

2. **Verify Battery Detection**:
   - Go to **Device List** → **Battery**
   - You should see "Samsung SDI ELPM482-00005" listed
   - Check SOC, voltage, and current readings

3. **ESS Configuration** (if using MultiPlus):
   - Go to **Settings** → **ESS**
   - Select **BatteryLife** or **Keep Batteries Charged** mode
   - Verify battery appears in **Battery Monitor** section

4. **Remote Console Setup** (optional):
   - Go to **Settings** → **Remote Console**
   - Enable VRM connection for remote monitoring

## 🔍 Testing and Validation

### Run Diagnostic Script

```bash
cd /data/samsung-sdi
python3 diagnose_charging.py
```

**Expected Output**:
```
Samsung SDI Diagnostic Tool
==========================

CAN Interface Status:
✓ can0: UP (500kbps)

Samsung SDI Systems Detected:
✓ System 1: Online
  - SOC: 85%
  - Voltage: 52.1V
  - Current: 2.3A
  - Temperature: 25°C

D-Bus Services:
✓ com.victronenergy.battery.samsung_sdi_system1: Active
✓ com.victronenergy.battery.samsung_sdi_aggregated: Active

ESS Compatibility:
✓ MultiPlus charge limit: 50A
✓ Battery aggregator: Active
```

### Test Script Execution

```bash
# Run comprehensive test
python3 test_samsung_sdi.py

# Expected: All tests pass
```

## 🛠️ Troubleshooting Installation Issues

### Service Won't Start

**Symptom**: `systemctl status samsung-sdi-bms` shows failed state

**Solutions**:
```bash
# Check service logs
journalctl -u samsung-sdi-bms -n 50

# Common issues:
# 1. CAN interface not configured
# 2. Missing Python dependencies
# 3. Configuration file errors
# 4. Permission issues
```

### No CAN Messages

**Symptom**: `candump can0` shows no traffic

**Solutions**:
```bash
# Check CAN interface
ip link show can0

# Restart CAN interface
ip link set can0 down
ip link set can0 up type can bitrate 500000

# Check wiring and termination
# Verify Samsung SDI is powered and communicating
```

### D-Bus Service Not Visible

**Symptom**: Battery not appearing in Victron interface

**Solutions**:
```bash
# Restart D-Bus
systemctl restart dbus

# Restart service
systemctl restart samsung-sdi-bms

# Check service registration
dbus-send --system --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep samsung
```

### Python Import Errors

**Symptom**: Service fails with import errors

**Solutions**:
```bash
# Install missing packages
pip3 install python-can dbus-python pygobject3

# Check Python path
python3 -c "import can, dbus, gi; print('All imports successful')"
```

## 📊 Monitoring and Maintenance

### Daily Checks

```bash
# Quick status check
cd /data/samsung-sdi
python3 diagnose_charging.py --quick
```

### Log Monitoring

```bash
# View recent logs
journalctl -u samsung-sdi-bms --since "1 hour ago"

# Follow logs in real-time
journalctl -u samsung-sdi-bms -f
```

### Performance Monitoring

```bash
# Check CPU usage
top -p $(pgrep -f samsung_sdi)

# Monitor CAN traffic
candump can0 | grep "500\|501\|502\|503\|504"
```

## 🔄 Updates and Upgrades

### Update Integration Software

```bash
cd /data/samsung-sdi

# Backup current config
cp config.ini config.ini.backup

# Pull latest changes
git pull origin main

# Restart service
systemctl restart samsung-sdi-bms
```

### Firmware Updates

**For Samsung SDI firmware updates:**
- Follow Samsung SDI documentation
- Stop integration service during update: `systemctl stop samsung-sdi-bms`
- Restart service after update: `systemctl start samsung-sdi-bms`

## 🆘 Getting Help

### Quick Diagnostics Script

Run this to gather system information for support:

```bash
cd /data/samsung-sdi
python3 diagnose_charging.py --full > diagnostic_report.txt
```

### Support Information

When asking for help, please provide:
- Venus OS version (`uname -a`)
- Diagnostic report output
- CAN interface configuration
- Service logs (`journalctl -u samsung-sdi-bms -n 100`)
- Samsung SDI system details

### Common Issues and Solutions

| Issue | Symptom | Solution |
|-------|---------|----------|
| No battery detected | Battery not visible in Victron | Check CAN wiring, restart service |
| Wrong SOC | SOC reading incorrect | Verify CAN message parsing, check config |
| Service crashes | Service stops unexpectedly | Check logs, verify dependencies |
| High CPU usage | System slow | Reduce logging, check CAN traffic |
| ESS not working | Charging not optimized | Verify aggregator configuration |

## 📞 Support Resources

- **GitHub Issues**: Report bugs and request features
- **GitHub Discussions**: Ask questions and share experiences
- **Victron Community**: General Victron integration questions
- **Samsung SDI Documentation**: Hardware-specific information

---

**🎉 Installation Complete!** Your Samsung SDI batteries should now be fully integrated with your Victron system. Monitor the Venus OS interface to confirm everything is working correctly.