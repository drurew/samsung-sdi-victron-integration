# Troubleshooting Log: Samsung SDI Battery Integration

This log summarizes a successful troubleshooting session involving the `samsung-sdi-victron-integration` package. It highlights common pitfalls, error messages, and their solutions.

## 1. Installation Issues

### Wrong Directory
*   **Symptom**: `chmod: /data/samsung-sdi/*.py: No such file or directory`
*   **Cause**: Files were extracted to `/samsung-sdi` or `/` instead of `/data/samsung-sdi`.
*   **Solution**: Ensure the repository is moved to `/data/samsung-sdi` before running installation scripts.

### Missing Dependencies
*   **Symptom**: `pip3: command not found` or `ModuleNotFoundError: No module named 'dbus'`
*   **Cause**: Venus OS is minimal and might lack python packages.
*   **Solution**: Use `opkg install python3-pip python3-can python3-dbus` (handled by newer setup scripts).

## 2. Service Errors (Software Bugs)

### Argparse Error
*   **Symptom**: `AttributeError: module 'argparse' has no attribute 'parse_args'`
*   **Cause**: Bug in `samsung_sdi_bms_service.py`. Code called `argparse.parse_args()` instead of `parser.parse_args()`.
*   **Status**: Fixed in v1.1.1.

### D-Bus Initialization Error
*   **Symptom**: `AttributeError: 'SamsungSDIMonitor' object has no attribute 'node_id'`
*   **Cause**: Variable name mismatch (`node_id` vs `system_id`).
*   **Status**: Fixed in v1.1.1.

### Aggregator Error
*   **Symptom**: `AttributeError: 'SamsungSDIAggregatorService' object has no attribute 'batteries'`
*   **Cause**: Variable name mismatch (`batteries` vs `systems`).
*   **Status**: Fixed in v1.1.1.

### Config File Not Found
*   **Symptom**: Service logs show `Config file /data/superb-bms/config.ini not found`.
*   **Cause**: The service was starting without passing the config file path argument.
*   **Status**: Fixed in `setup` and `install.sh` scripts.

## 3. Hardware & Wiring (CRITICAL)

### Setup
*   **Components**: Samsung SDI ELPM482-00005, Victron Cerbo GX.
*   **Interface**: Cerbo GX `VE.Can Port 1` usually maps to `vecan0`.

### The Pinout Mismatch
*   **Symptom**: No CAN traffic (`candump vecan0` shows nothing), or TX errors with `RX: 0`.
*   **Problem**: Samsung SDI uses **Pins 1 & 2** for CAN-H/L. Victron VE.Can uses **Pins 7 & 8**. A standard Ethernet cable connects 1-1, 2-2, sending CAN signals to unused pins on the Cerbo.
*   **Solution**: **Custom Crossover Cable is REQUIRED.**
    *   Battery Pin 1 (CAN-H) -> Cerbo Pin 7 (CAN-H)
    *   Battery Pin 2 (CAN-L) -> Cerbo Pin 8 (CAN-L)
    *   Battery Pin 3 (GND)   -> Cerbo Pin 3 (GND)

### CAN Termination
*   **Symptom**: `TX dropped` packets, `Bus warn`.
*   **Requirement**: 120Ω termination is needed at **BOTH** ends.
    *   **Battery End**: Turn on the T/R switch (or correct DIP switch).
    *   **Cerbo End**: Use a specific RJ45 terminator plug, or enable internal termination if supported (some Cerbo models do not have software switchable termination and require a plug).

### Battery Power-Up
*   **Procedure**:
    1.  Ensure all DIP switches are correct (Protocol: CAN, ID: 1).
    2.  Hold the **Red ON/OFF Button** for >3 seconds.
    3.  LEDs should show SOC (multiple LEDs lit), not just a single green standby LED.

## 4. Diagnostic Commands provided in Thread

*   Check service status: `svc -t /service/samsung-sdi-bms`
*   View logs: `tail -f /data/samsung-sdi/log/current`
*   Check CAN interface: `ip link show vecan0`
*   Monitor CAN traffic: `candump vecan0 -c` (requires `can-utils`)
