#!/bin/bash
# Deploy Samsung SDI BMS driver to Victron Cerbo GX
# Run from the repository root on your development machine.
#
# The driver publishes all 28 Samsung SDI fields directly to D-Bus
# (com.victronenergy.battery.samsung_sdi), giving full telemetry in
# the Venus OS GUI, DVCC control, and VRM.
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

echo "=== Samsung SDI BMS Driver (D-Bus) ==="
echo "Target: $CERBO_IP"
echo ""

echo "[1/3] Copying source files..."
$SSH 'mkdir -p /data/samsung-sdi'
$SCP src/samsung-sdi-bms.c Makefile root@$CERBO_IP:/data/samsung-sdi/
echo "  Done."

echo "[2/3] Installing build dependencies and compiling..."
$SSH 'opkg update && opkg install gcc gcc-symlinks binutils libgcc-s-dev libdbus-1-dev'
$SSH 'cd /data/samsung-sdi && make'
echo "  Done."

echo "[3/3] Installing daemontools service..."
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

echo ""
echo "=== Verification ==="
$SSH '
echo "Process:"
ps | grep samsung-sdi-bms | grep -v grep || echo "  (starting up)"
echo ""
echo "D-Bus battery service:"
soc=$(dbus -y com.victronenergy.battery.samsung_sdi /Soc GetValue 2>/dev/null || echo "not yet")
echo "  SoC: $soc"
voltage=$(dbus -y com.victronenergy.battery.samsung_sdi /Dc/0/Voltage GetValue 2>/dev/null || echo "not yet")
echo "  Voltage: $voltage"
cells=$(dbus -y com.victronenergy.battery.samsung_sdi /Voltages/Cell1 GetValue 2>/dev/null || echo "not yet")
echo "  Cell 1: $cells"
'

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Architecture:"
echo "  Samsung SDI BMS -> CAN -> samsung-sdi-bms -> D-Bus -> Venus OS GUI"
echo ""
echo "To also enable Victron CAN-BMS translation (limited data subset):"
echo "  Edit /service/samsung-sdi-bms/run and add --can-bms flag"
echo ""
echo "  Then: dbus -y com.victronenergy.settings /Settings/Canbus/can0/Profile SetValue 4"
echo ""
echo "Useful commands on the Cerbo:"
echo "  Status:   svstat /service/samsung-sdi-bms"
echo "  Logs:     tail -f /var/log/samsung-sdi-bms/current"
echo "  Restart:  svc -t /service/samsung-sdi-bms"
echo "  D-Bus:    dbus -y com.victronenergy.battery.samsung_sdi"
