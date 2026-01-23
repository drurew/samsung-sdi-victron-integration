#!/usr/bin/env python3
"""
Samsung SDI BMS CAN Client
Reads CAN messages from Samsung SDI battery modules
Based on Product Specification ELPM482-00005 Rev 0.2
"""

import can
import struct
import time
import logging
from typing import Optional, Tuple, Dict, Any, List
from dataclasses import dataclass
from threading import Lock

logger = logging.getLogger(__name__)


@dataclass
class CANMessageDefinition:
    """CAN message definition for Samsung SDI BMS"""
    can_id: int
    name: str
    fields: Dict[str, Dict[str, Any]]  # field_name -> {byte_offset, bit_offset, data_type, scale, unit, description}


class SamsungSDICANClient:
    """
    Samsung SDI BMS CAN client
    Communicates with Samsung SDI battery modules via CAN 2.0A at 500kbps
    """

    # CAN message definitions based on Samsung SDI specification
    CAN_MESSAGES = {
        0x500: CANMessageDefinition(
            can_id=0x500,
            name="System Status",
            fields={
                'system_voltage': {'byte_offset': 0, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.01, 'unit': 'V', 'description': 'Average tray voltage in normal trays'},
                'system_current': {'byte_offset': 2, 'bit_offset': None, 'data_type': 'S16', 'scale': 1.0, 'unit': 'A', 'description': 'Total current in all tray'},
                'system_soc': {'byte_offset': 4, 'bit_offset': None, 'data_type': 'U8', 'scale': 1.0, 'unit': '%', 'description': 'Average SOC in all tray'},
                'system_soh': {'byte_offset': 5, 'bit_offset': None, 'data_type': 'U8', 'scale': 1.0, 'unit': '%', 'description': 'Average SOH in all tray'},
                'system_heartbeat': {'byte_offset': 6, 'bit_offset': None, 'data_type': 'U16', 'scale': 1.0, 'unit': 'dec', 'description': 'Heart-Beat Value'},
                'alarm_status': {'byte_offset': 0, 'bit_offset': 0, 'data_type': 'BITFIELD8', 'scale': 1.0, 'unit': '', 'description': 'System Alarm Status'},
                'protection_status': {'byte_offset': 2, 'bit_offset': 0, 'data_type': 'BITFIELD8', 'scale': 1.0, 'unit': '', 'description': 'System Protection Status'},
            }
        ),
        0x501: CANMessageDefinition(
            can_id=0x501,
            name="System Configuration",
            fields={
                'total_trays': {'byte_offset': 4, 'bit_offset': None, 'data_type': 'U8', 'scale': 1.0, 'unit': 'EA', 'description': 'Number of Total Tray'},
                'normal_trays': {'byte_offset': 5, 'bit_offset': None, 'data_type': 'U8', 'scale': 1.0, 'unit': 'EA', 'description': 'Number of Normal Operating Tray'},
                'fault_trays': {'byte_offset': 6, 'bit_offset': None, 'data_type': 'U8', 'scale': 1.0, 'unit': 'EA', 'description': 'Number of Fault Tray'},
            }
        ),
        0x502: CANMessageDefinition(
            can_id=0x502,
            name="Charge/Discharge Limits",
            fields={
                'battery_charge_voltage': {'byte_offset': 0, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.1, 'unit': 'V', 'description': 'Set point for battery charge voltage'},
                'charge_current_limit': {'byte_offset': 2, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.1, 'unit': 'A', 'description': 'DC charge current limitation'},
                'discharge_current_limit': {'byte_offset': 4, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.1, 'unit': 'A', 'description': 'DC discharge current limitation'},
                'battery_discharge_voltage': {'byte_offset': 6, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.1, 'unit': 'V', 'description': 'Voltage discharge limit'},
            }
        ),
        0x503: CANMessageDefinition(
            can_id=0x503,
            name="Cell Voltage Summary",
            fields={
                'avg_cell_voltage': {'byte_offset': 0, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.001, 'unit': 'V', 'description': 'Average cell voltage in all tray'},
                'max_cell_voltage': {'byte_offset': 2, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.001, 'unit': 'V', 'description': 'Maximum cell voltage in all tray'},
                'min_cell_voltage': {'byte_offset': 4, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.001, 'unit': 'V', 'description': 'Minimum cell voltage in all tray'},
                'avg_tray_voltage': {'byte_offset': 0, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.01, 'unit': 'V', 'description': 'Average tray voltage in all tray'},
                'max_tray_voltage': {'byte_offset': 2, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.01, 'unit': 'V', 'description': 'Maximum tray voltage in all tray'},
                'min_tray_voltage': {'byte_offset': 4, 'bit_offset': None, 'data_type': 'U16', 'scale': 0.01, 'unit': 'V', 'description': 'Minimum tray voltage in all tray'},
            }
        ),
        0x504: CANMessageDefinition(
            can_id=0x504,
            name="Temperature Summary",
            fields={
                'avg_cell_temp': {'byte_offset': 4, 'bit_offset': None, 'data_type': 'S8', 'scale': 1.0, 'unit': '°C', 'description': 'Average cell temperature in all tray'},
                'max_cell_temp': {'byte_offset': 5, 'bit_offset': None, 'data_type': 'S8', 'scale': 1.0, 'unit': '°C', 'description': 'Maximum cell temperature in all tray'},
                'min_cell_temp': {'byte_offset': 6, 'bit_offset': None, 'data_type': 'S8', 'scale': 1.0, 'unit': '°C', 'description': 'Minimum cell temperature in all tray'},
            }
        ),
    }

    def __init__(self, can_interface: str = 'vecan0', timeout: float = 1.0):
        self.can_interface = can_interface
        self.timeout = timeout
        self.bus: Optional[can.Bus] = None
        self.data_lock = Lock()
        self.last_update = 0
        self.battery_data = {}

        # Initialize CAN bus
        try:
            self.bus = can.Bus(interface='socketcan', channel=can_interface, bitrate=500000)
            logger.info(f"Connected to CAN bus {can_interface} at 500kbps")
        except Exception as e:
            logger.error(f"Failed to connect to CAN bus {can_interface}: {e}")
            raise

    def __del__(self):
        if self.bus:
            self.bus.shutdown()

    def _parse_can_message(self, message: can.Message) -> Dict[str, Any]:
        """Parse a CAN message according to Samsung SDI specification"""
        if message.arbitration_id not in self.CAN_MESSAGES:
            return {}

        msg_def = self.CAN_MESSAGES[message.arbitration_id]
        data = message.data
        parsed_data = {}

        for field_name, field_info in msg_def.fields.items():
            try:
                byte_offset = field_info['byte_offset']
                data_type = field_info['data_type']
                scale = field_info['scale']

                if data_type == 'U8':
                    raw_value = data[byte_offset]
                elif data_type == 'S8':
                    raw_value = struct.unpack('<b', bytes([data[byte_offset]]))[0]
                elif data_type == 'U16':
                    raw_value = struct.unpack('<H', data[byte_offset:byte_offset+2])[0]
                elif data_type == 'S16':
                    raw_value = struct.unpack('<h', data[byte_offset:byte_offset+2])[0]
                elif data_type == 'BITFIELD8':
                    raw_value = data[byte_offset]
                else:
                    continue

                parsed_value = raw_value * scale
                parsed_data[field_name] = parsed_value

            except (IndexError, struct.error) as e:
                logger.warning(f"Failed to parse field {field_name} in message 0x{message.arbitration_id:03X}: {e}")
                continue

        return parsed_data

    def read_battery_data(self) -> Dict[str, Any]:
        """Read battery data from CAN bus"""
        with self.data_lock:
            current_time = time.time()

            # Only update if it's been more than 100ms since last update
            if current_time - self.last_update < 0.1:
                return self.battery_data.copy()

            try:
                # Read messages with timeout
                messages = []
                start_time = time.time()

                while time.time() - start_time < self.timeout:
                    msg = self.bus.recv(timeout=0.01)
                    if msg:
                        messages.append(msg)
                    else:
                        break

                # Parse messages
                for msg in messages:
                    parsed = self._parse_can_message(msg)
                    if parsed:
                        self.battery_data.update(parsed)

                self.last_update = current_time

            except Exception as e:
                logger.error(f"Error reading CAN messages: {e}")

            return self.battery_data.copy()

    def get_voltage(self) -> Optional[float]:
        """Get system voltage"""
        data = self.read_battery_data()
        return data.get('system_voltage')

    def get_current(self) -> Optional[float]:
        """Get system current"""
        data = self.read_battery_data()
        return data.get('system_current')

    def get_soc(self) -> Optional[float]:
        """Get system SOC"""
        data = self.read_battery_data()
        return data.get('system_soc')

    def get_soh(self) -> Optional[float]:
        """Get system SOH"""
        data = self.read_battery_data()
        return data.get('system_soh')

    def get_temperature(self) -> Optional[float]:
        """Get average cell temperature"""
        data = self.read_battery_data()
        return data.get('avg_cell_temp')

    def get_charge_current_limit(self) -> Optional[float]:
        """Get charge current limitation"""
        data = self.read_battery_data()
        return data.get('charge_current_limit')

    def get_discharge_current_limit(self) -> Optional[float]:
        """Get discharge current limitation"""
        data = self.read_battery_data()
        return data.get('discharge_current_limit')

    def get_alarm_status(self) -> Optional[int]:
        """Get alarm status bitfield"""
        data = self.read_battery_data()
        return data.get('alarm_status')

    def get_protection_status(self) -> Optional[int]:
        """Get protection status bitfield"""
        data = self.read_battery_data()
        return data.get('protection_status')

    def get_tray_counts(self) -> Tuple[Optional[int], Optional[int], Optional[int]]:
        """Get tray counts (total, normal, fault)"""
        data = self.read_battery_data()
        return (
            data.get('total_trays'),
            data.get('normal_trays'),
            data.get('fault_trays')
        )

    def is_connected(self) -> bool:
        """Check if CAN bus is connected and receiving data"""
        try:
            data = self.read_battery_data()
            return len(data) > 0 and time.time() - self.last_update < 5.0
        except:
            return False