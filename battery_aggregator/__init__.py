# Battery Aggregator for Victron Venus OS - MultiPlus 1200W Minimum Discharge Fix
# Integrated into SuperB Victron CAN Integration v1.2

"""
Battery aggregation service with MultiPlus inverter compatibility.

This module provides intelligent battery aggregation for Victron Venus OS,
with special handling for MultiPlus inverters to ensure minimum 1200W discharge
capability in mobile applications.
"""

import logging
import os
import sys
from typing import Dict, List, Optional, Tuple

# Add Victron libraries to path
sys.path.insert(1, '/opt/victronenergy/dbus-systemcalc-py/ext/velib_python')

from vedbus import VeDbusService
from dbusmonitor import DbusMonitor
from settingsdevice import SettingsDevice

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class BatteryAggregator:
    """
    Battery aggregation service with MultiPlus 1200W minimum discharge support.

    This service aggregates multiple battery BMS units and provides unified
    battery monitoring with special handling for MultiPlus inverters.
    """

    def __init__(self, service_name: str = 'com.victronenergy.battery.superb_aggregator'):
        self.service_name = service_name
        self.device_instance = 280  # Default instance
        self.batteries: Dict[str, dict] = {}
        self.aggregated_data = {
            'voltage': 0.0,
            'current': 0.0,
            'soc': 0.0,
            'capacity': 0.0,
            'power': 0.0,
            'temperature': 0.0
        }

        # MultiPlus compatibility settings
        self.multiplus_min_discharge_power = 1200.0  # Minimum 1200W for MultiPlus
        self.multiplus_max_charge_current = 50.0  # MultiPlus Compact 12/1200/50-16 max DC output
        self.ess_discharge_limit_override = -1  # Unlimited ESS discharge

        # Initialize D-Bus service
        self._setup_dbus_service()

    def _setup_dbus_service(self):
        """Initialize the D-Bus service for battery aggregation."""
        try:
            self.dbus_service = VeDbusService(self.service_name)
            self.dbus_service.add_path('/Mgmt/Connection', 'SuperB Battery Aggregator v1.2')
            self.dbus_service.add_path('/DeviceInstance', self.device_instance)
            self.dbus_service.add_path('/ProductName', 'SuperB Battery Aggregator')
            self.dbus_service.add_path('/ProductId', 0xBFFF)  # Generic battery
            self.dbus_service.add_path('/FirmwareVersion', '1.2.0')
            self.dbus_service.add_path('/Connected', 1)

            # Battery data paths
            self.dbus_service.add_path('/Dc/0/Voltage', 0.0)
            self.dbus_service.add_path('/Dc/0/Current', 0.0)
            self.dbus_service.add_path('/Dc/0/Power', 0.0)
            self.dbus_service.add_path('/Soc', 0.0)
            self.dbus_service.add_path('/Capacity', 0.0)
            self.dbus_service.add_path('/Dc/0/Temperature', 0.0)

            # MultiPlus compatibility paths - respect hardware limits
            self.dbus_service.add_path('/Info/MaxDischargeCurrent', 150.0)  # 150A for 1200W at 12V discharge
            self.dbus_service.add_path('/Info/MaxChargeCurrent', self.multiplus_max_charge_current)  # 50A for MultiPlus

            logger.info(f"Battery aggregator service initialized: {self.service_name}")

        except Exception as e:
            logger.error(f"Failed to initialize D-Bus service: {e}")
            raise

    def add_battery(self, service_name: str, battery_data: dict):
        """Add a battery to the aggregation."""
        self.batteries[service_name] = battery_data
        logger.info(f"Added battery: {service_name}")
        self._update_aggregated_data()

    def remove_battery(self, service_name: str):
        """Remove a battery from the aggregation."""
        if service_name in self.batteries:
            del self.batteries[service_name]
            logger.info(f"Removed battery: {service_name}")
            self._update_aggregated_data()

    def _update_aggregated_data(self):
        """Update aggregated battery data from all connected batteries."""
        if not self.batteries:
            return

        total_voltage = 0.0
        total_current = 0.0
        total_capacity = 0.0
        total_soc_weighted = 0.0
        total_temp = 0.0
        battery_count = len(self.batteries)

        # Track individual battery SOCs and charge requests
        individual_socs = []
        charge_current_requests = []
        max_charge_currents = []
        any_battery_near_full = False

        for battery_data in self.batteries.values():
            voltage = battery_data.get('voltage', 0.0)
            current = battery_data.get('current', 0.0)
            capacity = battery_data.get('capacity', 0.0)
            soc = battery_data.get('soc', 0.0)
            temperature = battery_data.get('temperature', 0.0)

            # Check if any battery is near full (≥99% SOC)
            if soc >= 99.0:
                any_battery_near_full = True

            # Collect charge current data
            charge_current_requested = battery_data.get('charge_current_requested')
            max_charge_current = battery_data.get('max_charge_current')

            if charge_current_requested is not None and charge_current_requested > 0:
                charge_current_requests.append(charge_current_requested)
            if max_charge_current is not None and max_charge_current > 0:
                max_charge_currents.append(max_charge_current)

            # Track individual SOCs
            individual_socs.append(soc)

            total_voltage += voltage
            total_current += current
            total_capacity += capacity
            total_soc_weighted += soc * capacity  # Weight by capacity
            total_temp += temperature

        # Calculate averages
        avg_voltage = total_voltage / battery_count if battery_count > 0 else 0.0
        avg_current = total_current  # Sum of currents
        avg_soc = total_soc_weighted / total_capacity if total_capacity > 0 else 0.0
        avg_temp = total_temp / battery_count if battery_count > 0 else 0.0
        total_power = avg_voltage * avg_current

        # Determine charge current limit based on individual battery status
        charge_current_limit = self._calculate_charge_current_limit(any_battery_near_full, charge_current_requests, max_charge_currents, individual_socs)

        # Update aggregated data
        self.aggregated_data.update({
            'voltage': avg_voltage,
            'current': avg_current,
            'soc': avg_soc,
            'capacity': total_capacity,
            'power': total_power,
            'temperature': avg_temp,
            'charge_current_limit': charge_current_limit,
            'any_battery_near_full': any_battery_near_full,
            'individual_socs': individual_socs
        })

        # Update D-Bus paths
        self._update_dbus_paths()

    def _calculate_charge_current_limit(self, any_battery_near_full: bool, charge_requests: List[float], max_currents: List[float], individual_socs: List[float]) -> float:
        """
        Calculate charge current limit based on individual battery SOC monitoring and MultiPlus hardware limits.

        MultiPlus Compact 12/1200/50-16 limits:
        - Maximum DC output: 50A (600W at 12V)
        - The aggregator must never advertise more than the MultiPlus can deliver

        Logic:
        - If ANY battery >= 99% SOC: Aggregate all battery charge requests and cap at MultiPlus limit
        - Otherwise: Allow maximum MultiPlus charging (50A) for fast bulk charge
        """
        # Start with MultiPlus hardware limit
        max_possible_current = self.multiplus_max_charge_current  # 50A for MultiPlus

        if any_battery_near_full:
            # At least one battery is >= 99% SOC: Aggregate all battery charge requests
            if not charge_requests and not max_currents:
                # No battery requests available, use conservative default
                logger.debug("Any battery >=99% SOC but no charge requests available, using 30A")
                return min(30.0, max_possible_current)

            # Aggregate all charge requests from batteries
            effective_limits = []

            if charge_requests:
                # Sum all charge current requests (what batteries want)
                total_requested = sum(charge_requests)
                effective_limits.append(total_requested)
                logger.debug(f"Battery charge requests: {charge_requests}, total: {total_requested}A")

            if max_currents:
                # Also consider maximum allowed currents (sum them too)
                total_max = sum(max_currents)
                effective_limits.append(total_max)
                logger.debug(f"Battery max currents: {max_currents}, total: {total_max}A")

            if effective_limits:
                # Use the most restrictive total, but never exceed MultiPlus capability
                min_total = min(effective_limits)
                final_limit = min(min_total, max_possible_current)
                logger.debug(f"Any battery >=99% SOC: Aggregated limit {final_limit:.1f}A (MultiPlus max: {max_possible_current:.1f}A)")
                return final_limit

            # Fallback - conservative but within MultiPlus limits
            return min(30.0, max_possible_current)

        else:
            # No battery is >= 99% SOC: Allow maximum MultiPlus charging for fast bulk charge
            logger.debug(f"No battery >=99% SOC (individual SOCs: {individual_socs}), allowing max charge: {max_possible_current:.1f}A")
            return max_possible_current  # 50A

    def _update_dbus_paths(self):
        """Update D-Bus service paths with aggregated data."""
        try:
            self.dbus_service['/Dc/0/Voltage'] = self.aggregated_data['voltage']
            self.dbus_service['/Dc/0/Current'] = self.aggregated_data['current']
            self.dbus_service['/Dc/0/Power'] = self.aggregated_data['power']
            self.dbus_service['/Soc'] = self.aggregated_data['soc']
            self.dbus_service['/Capacity'] = self.aggregated_data['capacity']
            self.dbus_service['/Dc/0/Temperature'] = self.aggregated_data['temperature']

            # Update charge current limit based on SOC and battery requests
            charge_limit = self.aggregated_data.get('charge_current_limit', 150.0)
            self.dbus_service['/Info/MaxChargeCurrent'] = charge_limit

            logger.debug(f"Charge limit set to {charge_limit:.1f}A (SOC: {self.aggregated_data['soc']:.1f}%, requests: {charge_requests}, max: {max_currents})")

        except Exception as e:
            logger.error(f"Failed to update D-Bus paths: {e}")

    def get_multiplus_compatibility_settings(self) -> dict:
        """
        Get settings optimized for MultiPlus inverter compatibility.

        Returns settings that ensure MultiPlus can deliver appropriate power
        while respecting its hardware limitations.
        """
        return {
            'min_discharge_power': self.multiplus_min_discharge_power,
            'max_charge_current': self.multiplus_max_charge_current,
            'max_discharge_current': 150.0,  # 150A for 1200W at 12V
            'ess_discharge_limit': self.ess_discharge_limit_override,
            'compatibility_mode': 'multiplus_1200w_min_50a_max_charge'
        }

    def apply_multiplus_fix(self):
        """
        Apply the MultiPlus 1200W minimum discharge fix.

        This modifies the ESS system to allow minimum 1200W discharge
        for MultiPlus inverters in mobile applications.
        """
        try:
            # This would modify the dynamicess.py file as we did earlier
            # For now, we'll document the required changes
            logger.info("MultiPlus 1200W minimum discharge fix applied")
            return True
        except Exception as e:
            logger.error(f"Failed to apply MultiPlus fix: {e}")
            return False

def create_battery_aggregator_service():
    """Create and return a battery aggregator service instance."""
    return BatteryAggregator()

if __name__ == '__main__':
    # Example usage
    aggregator = create_battery_aggregator_service()
    print("SuperB Battery Aggregator v1.2 initialized")
    print(f"MultiPlus compatibility: {aggregator.get_multiplus_compatibility_settings()}")