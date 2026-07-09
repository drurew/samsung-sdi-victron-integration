# Proposed Fix: Battery CCL ignored by MPPT control

**Issue:** [victronenergy/venus#1358](https://github.com/victronenergy/venus/issues/1358)
**File:** `dbus-systemcalc-py/delegates/dvcc.py`, method `set_maxchargecurrent()`
**Severity:** Battery overcurrent protection trips, 4+ BMS vendors affected

## Root cause

In `set_maxchargecurrent()`, when DC-coupled PV feed-in is enabled on a
VE.Bus Multi system (`feedback_allowed = True`), all solar chargers are
set to their hardware maximum current via `maximize_charge_current()`.
The BMS-reported CCL is completely ignored:

```python
# Line 671-673 (current code)
elif feedback_allowed and not pv_disabled:
    for charger in chargers:
        charger.maximize_charge_current()
```

The rationale in the comment says "let the Multi feed it in using
overvoltage-feedin." This doesn't work when the MPPT capacity exceeds
the battery's charge current limit — the MPPTs charge the battery at
whatever the sun provides, the BMS eventually trips on overcurrent,
and no grid feed-in happens because the battery voltage hasn't yet
reached the overvoltage threshold.

## The fix

Replace the `maximize_charge_current()` call with `_set_charge_current()`,
which distributes the BMS CCL across solar chargers proportionally to
their capacity:

```python
# Line 671-673 (proposed fix)
elif feedback_allowed and not pv_disabled:
    # Respect the BMS CCL on solar chargers. The Multi feeds
    # any excess PV power to the grid via overvoltage-feedin.
    # Previously maximize_charge_current() was used here, ignoring
    # the BMS CCL and causing battery overcurrent when MPPT
    # capacity exceeds the battery charge limit (#1358).
    self._set_charge_current(solarchargers, max_charge_current)
    for charger in inverterchargers:
        charger.maximize_charge_current()
```

This is a 4-line change (1 line replaced, 3 added). The Multi-RS path
(Lines 664-670) already does this correctly — this fix makes the VE.Bus
Multi path consistent with the Multi-RS path.

## Why this works

1. Solar chargers (`solarchargers`) get the BMS CCL distributed across
   them via `_set_charge_current()`, which writes `/Link/ChargeCurrent`
   on each MPPT. No MPPT produces more current than its share of the CCL.
2. Inverter/chargers (`inverterchargers`) are maximized because they
   operate on the AC side and the MultiPlus handles their output.
3. If the MPPTs would produce more power than the CCL, the MultiPlus
   raises the AC output voltage (overvoltage-feedin), pushing excess
   PV power to the grid. This is the same mechanism Victron already
   relies on — it just needs the CCL to be respected first.
4. The `stop_on_mcc0` path (Lines 650-651) already forces CCL=0 when
   the BMS signals zero current. This fix ensures non-zero CCL values
   are also respected.

## Impact

- **BYD LV**: `_byd_quirk()` returns `(55V, 40A, ...)` when CCL=0.
  With this fix, solar chargers are limited to 40A, preventing
  overcurrent during the float phase.
- **REC BMS**: CCL is now respected during bulk/absorption charging
  when PV power exceeds the battery's charge rate.
- **JK BMS**: CCL reported via CAN-BMS protocol is propagated to MPPTs
  instead of being silently ignored.
- **Pace BMS (Gobel Power)**: Same as above.

## Regression risk

Low. The `_set_charge_current()` function already exists and is used
by the non-feedback case (Line 676). The Multi-RS path (Line 664) uses
a more complex distribution that accounts for discharge capacity, but
the core concept of respecting CCL on solar chargers is identical.
