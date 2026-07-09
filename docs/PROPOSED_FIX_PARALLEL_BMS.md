# Proposed Fix: dbus-parallel-bms misses BMSes on reconnect

**Issue:** [victronenergy/venus#1652](https://github.com/victronenergy/venus/issues/1652)
**File:** `dbus-parallel-bms/dbus_parallel_bms.py`
**Severity:** High priority, filed 2026-07-02

## Root cause

When a Lynx BMS connects to D-Bus, `_device_added()` fires immediately via
`DbusMonitor`. The `_add_bms()` method reads `/ProductId`, `/Serial`, and
`/N2kDeviceInstance` via `get_value()`. If any of these return `None`
(because the BMS hasn't published them yet, or the D-Bus monitor hasn't
received the initial `ItemsChanged` signal), the method returns early:

```python
if None in (productId, serial, N2kDeviceInstance) or productId not in (...):
    return  # silently drops the BMS, no retry
```

The `_dbus_value_changed()` callback handles subsequent path updates, but
only checks `/FirmwareVersion`, `/N2kDeviceInstance`, `/ProductId`, and
`/Settings/Battery/NominalVoltage`. If the paths arrive in the same
`ItemsChanged` signal as `/Serial`, the service name is already gone from
the pending discovery list and never retried.

The timing window is tight — the BMS publishes paths within milliseconds
of registering its D-Bus service — but under load or when connecting to
a different CAN port (as noted in the issue), the window can be missed.

## Fix

Add a retry mechanism: if `_add_bms` fails the initial check, schedule a
retry via `GLib.timeout_add()` after 500ms. Retry up to twice, then
give up. Store pending service names in a dict to avoid duplicate retries.

```python
def _device_added(self, dbusServiceName, instance):
    if not self._add_bms(dbusServiceName):
        # Values not available yet — retry after a short delay
        if dbusServiceName not in self._pending:
            self._pending[dbusServiceName] = 0
        self._pending[dbusServiceName] += 1
        if self._pending[dbusServiceName] <= 2:
            GLib.timeout_add(500, self._retry_add_bms, dbusServiceName)

def _retry_add_bms(self, dbusServiceName):
    self._add_bms(dbusServiceName)
    self._update_parallel_bmses()
    return False  # don't repeat
```

And modify `_add_bms` to return a boolean:

```python
def _add_bms(self, dbusServiceName):
    ...
    if None in (productId, serial, N2kDeviceInstance) or productId not in (...):
        return False  # was: return
    ...
    return True
```

This is 3 lines changed + 8 lines added. The retry gives the BMS time
to publish its initial paths without blocking the D-Bus monitor thread.
