#!/usr/bin/env python3
"""
Samsung SDI BMS Integration for Victron Venus OS v1.0
Multi-battery CAN integration with intelligent aggregation

Supports multiple Samsung SDI ELPM482-00005 battery modules with:
- CAN 2.0A communication at 500kbps
- Individual module monitoring
- Intelligent SOC aggregation using battery_aggregator module
- Dynamic charge current limiting based on Samsung SDI limits
- MultiPlus compatibility

Based on Samsung SDI Product Specification ELPM482-00005 Rev 0.2
Licensed under MIT License
"""

import sys
import os
import time
import logging
import configparser
import threading
from typing import Optional, Dict, Any, List

# Setup GLib main loop before importing D-Bus
from gi.repository import GLib
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop

# Initialize D-Bus main loop FIRST
DBusGMainLoop(set_as_default=True)

# D-Bus connection helper (from Victron community example)
class SystemBus(dbus.bus.BusConnection):
    def __new__(cls):
        return dbus.bus.BusConnection.__new__(cls, dbus.bus.BusConnection.TYPE_SYSTEM)

class SessionBus(dbus.bus.BusConnection):
    def __new__(cls):
        return dbus.bus.BusConnection.__new__(cls, dbus.bus.BusConnection.TYPE_SESSION)

def dbusconnection():
    return SessionBus() if 'DBUS_SESSION_BUS_ADDRESS' in os.environ else SystemBus()

# Victron packages
try:
    sys.path.insert(1, '/opt/victronenergy/dbus-systemcalc-py/ext/velib_python')
    from vedbus import VeDbusService
except ImportError:
    print("WARNING: Running without Victron D-Bus libraries (testing mode)")
    VeDbusService = None

from samsung_sdi_can_client import SamsungSDICANClient
from battery_aggregator import BatteryAggregator

logger = logging.getLogger(__name__)


class SamsungSDIMonitor:
    """Samsung SDI battery system monitor with CAN communication"""

    def __init__(self, system_id: int, device_instance: int, config: configparser.ConfigParser,
                 sdi_client: SamsungSDICANClient):
        self.system_id = system_id
        self.device_instance = device_instance
        self.config = config
        self.sdi_client = sdi_client
        self.dbus_service: Optional[VeDbusService] = None
        self.last_update = 0
        self.last_update = 0
        self.firmware_updater = None

    def setup_dbus(self) -> bool:
        """Initialize D-Bus service for Samsung SDI system"""
        if VeDbusService is None:
            logger.warning(f"System {self.system_id}: D-Bus not available (testing mode)")
            return True

        service_prefix = self.config['Victron']['service_name_prefix']
        service_name = f"{service_prefix}_system{self.system_id}"
        product_name = self.config['Victron']['product_name']

        try:
            # Create D-Bus connection for Samsung SDI system
            self.dbus_service = VeDbusService(service_name, dbusconnection())

            # Product info
            self.dbus_service.add_path('/Mgmt/ProcessName', __file__)
            self.dbus_service.add_path('/Mgmt/ProcessVersion', '1.0.0')
            self.dbus_service.add_path('/Mgmt/Connection', f'Samsung SDI System {self.system_id}')
            self.dbus_service.add_path('/DeviceInstance', self.device_instance)
            self.dbus_service.add_path('/ProductId', 0xB005)  # Lithium battery
            self.dbus_service.add_path('/ProductName', f"{product_name} (System {self.system_id})")
            self.dbus_service.add_path('/FirmwareVersion', '1.0.0')
            self.dbus_service.add_path('/HardwareVersion', 'Samsung SDI ELPM482-00005')
            self.dbus_service.add_path('/Connected', 1)

            # Custom info
            self.dbus_service.add_path('/CustomName', f'Samsung SDI {self.system_id}', writeable=True)

            # Battery essentials
            self.dbus_service.add_path('/Dc/0/Voltage', None, writeable=False,
                                      gettextcallback=lambda p, v: f"{v:.2f}V" if v else "---")
            self.dbus_service.add_path('/Dc/0/Current', None, writeable=False,
                                      gettextcallback=lambda p, v: f"{v:.1f}A" if v else "---")
            self.dbus_service.add_path('/Dc/0/Power', None, writeable=False,
                                      gettextcallback=lambda p, v: f"{v:.0f}W" if v else "---")
            self.dbus_service.add_path('/Dc/0/Temperature', None, writeable=False,
                                      gettextcallback=lambda p, v: f"{v:.1f}°C" if v else "---")
            self.dbus_service.add_path('/Soc', None, writeable=False,
                                      gettextcallback=lambda p, v: f"{v:.0f}%" if v else "---")

            # Battery details
            capacity = float(self.config['Battery']['capacity'])
            self.dbus_service.add_path('/Capacity', capacity)
            self.dbus_service.add_path('/InstalledCapacity', capacity)
            self.dbus_service.add_path('/ConsumedAmphours', None, writeable=False)

            # Battery info
            self.dbus_service.add_path('/Info/BatteryLowVoltage', None, writeable=False)
            self.dbus_service.add_path('/Info/MaxChargeCurrent', None, writeable=False)
            self.dbus_service.add_path('/Info/MaxDischargeCurrent', None, writeable=False)

            # System info
            self.dbus_service.add_path('/System/NrOfCellsPerBattery',
                                      int(self.config['Battery']['number_of_cells']))
            self.dbus_service.add_path('/System/NrOfModulesOnline', 1)
            self.dbus_service.add_path('/System/NrOfModulesOffline', 0)
            self.dbus_service.add_path('/System/NrOfModulesBlockingCharge', 0)
            self.dbus_service.add_path('/System/NrOfModulesBlockingDischarge', 0)

            # History
            self.dbus_service.add_path('/History/ChargeCycles', None, writeable=False)
            self.dbus_service.add_path('/History/TotalAhDrawn', None, writeable=False)

            # Alarms
            self.dbus_service.add_path('/Alarms/LowVoltage', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/HighVoltage', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/LowCellVoltage', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/HighCellVoltage', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/LowSoc', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/HighChargeCurrent', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/HighDischargeCurrent', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/CellImbalance', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/InternalFailure', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/HighChargeTemperature', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/LowChargeTemperature', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/HighTemperature', 0, writeable=False)
            self.dbus_service.add_path('/Alarms/LowTemperature', 0, writeable=False)

            logger.info(f"Node {self.node_id}: D-Bus service initialized ({service_name})")
            return True

        except Exception as e:
            logger.error(f"Node {self.node_id}: Failed to initialize D-Bus service: {e}")
            return False

    def update(self) -> bool:
        """Update Samsung SDI system data"""
        try:
            # Read system data from CAN bus
            voltage = self.sdi_client.get_voltage()
            current = self.sdi_client.get_current()
            soc = self.sdi_client.get_soc()
            temperature = self.sdi_client.get_temperature()

            if voltage is None or current is None or soc is None:
                logger.warning(f"System {self.system_id}: Incomplete data received")
                return False

            # Create data dict for D-Bus update
            system_data = {
                'voltage': voltage,
                'current': current,
                'soc': soc,
                'temperature': temperature
            }

            # Update D-Bus
            if self.dbus_service:
                self._update_dbus(system_data)

            self.last_update = time.time()
            return True

        except Exception as e:
            logger.error(f"System {self.system_id}: Update error: {e}")
            return False

    def _update_dbus(self, system_data: Dict[str, Any]):
        """Update D-Bus paths with Samsung SDI system data"""
        try:
            # Essential battery data
            if 'voltage' in system_data:
                self.dbus_service['/Dc/0/Voltage'] = system_data['voltage']

            if 'current' in system_data:
                # Samsung SDI current: positive=charge, negative=discharge
                current = system_data['current']
                self.dbus_service['/Dc/0/Current'] = current

                if 'voltage' in system_data:
                    power = system_data['voltage'] * current
                    self.dbus_service['/Dc/0/Power'] = power

            if 'temperature' in system_data:
                self.dbus_service['/Dc/0/Temperature'] = system_data['temperature']

            if 'soc' in system_data:
                self.dbus_service['/Soc'] = system_data['soc']

                capacity = float(self.config['Battery']['capacity'])
                consumed = capacity * (100 - system_data['soc']) / 100
                self.dbus_service['/ConsumedAmphours'] = consumed

            if 'cycles' in bms_data:
                self.dbus_service['/History/ChargeCycles'] = int(bms_data['cycles'])

            if 'ah_since_eq' in bms_data:
                self.dbus_service['/History/TotalAhDrawn'] = bms_data['ah_since_eq']

            # Set static values
            self.dbus_service['/Info/BatteryLowVoltage'] = float(self.config['Battery']['battery_low_voltage'])
            self.dbus_service['/Info/MaxChargeCurrent'] = float(self.config['Battery']['max_charge_current'])
            self.dbus_service['/Info/MaxDischargeCurrent'] = float(self.config['Battery']['max_discharge_current'])

            logger.debug(f"Node {self.node_id}: V={bms_data.get('voltage', 0):.2f}V, "
                        f"I={bms_data.get('current', 0):.2f}A, "
                        f"SOC={bms_data.get('soc', 0):.1f}%")

        except Exception as e:
            logger.error(f"Node {self.node_id}: D-Bus update error: {e}")


class SamsungSDIAggregatorService:
    """Combined Samsung SDI BMS service with intelligent aggregation for ESS compatibility"""

    def __init__(self, config_file: str = '/data/samsung-sdi-bms/config.ini'):
        self.config = self.load_config(config_file)
        self.sdi_client: Optional[SamsungSDICANClient] = None
        self.systems: List[SamsungSDIMonitor] = []
        self.battery_aggregator: Optional[BatteryAggregator] = None
        self.running = False

        # MultiPlus compatibility settings
        self.multiplus_min_power = float(self.config['Aggregation']['multiplus_min_power'])  # 1200W minimum discharge

    def load_config(self, config_file: str) -> configparser.ConfigParser:
        """Load configuration with defaults"""
        config = configparser.ConfigParser()

        # CAN settings - Samsung SDI uses 500kbps
        config['CAN'] = {
            'interface': 'can0',
            'bitrate': '500000',
            'system_ids': '1'  # Single system ID for Samsung SDI
        }

        # Victron settings
        config['Victron'] = {
            'service_name_prefix': 'com.victronenergy.battery.samsung_sdi',
            'device_instance_start': '280',
            'product_name': 'Samsung SDI ELPM482-00005',
            'update_interval': '1.0'
        }

        # Battery settings - Samsung SDI 4.84kWh module
        config['Battery'] = {
            'capacity': '4840.0',  # 4.84kWh
            'number_of_cells': '14',  # Based on spec
            'battery_low_voltage': '40.0',  # Approximate for 14S system
            'max_charge_current': '50.0',  # Samsung SDI typical limit
            'max_discharge_current': '50.0'
        }

        # Aggregation settings (now handled by battery_aggregator module)
        config['Aggregation'] = {
            'enabled': 'true',
            'multiplus_min_power': '1200.0'  # 1200W minimum discharge for MultiPlus compatibility
        }

        # Logging settings
        config['Logging'] = {
            'update_interval': '2.0',
            'debug': 'false'
        }

        # Load config file if it exists
        if os.path.exists(config_file):
            config.read(config_file)
            logger.info(f"Loaded configuration from {config_file}")
        else:
            logger.info(f"Config file {config_file} not found, using defaults")

        return config

    def setup_sdi(self) -> bool:
        """Initialize Samsung SDI CAN client"""
        can_interface = self.config['CAN']['interface']

        try:
            self.sdi_client = SamsungSDICANClient(can_interface)
            logger.info(f"Initialized Samsung SDI CAN client on {can_interface}")
        except Exception as e:
            logger.error(f"Failed to initialize Samsung SDI CAN client: {e}")
            return False

        # Determine system IDs
        system_ids_config = self.config['CAN']['system_ids'].strip()
        system_ids = [int(x.strip()) for x in system_ids_config.split(',')]

        logger.info(f"Using Samsung SDI system IDs: {system_ids}")

        # Create system monitors
        device_instance = int(self.config['Victron']['device_instance_start'])

        for system_id in system_ids:
            system = SamsungSDIMonitor(system_id, device_instance, self.config, self.sdi_client)

            if system.setup_dbus():
                self.systems.append(system)
                logger.info(f"Initialized Samsung SDI system monitor for system {system_id} (device instance {device_instance})")
                device_instance += 1
            else:
                logger.error(f"Failed to initialize Samsung SDI system monitor for system {system_id}")

        if not self.systems:
            logger.error("No Samsung SDI system monitors initialized")
            return False

        # Setup battery aggregator module
        return self.setup_battery_aggregator()

    def setup_battery_aggregator(self) -> bool:
        """Setup the battery aggregator module for ESS compatibility"""
        try:
            # Initialize the battery aggregator with MultiPlus compatibility
            aggregator_service_name = f"com.victronenergy.battery.superb_aggregated"
            self.battery_aggregator = BatteryAggregator(aggregator_service_name)

            # Configure MultiPlus minimum discharge power
            self.battery_aggregator.multiplus_min_power = self.multiplus_min_power

            logger.info(f"Battery aggregator module initialized with MultiPlus {self.multiplus_min_power}W minimum discharge")
            return True

        except Exception as e:
            logger.error(f"Failed to initialize battery aggregator module: {e}")
            return False

    def update_aggregator(self) -> bool:
        """Update battery aggregator module with current Samsung SDI system data"""
        try:
            if not self.battery_aggregator:
                return True

            # Collect data from Samsung SDI CAN client
            voltage = self.sdi_client.get_voltage()
            current = self.sdi_client.get_current()
            soc = self.sdi_client.get_soc()
            temperature = self.sdi_client.get_temperature()
            charge_current_limit = self.sdi_client.get_charge_current_limit()

            if voltage is not None and soc is not None:
                system_data = {
                    'voltage': voltage,
                    'current': current or 0.0,
                    'soc': soc,
                    'capacity': float(self.config['Battery']['capacity']),
                    'temperature': temperature,
                    'charge_current_requested': charge_current_limit,  # Samsung SDI provides limits
                    'max_charge_current': charge_current_limit
                }

                # Update aggregator with Samsung SDI system data
                service_name = f"com.victronenergy.battery.samsung_sdi_system{self.systems[0].system_id}"
                self.battery_aggregator.add_battery(service_name, system_data)

            # The battery_aggregator module handles the D-Bus publishing internally
            return True

        except Exception as e:
            logger.error(f"Aggregator update error: {e}")
            return True

    def get_battery_value(self, service, path, default=None):
        """Read a value from a battery D-Bus service"""
        if service is None:
            return default
        try:
            return service[path]
        except:
            return default

    def update_callback(self):
        """Periodic update callback"""
        try:
            # Update individual batteries
            for battery in self.batteries:
                battery.update()

            # Update aggregator
            self.update_aggregator()

            return True  # Continue timer

        except Exception as e:
            logger.error(f"Update callback error: {e}", exc_info=True)
            return True

    def run(self):
        """Main service loop"""
        logger.info(f"Starting Samsung SDI BMS Integration v1.0.0 ({len(self.systems)} systems + battery aggregator module)")

        if not self.setup_sdi():
            logger.error("Samsung SDI setup failed")
            return False

        update_interval = float(self.config['Logging']['update_interval']) * 1000  # Convert to ms
        self.running = True

        logger.info(f"Service running (update interval: {update_interval}ms)")

        # Setup GLib main loop
        mainloop = GLib.MainLoop()

        # Register periodic update callback
        GLib.timeout_add(int(update_interval), self.update_callback)

        try:
            mainloop.run()
        except KeyboardInterrupt:
            logger.info("Service interrupted by user")
        except Exception as e:
            logger.error(f"Service error: {e}", exc_info=True)
        finally:
            self.cleanup()

        return True

    def cleanup(self):
        """Cleanup resources"""
        logger.info("Cleaning up...")
        self.running = False

        if self.canopen_client:
            self.canopen_client.disconnect()


def main():
    """Main entry point"""
    import argparse
    parser = argparse.ArgumentParser(description='SuperB BMS Integration for Victron Venus OS')
    parser.add_argument('--interface', default='vecan0', help='CAN interface name')
    parser.add_argument('--bitrate', type=int, default=250000, help='CAN bitrate')
    parser.add_argument('--log-file', default='/var/log/superb-bms.log', help='Log file path')
    parser.add_argument('config', nargs='?', default='/data/superb-bms/config.ini', help='Config file path')
    args = argparse.parse_args()

    # Setup logging
    log_handlers = [logging.StreamHandler()]
    try:
        log_handlers.append(logging.FileHandler(args.log_file))
    except PermissionError:
        print(f"Warning: Cannot write to {args.log_file}, logging to console only")

    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
        handlers=log_handlers
    )

    config_file = args.config

    service = SamsungSDIAggregatorService(config_file)
    service.run()


if __name__ == '__main__':
    main()