#!/bin/bash
# Samsung SDI Victron Integration Uninstallation Script
# This script removes the Samsung SDI BMS service from Venus OS

set -e

echo "Samsung SDI Victron Integration Uninstaller"
echo "==========================================="

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root"
    exit 1
fi

SERVICE_NAME="samsung-sdi-bms"
SERVICE_FILE="/service/${SERVICE_NAME}"
INSTALL_DIR="/data/samsung-sdi"

echo "Uninstalling Samsung SDI Victron Integration..."

# Stop service
if [ -f "${SERVICE_FILE}" ]; then
    echo "Stopping service..."
    svc -d /service/${SERVICE_NAME} 2>/dev/null || true
    sleep 2
    rm -f "${SERVICE_FILE}"
    echo "Service stopped and removed"
else
    echo "Service not found (may already be removed)"
fi

# Remove installation directory
if [ -d "${INSTALL_DIR}" ]; then
    echo "Removing installation files..."
    rm -rf "${INSTALL_DIR}"
    echo "Installation files removed"
else
    echo "Installation directory not found"
fi

# Clean up Python packages (optional)
echo "Note: Python packages (python-can, dbus-python, pygobject3) are not removed"
echo "They can be safely left installed for other applications"

echo ""
echo "Uninstallation complete!"
echo ""
echo "The following were removed:"
echo "- Service: ${SERVICE_FILE}"
echo "- Files: ${INSTALL_DIR}"
echo ""
echo "To reinstall, follow the installation guide in docs/INSTALL.md"