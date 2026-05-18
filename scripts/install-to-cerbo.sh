#!/bin/bash
# Deploy Samsung SDI BMS driver to Victron Cerbo GX
# Run from the repository root on your development machine.
#
# Usage: ./scripts/install-to-cerbo.sh <cerbo-ip> [password]

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <cerbo-ip> [password]"
    exit 1
fi

CERBO_IP="$1"
CERBO_PASS="${2:-root}"

if ! command -v sshpass &> /dev/null; then
    echo "sshpass is required. Install it:"
    echo "  Fedora: sudo dnf install sshpass"
    echo "  Debian: sudo apt install sshpass"
    exit 1
fi

SSH="sshpass -p $CERBO_PASS ssh -o StrictHostKeyChecking=no root@$CERBO_IP"
SCP="sshpass -p $CERBO_PASS scp -o StrictHostKeyChecking=no"

echo "=== Samsung SDI BMS Driver Installation ==="
echo "Target: $CERBO_IP"
echo ""

echo "[1/5] Copying source files..."
$SSH 'mkdir -p /data/samsung-sdi/src /data/samsung-sdi/docs /data/samsung-sdi/scripts'
$SCP src/samsung-sdi-bms.c src/samsung_sdi_bms_service.py \
     src/samsung_sdi_can_client.py \
     root@$CERBO_IP:/data/samsung-sdi/src/
$SCP Makefile config.ini root@$CERBO_IP:/data/samsung-sdi/
$SCP docs/INSTALL.md root@$CERBO_IP:/data/samsung-sdi/docs/
echo "  Done."

echo "[2/5] Installing build tools..."
$SSH 'opkg update && opkg install gcc gcc-symlinks binutils libgcc-s-dev'
echo "[3/5] Compiling C driver..."
$SSH 'cd /data/samsung-sdi && make'
echo "  Done."

echo "[4/5] Configuring CAN interface (500 kbps)..."
$SSH 'ip link set can0 type can bitrate 500000 2>/dev/null; ip link set can0 up 2>/dev/null'
echo "  Done."

echo "[5/5] Installing daemontools service..."
$SSH 'mkdir -p /service/samsung-sdi-bms/log'
$SSH 'cat > /service/samsung-sdi-bms/run << "RUNEOF"
#!/bin/sh
exec 2>&1
exec /data/samsung-sdi/samsung-sdi-bms can0
RUNEOF'
$SSH 'cat > /service/samsung-sdi-bms/log/run << "LOGEOF"
#!/bin/sh
exec multilog t s25000 n4 /var/log/samsung-sdi-bms
LOGEOF'
$SSH 'chmod +x /service/samsung-sdi-bms/run /service/samsung-sdi-bms/log/run'
echo "  Done."

sleep 4

echo ""
echo "=== Verification ==="
$SSH '
echo "Process:"
ps | grep samsung-sdi-bms | grep -v grep || echo "  (starting up)"
echo ""
echo "Battery data:"
soc=$(dbus -y com.victronenergy.battery.samsung_sdi /Soc GetValue 2>/dev/null || echo "not yet")
echo "  SoC: $soc%"
voltage=$(dbus -y com.victronenergy.battery.samsung_sdi /Dc/0/Voltage GetValue 2>/dev/null || echo "not yet")
echo "  Voltage: $voltage"
'

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Useful commands on the Cerbo:"
echo "  Status:   svstat /service/samsung-sdi-bms"
echo "  Logs:     tail -f /var/log/samsung-sdi-bms/current"
echo "  Restart:  svc -t /service/samsung-sdi-bms"
echo ""
echo "Set as system battery service:"
echo "  dbus -y com.victronenergy.settings /Settings/SystemSetup/BatteryService \\"
echo "    SetValue 'com.victronenergy.battery.samsung_sdi'"
