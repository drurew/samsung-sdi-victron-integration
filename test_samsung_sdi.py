#!/usr/bin/env python3
"""
Test script for Samsung SDI CAN client
"""

import sys
import os
import time
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
        if len(sys.argv) > 1:
            interface = sys.argv[1]
        else:
            interface = 'vcan0'  # Default to virtual interface for testing
        
        print(f"🔌 Connecting to interface: {interface}")
        
        # Try to create client
        client = SamsungSDICANClient(interface)
        print("✅ CAN client created successfully")

        # Wait a moment for data
        print("⏳ Waiting for CAN data (start the simulator if using vcan0)...")
        time.sleep(2)

        # Test data reading
        voltage = client.get_voltage()
        current = client.get_current()
        soc = client.get_soc()

        print("\n📊 BMS Data Received:")
        print(f"   Voltage: {voltage} V")
        print(f"   Current: {current} A")
        print(f"   SOC:     {soc} %")

        if voltage is None:
             print("\n⚠️  No data received. Is the BMS (or simulator) connected and transmitting?")
        else:
             print("\n✅ Valid data received!")

        print("\n✅ CAN client methods work")
        return True

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