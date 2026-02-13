#!/usr/bin/env python3
"""
Test script for Samsung SDI CAN client
"""

import sys
import os
sys.path.append(os.path.dirname(__file__))

try:
    from samsung_sdi_can_client import SamsungSDICANClient
    print("✅ Samsung SDI CAN client imported successfully")
except ImportError as e:
    print(f"❌ Failed to import Samsung SDI CAN client: {e}")
    sys.exit(1)

def test_can_client():
    """Test the Samsung SDI CAN client"""
    print("\n🧪 Testing Samsung SDI CAN Client")
    print("=" * 40)

    try:
        # Try to create client (will fail without CAN hardware)
        client = SamsungSDICANClient('can0')
        print("✅ CAN client created successfully")

        # Test data reading (will return None without hardware)
        voltage = client.get_voltage()
        current = client.get_current()
        soc = client.get_soc()

        print(f"Voltage: {voltage}")
        print(f"Current: {current}")
        print(f"SOC: {soc}")

        print("✅ CAN client methods work (no hardware connected)")

    except Exception as e:
        print(f"❌ CAN client test failed: {e}")
        return False

    return True

if __name__ == "__main__":
    success = test_can_client()
    if success:
        print("\n🎉 Samsung SDI integration test completed successfully!")
        print("\nNext steps:")
        print("1. Connect Samsung SDI module to CAN bus")
        print("2. Configure CAN interface: ip link set can0 up type can bitrate 500000")
        print("3. Run: python3 samsung_sdi_bms_service.py")
    else:
        print("\n❌ Test failed - check dependencies and configuration")
        sys.exit(1)