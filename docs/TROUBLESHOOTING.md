# Troubleshooting Guide

This guide covers common issues with the Samsung SDI Victron integration and provides step-by-step solutions.

## 🔍 Quick Diagnostics

Run the diagnostic script first:

```bash
cd /data/samsung-sdi
python3 diagnose_charging.py
```

This will check:
- CAN interface status
- Samsung SDI system detection
- D-Bus service registration
- ESS compatibility

## 🚨 Common Issues

### 1. Service Won't Start

**Symptoms:**
- `systemctl status samsung-sdi-bms` shows failed
- Service not listed in process list

**Diagnostic Steps:**
```bash
# Check service status
systemctl status samsung-sdi-bms

# View detailed logs
journalctl -u samsung-sdi-bms -n 50

# Check for Python errors
python3 -c "import samsung_sdi_can_client, samsung_sdi_bms_service"
```

**Common Causes & Solutions:**

**Missing Dependencies:**
```bash
# Install required packages
pip3 install python-can dbus-python pygobject3
```

**CAN Interface Issues:**
```bash
# Check CAN interface
ip link show can0

# Restart CAN if needed
ip link set can0 down
ip link set can0 up type can bitrate 500000
```

**Configuration Errors:**
```bash
# Validate config file
python3 -c "import configparser; c=configparser.ConfigParser(); c.read('config.ini'); print('Config OK')"
```

### 2. No CAN Messages Received

**Symptoms:**
- `candump can0` shows no traffic
- Samsung SDI systems not detected
- Diagnostic shows "No systems found"

**Diagnostic Steps:**
```bash
# Check CAN interface status
ip -details link show can0

# Monitor CAN traffic
candump can0

# Check for any CAN errors
ip -statistics link show can0
```

**Common Causes & Solutions:**

**CAN Interface Down:**
```bash
# Bring up CAN interface
/sbin/ip link set can0 up type can bitrate 500000

# Verify bitrate
ip link show can0
```

**Wiring Issues:**
- Check CAN_H and CAN_L connections
- Verify termination resistors (120Ω at each end)
- Test continuity with multimeter
- Check Samsung SDI power and status LEDs

**Bitrate Mismatch:**
```bash
# Samsung SDI requires exactly 500kbps
ip link set can0 down
ip link set can0 up type can bitrate 500000
```

**Hardware Fault:**
- Test with known working CAN device
- Check Samsung SDI CAN transceiver
- Verify Venus OS CAN hardware

### 3. Battery Not Visible in Victron Interface

**Symptoms:**
- Battery doesn't appear in Device List
- No Samsung SDI battery in VRM
- ESS shows "No battery detected"

**Diagnostic Steps:**
```bash
# Check D-Bus services
dbus-send --system --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep samsung

# Test D-Bus paths
dbus -y com.victronenergy.battery.samsung_sdi_system1 /Soc

# Check service logs
journalctl -u samsung-sdi-bms -f
```

**Common Causes & Solutions:**

**Service Not Running:**
```bash
# Start service
systemctl start samsung-sdi-bms

# Enable auto-start
systemctl enable samsung-sdi-bms
```

**D-Bus Registration Failed:**
```bash
# Restart D-Bus
systemctl restart dbus

# Restart service
systemctl restart samsung-sdi-bms
```

**Device Instance Conflict:**
```bash
# Check for conflicting device instances
dbus-send --system --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep battery

# Change device instance in config.ini
vi config.ini
# Modify device_instance_start to unused number (280-299)
```

### 4. Incorrect SOC or Voltage Readings

**Symptoms:**
- SOC doesn't match Samsung SDI display
- Voltage readings are wrong
- Current readings incorrect

**Diagnostic Steps:**
```bash
# Monitor raw CAN messages
candump can0 | grep "500\|501\|502"

# Test CAN client parsing
python3 -c "
from samsung_sdi_can_client import SamsungSDICANClient
client = SamsungSDICANClient('can0', 500000)
client.connect()
data = client.read_system_status()
print('Parsed data:', data)
"
```

**Common Causes & Solutions:**

**CAN Message Format Issues:**
- Verify Samsung SDI firmware version
- Check for protocol documentation updates
- Compare raw CAN data with expected format

**Byte Order Problems:**
```bash
# Test with different endianness
# Edit samsung_sdi_can_client.py if needed
```

**Scaling Factor Errors:**
- Check voltage/current scaling in CAN client
- Verify units (V, A, %) match Victron expectations

### 5. ESS Charging Issues

**Symptoms:**
- MultiPlus not charging properly
- ESS shows optimization disabled
- Battery aggregator not working

**Diagnostic Steps:**
```bash
# Check aggregator status
dbus -y com.victronenergy.battery.samsung_sdi_aggregated /Info/MaxChargeCurrent

# Monitor charging decisions
python3 diagnose_charging.py --charging

# Check MultiPlus settings
dbus -y com.victronenergy.settings /Settings/CGwacs/BatteryLife/State
```

**Common Causes & Solutions:**

**Aggregator Not Active:**
```bash
# Check aggregator logs
journalctl -u samsung-sdi-bms | grep aggregator

# Verify aggregator configuration
vi config.ini
# Check [Battery] section settings
```

**MultiPlus Limits Too Low:**
```bash
# Check MultiPlus charge current limit
dbus -y com.victronenergy.vebus.ttyO1 /Ac/ActiveIn/L1/I  # Adjust device path

# Increase limit if needed (50A max for MultiPlus)
```

**ESS Mode Configuration:**
- Verify ESS is enabled in Venus OS
- Check battery life settings
- Confirm DC coupling configuration

### 6. High CPU Usage

**Symptoms:**
- System running slow
- High CPU usage by Python processes
- Service consuming excessive resources

**Diagnostic Steps:**
```bash
# Check CPU usage
top -p $(pgrep -f samsung_sdi)

# Monitor process over time
ps aux | grep samsung

# Check CAN traffic volume
candump can0 | head -20
```

**Common Causes & Solutions:**

**Excessive Logging:**
```bash
# Reduce log level in service
vi samsung_sdi_bms_service.py
# Change logging level from DEBUG to INFO
```

**CAN Traffic Flood:**
- Check for CAN bus errors
- Verify proper termination
- Reduce CAN message frequency if possible

**Memory Leaks:**
```bash
# Monitor memory usage
ps aux --sort=-%mem | head

# Restart service periodically
systemctl restart samsung-sdi-bms
```

## 🛠️ Advanced Troubleshooting

### CAN Bus Analysis

**Monitor CAN Traffic:**
```bash
# Capture traffic to file
candump -l can0

# Analyze specific message IDs
candump can0 | grep " 500 "  # System status
candump can0 | grep " 501 "  # System config
candump can0 | grep " 502 "  # Limits
```

**CAN Bus Load:**
```bash
# Check bus utilization
candump can0 | wc -l  # Messages per second
```

### D-Bus Debugging

**List All Victron Services:**
```bash
dbus-send --system --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep victron
```

**Monitor D-Bus Traffic:**
```bash
# Monitor all D-Bus traffic (verbose)
dbus-monitor --system

# Monitor specific service
dbus-monitor --system "type='signal',sender='com.victronenergy.battery.samsung_sdi_system1'"
```

### Python Debugging

**Test Individual Components:**
```bash
# Test CAN client
python3 -c "
from samsung_sdi_can_client import SamsungSDICANClient
client = SamsungSDICANClient('can0', 500000)
print('CAN client created successfully')
"

# Test service initialization
python3 -c "
import samsung_sdi_bms_service
print('Service imports successful')
"
```

**Enable Debug Logging:**
```bash
# Temporary debug run
python3 samsung_sdi_bms_service.py --debug
```

### System Resource Monitoring

**Monitor System Resources:**
```bash
# CPU and memory
top

# Disk space
df -h

# Network
ifconfig

# CAN statistics
ip -statistics link show can0
```

## 📊 Diagnostic Report Generation

Generate a comprehensive diagnostic report:

```bash
cd /data/samsung-sdi

# Create diagnostic report
python3 diagnose_charging.py --full > diagnostic_$(date +%Y%m%d_%H%M%S).txt

# Include system information
echo "=== System Information ===" >> diagnostic_report.txt
uname -a >> diagnostic_report.txt
python3 --version >> diagnostic_report.txt
pip3 list | grep -E "(can|dbus)" >> diagnostic_report.txt

# Include CAN configuration
echo "=== CAN Configuration ===" >> diagnostic_report.txt
ip link show can0 >> diagnostic_report.txt
cat /etc/venus/can.conf >> diagnostic_report.txt

# Include service logs
echo "=== Service Logs ===" >> diagnostic_report.txt
journalctl -u samsung-sdi-bms -n 100 >> diagnostic_report.txt
```

## 🚑 Emergency Procedures

### Service Won't Stop
```bash
# Force kill process
pkill -f samsung_sdi_bms_service.py

# Remove service file
rm /service/samsung-sdi-bms
```

### CAN Bus Hung
```bash
# Reset CAN interface
ip link set can0 down
sleep 2
ip link set can0 up type can bitrate 500000 restart-ms 100
```

### Complete Reinstall
```bash
# Stop and remove service
./uninstall.sh

# Remove files
cd /data
rm -rf samsung-sdi

# Reinstall from scratch
# Follow installation guide
```

## 📞 Getting Help

### Information to Provide

When seeking help, include:

1. **Venus OS Version:**
   ```bash
   uname -a
   cat /opt/victronenergy/version
   ```

2. **Diagnostic Report:**
   ```bash
   python3 diagnose_charging.py --full
   ```

3. **Service Logs:**
   ```bash
   journalctl -u samsung-sdi-bms -n 200
   ```

4. **CAN Traffic Sample:**
   ```bash
   timeout 10 candump can0
   ```

5. **Configuration:**
   ```bash
   cat config.ini
   ```

### Support Channels

- **GitHub Issues**: For bugs and feature requests
- **GitHub Discussions**: For questions and community support
- **Victron Community Forums**: For general Victron integration questions

## 🔧 Maintenance Tasks

### Regular Checks

**Weekly:**
```bash
# Check service status
systemctl status samsung-sdi-bms

# Verify CAN connectivity
python3 diagnose_charging.py --quick

# Check disk space
df -h /data
```

**Monthly:**
```bash
# Review logs for errors
journalctl -u samsung-sdi-bms --since "30 days ago" | grep -i error

# Update software
cd /data/samsung-sdi
git pull origin main
systemctl restart samsung-sdi-bms
```

### Log Rotation

Venus OS handles log rotation automatically, but you can monitor log sizes:

```bash
# Check log sizes
journalctl --disk-usage
ls -lh /var/log/
```

### Backup Configuration

```bash
# Backup config
cp config.ini config.ini.backup

# Backup entire installation
tar -czf samsung-sdi-backup-$(date +%Y%m%d).tar.gz /data/samsung-sdi
```

---

**Remember**: Most issues are resolved by checking CAN connectivity and service status. Start with the diagnostic script and work through the checklist systematically.