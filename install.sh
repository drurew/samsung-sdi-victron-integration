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

APP_NAME="samsung-sdi"
INSTALL_DIR="/data/${APP_NAME}"
SERVICE_NAME="samsung-sdi-bms"
SERVICE_LINK="/service/${SERVICE_NAME}"
SERVICE_TARGET="${INSTALL_DIR}/service"

echo "Installing to ${INSTALL_DIR}..."

# 1. Prepare Installation Directory
mkdir -p "${INSTALL_DIR}"

# Resolve absolute paths to check if we are installing from the target directory
CURRENT_DIR=$(readlink -f "$PWD")
TARGET_DIR=$(readlink -f "${INSTALL_DIR}")

if [ "$CURRENT_DIR" != "$TARGET_DIR" ]; then
    echo "Copying files from $CURRENT_DIR to $TARGET_DIR..."
    cp -r ./* "${INSTALL_DIR}/"
else
    echo "Running from installation directory. Skipping copy."
fi

chmod +x "${INSTALL_DIR}/"*.py
chmod +x "${INSTALL_DIR}/"*.sh

# 2. Dependency Management
echo "Checking dependencies..."

# Function to check python module
check_module() {
    python3 -c "import $1" > /dev/null 2>&1
}

# Venus OS Mod Registration Standard
register_mod() {
    local MOD_ID=$1
    local MOD_NAME=$2
    local MOD_VERSION=$3
    local MOD_REPO=$4
    local MOD_FILE=$5
    
    # Proposed standard location in persistent storage
    local MANIFEST_DIR="/data/etc/venus-mods"
    mkdir -p "$MANIFEST_DIR"
    
    # Calculate hash to verify file integrity later
    local HASH="none"
    if [ -f "$MOD_FILE" ]; then
        HASH=$(md5sum "$MOD_FILE" | awk '{print $1}')
    fi
    
    local TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    
    # Write JSON Manifest
    cat > "$MANIFEST_DIR/${MOD_ID}.json" <<EOF
{
  "id": "${MOD_ID}",
  "name": "${MOD_NAME}",
  "version": "${MOD_VERSION}",
  "repository": "${MOD_REPO}",
  "installed_at": "${TIMESTAMP}",
  "integrity_check": {
    "file": "${MOD_FILE}",
    "md5": "${HASH}"
  }
}
EOF
    echo "Module '${MOD_ID}' registered to Venus OS manifest."
}

# Try to install pip if missing
if ! command -v pip3 &> /dev/null; then
    echo "pip3 not found. Attempting to install pip..."
    if command -v opkg &> /dev/null; then
        opkg update && opkg install python3-pip
    else
        echo "WARNING: opkg not found. Could not install pip. Please install 'python3-pip' manually."
    fi
fi

# Install Required Packages
echo "Installing dependencies..."

# Method 1: Try opkg (Venus OS Native)
if command -v opkg &> /dev/null; then
    echo "Attempting to install dependencies via opkg..."
    # Update not always necessary/possible online, but good to try if package missing
    # We don't limit to specific versions
    opkg update || true
    opkg install python3-can python3-dbus python3-pygobject || echo "opkg install failed/incomplete. Falling back to pip..."
fi

# Method 2: pip (Universal)
if command -v pip3 &> /dev/null; then
    echo "Checking/Installing Python packages via pip..."
    # Upgrade pip to ensure wheel support etc.
    pip3 install --upgrade pip || true
    # Use PyGObject instead of pygobject3
    pip3 install python-can dbus-python PyGObject || echo "Warning: Pip install failed. This is expected if packages are system-managed."
else
    echo "WARNING: pip3 not available. Skipping pip install."
fi

# Verification
MISSING_DEPS=0
if ! check_module "can"; then echo "ERROR: Missing 'python-can'"; MISSING_DEPS=1; fi
if ! check_module "dbus"; then echo "ERROR: Missing 'dbus-python'"; MISSING_DEPS=1; fi
# Check for gi.repository
if ! python3 -c "import gi" > /dev/null 2>&1; then echo "ERROR: Missing 'PyGObject' (gi)"; MISSING_DEPS=1; fi

if [ $MISSING_DEPS -eq 1 ]; then
    echo "---------------------------------------------------"
    echo "CRITICAL: Missing Python dependencies."
    echo "The service will NOT start without these packages."
    echo "Try: opkg install python3-dbus python3-pygobject python3-can"
    echo "Or via pip: pip3 install python-can dbus-python PyGObject"
    echo "---------------------------------------------------"
    exit 1
fi

# 3. Service Configuration
echo "Configuring service..."

# Remove existing service if it exists (symlink or file)
if [ -e "${SERVICE_LINK}" ]; then
    echo "Removing existing service link..."
    rm -rf "${SERVICE_LINK}"
fi

# Create service directory structure in INSTALL_DIR
mkdir -p "${SERVICE_TARGET}"
mkdir -p "${SERVICE_TARGET}/log"

# Create Run Script
cat > "${SERVICE_TARGET}/run" << EOF
#!/bin/sh
exec 2>&1
cd ${INSTALL_DIR}
exec python3 samsung_sdi_bms_service.py
EOF
chmod +x "${SERVICE_TARGET}/run"

# Create Log Run Script
cat > "${SERVICE_TARGET}/log/run" << EOF
#!/bin/sh
exec multilog t s25000 n4 ${INSTALL_DIR}/log
EOF
chmod +x "${SERVICE_TARGET}/log/run"

# 4. Enable Service
echo "Enabling service..."
ln -s "${SERVICE_TARGET}" "${SERVICE_LINK}"

# 5. CAN Interface Config (Optional)
if [ ! -f "/etc/venus/can.conf" ]; then
    echo "Creating default CAN config..."
    mkdir -p /etc/venus
    echo "interface=can0" > /etc/venus/can.conf
    echo "bitrate=250000" >> /etc/venus/can.conf
fi

# 6. Wait for service to start
echo "Waiting for service to start..."
sleep 2
svstat "${SERVICE_LINK}"

# 7. Register Mod
register_mod "samsung-sdi-driver" "Samsung SDI Integration" "v1.1.0" "https://github.com/drurew/samsung-sdi-victron-integration" "${INSTALL_DIR}/samsung_sdi_bms_service.py"

echo "---------------------------------------------------"
echo "Installation Complete!"
echo "path: ${INSTALL_DIR}"
echo "---------------------------------------------------"