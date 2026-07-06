#!/bin/sh
# Start the Samsung SDI BMS driver on a Cerbo GX
#
# Two modes:
#   1. C translator (native): Samsung SDI CAN → Victron CAN-BMS frames
#      The stock Venus OS CAN-BMS driver picks up the translated frames.
#   2. Python driver (fallback): Direct D-Bus publishing via vedbus

BMS_DIR="/data/samsung-sdi"
CAN_IFACE="${1:-can0}"

# Prefer the C translator — simpler, no D-Bus code, uses Victron's
# stock CAN-BMS driver which has proven watchdog/reconnect behaviour.
if [ -x "$BMS_DIR/samsung-sdi-bms" ]; then
    exec "$BMS_DIR/samsung-sdi-bms" "$CAN_IFACE"
elif [ -f "$BMS_DIR/src/samsung_sdi_bms_service.py" ]; then
    exec python3 "$BMS_DIR/src/samsung_sdi_bms_service.py" "$BMS_DIR/config.ini"
else
    echo "ERROR: No BMS driver found in $BMS_DIR" >&2
    exit 1
fi
