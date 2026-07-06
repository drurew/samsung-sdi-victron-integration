#!/bin/bash
# Deploy Samsung SDI → Victron CAN-BMS protocol translator to Cerbo GX
# Run from the repository root on your development machine.
#
# The translator reads Samsung SDI CAN PDOs (0x500-0x504, 0x5F0-0x5F4)
# and re-transmits them as Victron CAN-bus BMS frames (0x351, 0x355,
# 0x356, 0x35A).  The stock Venus OS CAN-BMS driver then publishes to
# D-Bus — no custom D-Bus code needed.
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

echo "=== Samsung SDI → Victron CAN-BMS Translator ==="
echo "Target: $CERBO_IP"
echo ""

echo "[1/4] Copying source files..."
$SSH 'mkdir -p /data/samsung-sdi'
$SCP src/samsung-sdi-bms.c Makefile \
     root@$CERBO_IP:/data/samsung-sdi/
echo "  Done."

echo "[2/4] Installing build tools and compiling..."
$SSH 'opkg update && opkg install gcc gcc-symlinks binutils libgcc-s-dev'
$SSH 'cd /data/samsung-sdi && make'
echo "  Done."

echo "[3/4] Installing daemontools service..."
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

sleep 3

echo "[4/4] Configuring Victron CAN-BMS driver..."
# Enable CAN-bus BMS profile on can0 (profile 4 = CAN-bus BMS 500 kbit/s)
$SSH 'dbus -y com.victronenergy.settings /Settings/Canbus/can0/Profile SetValue 4' 2>/dev/null || {
    echo "  Note: Could not set CAN profile via D-Bus."
    echo "  Configure manually in Venus OS: Settings → Services → CAN-bus BMS (500 kbit/s)"
}
echo "  Done."

sleep 2

echo ""
echo "=== Verification ==="
$SSH '
echo "Translator process:"
ps | grep samsung-sdi-bms | grep -v grep || echo "  (starting up)"
echo ""
echo "Victron CAN-BMS service:"
ps | grep can-bus-bms | grep -v grep || echo "  (checking...)"
echo ""
echo "Battery data (via Victron CAN-BMS):"
soc=$(dbus -y com.victronenergy.battery.can0 /Soc GetValue 2>/dev/null || echo "not yet")
echo "  SoC: $soc"
voltage=$(dbus -y com.victronenergy.battery.can0 /Dc/0/Voltage GetValue 2>/dev/null || echo "not yet")
echo "  Voltage: $voltage"
'

echo ""
echo "=== Installation Complete ==="
echo ""
echo "What this does:"
echo "  Samsung SDI BMS → CAN (0x500-0x504) → Translator (samsung-sdi-bms)"
echo "    → Victron CAN-BMS frames (0x351, 0x355, 0x356, 0x35A)"
echo "    → Victron can-bus-bms driver → D-Bus → Venus OS GUI"
echo ""
echo "Useful commands on the Cerbo:"
echo "  Translator status: svstat /service/samsung-sdi-bms"
echo "  Translator logs:   tail -f /var/log/samsung-sdi-bms/current"
echo "  Restart translator: svc -t /service/samsung-sdi-bms"
echo ""
echo "Troubleshooting:"
echo "  Check CAN traffic:  candump can0"
echo "  Check D-Bus battery: dbus -y com.victronenergy.battery.can0 /Soc GetValue"
