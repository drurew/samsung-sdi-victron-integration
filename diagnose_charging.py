#!/usr/bin/env python3
"""
Samsung SDI MultiPlus Charging Diagnostic Script
Diagnoses why MultiPlus isn't charging efficiently when plugged into grid
"""

import subprocess
import sys
import time

def run_dbus_command(cmd):
    """Run a D-Bus command and return the result."""
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            return result.stdout.strip()
        else:
            return f"ERROR: {result.stderr.strip()}"
    except Exception as e:
        return f"EXCEPTION: {e}"

def check_service_value(service, path):
    """Check a D-Bus service path value."""
    cmd = f"dbus -y {service} {path} GetValue"
    return run_dbus_command(cmd)

def main():
    print("🔍 Samsung SDI MultiPlus Charging Diagnostic")
    print("=" * 50)

    # Check battery aggregator status
    print("\n📊 Battery Aggregator Status:")
    aggregator_soc = check_service_value("com.victronenergy.battery.samsung_sdi_aggregated", "/Soc")
    aggregator_charge_limit = check_service_value("com.victronenergy.battery.samsung_sdi_aggregated", "/Info/MaxChargeCurrent")
    print(f"  Aggregated SOC: {aggregator_soc}%")
    print(f"  Charge Current Limit: {aggregator_charge_limit}A")

    # Check Samsung SDI system
    print("\n🔋 Samsung SDI System Status:")
    service = "com.victronenergy.battery.samsung_sdi_system1"
    soc = check_service_value(service, "/Soc")
    charge_limit = check_service_value(service, "/Info/MaxChargeCurrent")
    voltage = check_service_value(service, "/Dc/0/Voltage")
    current = check_service_value(service, "/Dc/0/Current")
    print(f"  System 1: SOC={soc}%, MaxCharge={charge_limit}A")
    print(f"  Voltage: {voltage}V, Current: {current}A")

    # Check MultiPlus status
    print("\n⚡ MultiPlus Status:")
    multiplus_soc = check_service_value("com.victronenergy.vebus.ttyS4", "/Soc")
    multiplus_charge_current = check_service_value("com.victronenergy.vebus.ttyS4", "/Dc/0/MaxChargeCurrent")
    multiplus_ac_input = check_service_value("com.victronenergy.vebus.ttyS4", "/Ac/ActiveIn/L1/I")
    multiplus_dc_current = check_service_value("com.victronenergy.vebus.ttyS4", "/Dc/0/Current")
    print(f"  MultiPlus SOC: {multiplus_soc}")
    print(f"  MultiPlus MaxChargeCurrent: {multiplus_charge_current}A")
    print(f"  AC Input Current: {multiplus_ac_input}A")
    print(f"  DC Output Current: {multiplus_dc_current}A")

    # Check ESS settings
    print("\n🔧 ESS Configuration:")
    ess_mode = check_service_value("com.victronenergy.settings", "/Settings/Ess/Mode")
    ess_max_charge = check_service_value("com.victronenergy.settings", "/Settings/Ess/MaxChargeCurrent")
    print(f"  ESS Mode: {ess_mode}")
    print(f"  ESS Max Charge Current: {ess_max_charge}A")

    # Check DVCC settings
    print("\n🌐 DVCC Configuration:")
    dvcc_enabled = check_service_value("com.victronenergy.settings", "/Settings/SystemSetup/Dvcc")
    dvcc_max_charge = check_service_value("com.victronenergy.settings", "/Settings/SystemSetup/MaxChargeCurrent")
    print(f"  DVCC Enabled: {dvcc_enabled}")
    print(f"  DVCC Max Charge Current: {dvcc_max_charge}A")

    # Analysis
    print("\n🔍 Analysis:")
    try:
        soc_val = float(aggregator_soc) if aggregator_soc and aggregator_soc != "ERROR" else 0
        charge_limit_val = float(aggregator_charge_limit) if aggregator_charge_limit and aggregator_charge_limit != "ERROR" else 0
        ac_input_val = float(multiplus_ac_input) if multiplus_ac_input and multiplus_ac_input != "ERROR" else 0
        dc_current_val = float(multiplus_dc_current) if multiplus_dc_current and multiplus_dc_current != "ERROR" else 0

        print(f"  SOC: {soc_val:.1f}%")
        print(f"  Our charge limit: {charge_limit_val:.1f}A")
        print(f"  AC input: {ac_input_val:.1f}A")
        print(f"  DC output: {dc_current_val:.1f}A")

        # Expected performance for MultiPlus Compact 12/1200/50-16
        expected_ac_for_50a_dc = 50 * 12 / 0.9 / 120  # 50A DC * 12V / 90% efficiency / 120V AC
        print(f"  Expected AC for 50A DC: {expected_ac_for_50a_dc:.1f}A")

        if soc_val >= 99:
            print("  ⚠️  SOC ≥ 99%: Using battery requests (may be conservative, capped at 50A)")
        elif soc_val >= 95:
            print("  ⚠️  SOC 95-99%: Using 40A limit (absorption phase)")
        else:
            print("  ✅ SOC < 95%: Should allow 50A maximum (MultiPlus hardware limit)")

        if ac_input_val < 3:
            print("  ❌ AC input very low - charging not happening")
        elif ac_input_val < expected_ac_for_50a_dc * 0.6:
            print("  ⚠️  AC input below expected - charging limited")
        elif dc_current_val > 35:
            print("  ✅ DC current > 35A - charging working well")
        elif dc_current_val > 20:
            print("  ⚠️  DC current 20-35A - charging partially limited")
        else:
            print("  ❌ DC current < 20A - charging severely limited")

    except ValueError:
        print("  Could not parse numeric values for analysis")

    print("\n💡 Recommendations:")
    print("  1. MultiPlus Compact 12/1200/50-16 can only deliver 50A DC maximum")
    print("  2. If SOC ≥ 99%, batteries may request less current (normal taper charging)")
    print("  3. Check if SOC 95-99% (should allow 40A absorption charging)")
    print("  4. Verify ESS mode is set to 'Optimized with BatteryLife'")
    print("  5. Check DVCC MaxChargeCurrent is unlimited (-1)")
    print("  6. Monitor AC input current - should be ~5-6A for 50A DC charging")
    print("  7. 30-35A DC is reasonable if batteries are requesting less during taper")
    print("  8. Check MultiPlus internal charge current setting (may be limited to 30A)")

if __name__ == "__main__":
    main()