#!/bin/sh
# Start the Samsung SDI BMS driver on a Cerbo GX

BMS_DIR="/data/samsung-sdi"
CAN_IFACE="${1:-can0}"

if [ -x "$BMS_DIR/samsung-sdi-bms" ]; then
    exec "$BMS_DIR/samsung-sdi-bms" "$CAN_IFACE"
elif [ -f "$BMS_DIR/src/samsung_sdi_bms_service.py" ]; then
    exec python3 "$BMS_DIR/src/samsung_sdi_bms_service.py"
else
    echo "ERROR: No BMS driver found in $BMS_DIR" >&2
    exit 1
fi
