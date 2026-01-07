#!/bin/bash
# Samsung SDI Victron Integration Installation Script
# This script installs the Samsung SDI BMS service on Venus OS

set -e

echo "Samsung SDI Victron Integration Installer"
echo "=========================================="

# Check if running on Venus OS
if [ ! -d "/opt/victronenergy" ]; then
    echo "ERROR: This script must be run on Victron Venus OS"
    exit 1
fi

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root"
    exit 1
fi

INSTALL_DIR="/data/samsung-sdi"
SERVICE_NAME="samsung-sdi-bms"
SERVICE_FILE="/service/${SERVICE_NAME}"

echo "Installing Samsung SDI Victron Integration..."

# Check if already installed
if [ -f "${SERVICE_FILE}" ]; then
    echo "Service already exists. Removing old installation..."
    rm -f "${SERVICE_FILE}"
    sleep 2
fi

# Create service file
echo "Creating service file..."
cat > "${SERVICE_FILE}" << EOF
#!/bin/sh
exec 2>&1
cd ${INSTALL_DIR}
exec python3 samsung_sdi_bms_service.py
EOF

chmod +x "${SERVICE_FILE}"

# Make sure CAN interface is configured
echo "Checking CAN interface..."
if [ ! -f "/etc/venus/can.conf" ]; then
    echo "Creating CAN configuration..."
    mkdir -p /etc/venus
    cat > /etc/venus/can.conf << EOF
interface=can0
bitrate=500000
EOF
fi

# Install Python dependencies
echo "Installing Python dependencies..."
pip3 install --quiet python-can dbus-python pygobject3 || {
    echo "Warning: Some Python packages may not have installed correctly"
    echo "You may need to install them manually: pip3 install python-can dbus-python pygobject3"
}

# Set permissions
echo "Setting permissions..."
chmod +x "${INSTALL_DIR}/"*.py
chmod +x "${INSTALL_DIR}/"*.sh

# Start service
echo "Starting service..."
svc -t /service/${SERVICE_NAME} || {
    echo "Warning: Could not start service with svc"
    echo "Try restarting the service manually: svc -t /service/${SERVICE_NAME}"
}

echo "Installation complete!"
echo ""
echo "Next steps:"
echo "1. Check service status: svstat /service/${SERVICE_NAME}"
echo "2. View logs: tail -f /service/${SERVICE_NAME}/log/main/current"
echo "3. Run diagnostics: cd ${INSTALL_DIR} && python3 diagnose_charging.py"
echo "4. Check Victron interface for Samsung SDI battery"
echo ""
echo "For detailed instructions, see docs/INSTALL.md"