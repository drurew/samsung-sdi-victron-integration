# Test Procedure ("the drill card")

Run ALL of this on your own hardware before letting DVCC act on the
translator's output. Nothing here requires charging; keep the system's
battery monitor pointed elsewhere (or Automatic) throughout, except
drill 4. Evidence of one full pass: docs/bench-reports/.

## 0. Unit tests
`gcc -D_GNU_SOURCE -DUNIT_TEST -Os -Wall -Wextra -std=c99 -o test src/samsung-sdi-bms.c && ./test` → 28/28 (no libdbus needed). Validates parsing, clamping, protection-zeroing and
DLC guards against real captured frames.

## 1. Data validation (zero risk)
Viewer `--once`: cross-check pack voltage (multimeter), SOC, CVL/CCL/DCL
against expectations; alarms/protections none; all cells present.
Driver foreground with `--can-bms`: confirm `CVL=x(→clamped)`.
`candump vecan0,300:700`: 0x35x set at 1 Hz. Decode one 0x351 by hand:
bytes 0-1 little-endian × 0.1 V must equal your clamp, not the BMS's ask.

## 2. Cable-pull drill
Unplug battery CAN at the GX end. Expect within `--timeout` seconds:
translator prints `TX suspended`; 0x35x frames stop; the translator's
battery drops from the Device List. Replug: self-recovery, no restart.
FAIL = any stale value persisting as a live battery.

## 3. Crash & boot drills
`kill -9 $(pidof sdi-victron-translator)` → daemontools restarts it
within seconds (new pid). `reboot` → service up unaided after boot.

## 4. BMS-lost under selection
Temporarily set the translator's battery as the system battery monitor.
Pull the cable again: the GX must raise the BMS connection-lost condition
(this only fires for the *selected* battery). Restore selection after.

## 5. Alarm & protection injection (battery must stay CONNECTED)
CAN frames need an ACKing node — injecting on a dead bus jams the TX
queue ("No buffer space available"; recover with ip link down/up).
Inject alongside the live battery; your 20-50 Hz beats its 2 Hz.

Warning path (alarm bit 2 = over-temperature):
```sh
while :; do cansend vecan0 501#0400000001010000; sleep 0.05; done
```
→ GX shows the correctly-NAMED high-temperature notification.

Protection path (protection bit 2):
```sh
candump vecan0,351:7FF,35A:7FF > /tmp/drill.log & CDPID=$!
while :; do cansend vecan0 501#0000040001010000; sleep 0.02; done
# Ctrl+C after ~6 s, then: kill $CDPID
grep " 351 " /tmp/drill.log | sort | uniq -c
```
→ must contain `2A 02 00 00 00 00 C0 01`-pattern frames: CCL/DCL forced
to zero while CVL holds the clamp. Severity on the GX escalates to alarm.
Everything self-restores within seconds of stopping the loop.

## 6. Sign-off
All green → the translator may be selected as battery monitor and DVCC
enabled. First charge: low power, supervised, confirming taper toward
the CLAMPED voltage. Keep a VEConfigure absorption cap and the module
dry-contact as independent backstops regardless.
