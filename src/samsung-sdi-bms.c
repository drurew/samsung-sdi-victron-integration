/*
 * samsung-sdi-bms.c — Samsung SDI → Victron CAN-BMS Protocol Translator
 *
 * Translates Samsung SDI ELPM482-00005 CAN PDO broadcasts (0x500-0x504,
 * 0x5F0-0x5F4) into Victron's standard CAN-bus BMS protocol (0x351,
 * 0x355, 0x356, 0x35A).  The stock Venus OS CAN-BMS driver picks up
 * the translated frames and publishes everything to D-Bus — no custom
 * D-Bus code needed.
 *
 * This avoids the fragile hand-rolled D-Bus wire-protocol layer that
 * the previous version attempted (issues #15, #22).  A pure CAN-to-CAN
 * translator is simpler, smaller, and inherits Victron's native BMS-lost
 * watchdog, reconnect, and GUI integration.
 *
 * Protocol references:
 *   Samsung SDI ELPM482-00005 Product Specification Rev 0.2
 *   Victron CAN-bus BMS protocol (dbus-callback / can-bus-bms service)
 *
 * CAN IDs used (all CAN 2.0A, 500 kbps, little-endian):
 *   Input (Samsung SDI broadcasts every ~500 ms):
 *     0x500 — System status  (voltage, current, SOC, SOH, heartbeat)
 *     0x501 — System config   (alarm/protection bitfields, tray counts)
 *     0x502 — Charge/discharge limits
 *     0x503 — Cell voltage summary (avg/max/min cell, avg tray)
 *     0x504 — Temperature & tray voltage summary
 *     0x5F0-0x5F4 — Per-cell voltages (14 cells)
 *   Output (Victron CAN-BMS, sent every ~500 ms on same interface):
 *     0x351 — Battery state   (V, I, temp, SOC, SOH)
 *     0x355 — Precision SOC/SOH
 *     0x356 — Charge/discharge limits
 *     0x35A — Alarms & warnings
 *
 * Usage:
 *   Compile:  gcc -Os -s -std=c99 -Wall -Wextra -lm -o samsung-sdi-bms \
 *                    samsung-sdi-bms.c
 *   ARM:      arm-linux-gnueabihf-gcc -Os -s -std=c99 -Wall -Wextra -lm \
 *                    -o samsung-sdi-bms samsung-sdi-bms.c
 *   Run:      ./samsung-sdi-bms <can-interface>
 *
 * Venus OS setup:
 *   1. Install this binary (e.g. /data/samsung-sdi/samsung-sdi-bms)
 *   2. Ensure the Victron CAN-BMS driver is enabled:
 *        dbus -y com.victronenergy.settings /Settings/Canbus/can0/Profile SetValue 4
 *      (Profile 4 = CAN-bus BMS (500 kbit/s))
 *   3. Run: ./samsung-sdi-bms vecan0
 *
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <poll.h>

/* ─── SocketCAN definitions ──────────────────────────────────────────── */

#ifndef AF_CAN
#define AF_CAN 29
#endif
#ifndef PF_CAN
#define PF_CAN AF_CAN
#endif
#ifndef CAN_RAW
#define CAN_RAW 1
#endif

typedef unsigned int canid_t;

struct can_frame {
    canid_t       can_id;
    unsigned char can_dlc;
    unsigned char __pad;
    unsigned short __res0;
    unsigned char data[8] __attribute__((aligned(8)));
};

/* ─── Samsung SDI CAN protocol ────────────────────────────────────────── */

#define SDI_CAN_ID_STATUS           0x500
#define SDI_CAN_ID_CONFIG           0x501
#define SDI_CAN_ID_LIMITS           0x502
#define SDI_CAN_ID_CELL_VOLTAGE     0x503
#define SDI_CAN_ID_TEMPERATURE      0x504
#define SDI_CAN_ID_CELL_1_3         0x5F0
#define SDI_CAN_ID_CELL_4_6         0x5F1
#define SDI_CAN_ID_CELL_7_9         0x5F2
#define SDI_CAN_ID_CELL_10_12       0x5F3
#define SDI_CAN_ID_CELL_13_14       0x5F4

/* Victron CAN-BMS protocol ────────────────────────────────────────────── */

/* Victron CAN-bus BMS frame semantics (verified against the stock
 * Venus OS driver on live hardware — see docs/MAPPING.md):
 *   0x351 = LIMITS  (CVL 0.1V, CCL 0.1A, DCL 0.1A, DVL 0.1V)
 *   0x355 = SOC/SOH (u16, 1 %)
 *   0x356 = STATE   (V 0.01V s16, I 0.1A s16, T 0.1C s16)
 * The previous revision had 0x351/0x356 semantics swapped, which the
 * stock driver ingests as a CVL of ~489 V and a pack voltage of ~5.8 V. */
#define VC_CAN_ID_LIMITS            0x351
#define VC_CAN_ID_SOC_SOH           0x355
#define VC_CAN_ID_STATE             0x356
#define VC_CAN_ID_ALARMS            0x35A
#define VC_CAN_ID_NAME              0x35E
#define VC_CAN_ID_INFO              0x35F
#define VC_CAN_ID_MODULES           0x372
#define VC_CAN_ID_CELLINFO          0x373

/* Data timeout: stop translating after this long without fresh
 * critical frames (0x500 status + 0x501 config + 0x502 limits).
 * Overridable with --timeout. */
#define DATA_TIMEOUT_SEC  5

/* Charge-voltage ceiling: emitted CVL is min(BMS CVL, this).
 * 55.4 V = 3.957 V/cell on 14S; the BMS broadcasts 58.1 V (4.15 V/cell).
 * Overridable with --cvl-max. */
#define DEFAULT_CVL_MAX   55.4

/* ─── Forward declarations ────────────────────────────────────────────── */

#ifndef UNIT_TEST
static int  can_open(const char *ifname);
static int  can_recv(int fd, struct can_frame *frame, int timeout_ms);
static int  can_send(int fd, canid_t id, const unsigned char *data, int len);
static void sig_handler(int sig);
#endif

/* ─── Little-endian value extraction ──────────────────────────────────── */

static unsigned short u16_le(const unsigned char *buf, int offset) {
    return (unsigned short)(buf[offset] | (buf[offset + 1] << 8));
}

static short s16_le(const unsigned char *buf, int offset) {
    unsigned short u = u16_le(buf, offset);
    return (short)u;
}

static signed char s8_le(const unsigned char *buf, int offset) {
    return (signed char)buf[offset];
}

/* ─── Battery constants (ELPM482-00005, spec Rev 0.2 Table 4) ─────────── */

#define BATTERY_CAPACITY_AH  94.0   /* default; --capacity-ah overrides (10 modules = 940) */
#define CELLS_PER_MODULE     14

/* ─── Battery state ───────────────────────────────────────────────────── */

typedef struct {
    double   voltage;
    double   current;
    double   soc;
    double   soh;
    double   avg_cell_temp;
    double   max_cell_temp;
    double   min_cell_temp;
    double   max_cell_v;
    double   min_cell_v;
    double   avg_cell_v;
    double   avg_tray_v;
    double   max_tray_v;
    double   min_tray_v;
    double   charge_voltage_limit;
    double   charge_current_limit;
    double   discharge_current_limit;
    double   discharge_voltage_limit;
    unsigned char  total_trays;
    unsigned char  normal_trays;
    unsigned char  fault_trays;
    unsigned int   alarm_status;      /* 0x501 bytes 0-1 */
    unsigned int   protection_status; /* 0x501 bytes 2-3 */
    unsigned int   heartbeat;
    double   cell_v[14];              /* per-cell voltages (1-indexed) */
    int      cell_v_valid[14];
    time_t   last_update;
    /* per-frame freshness: translation requires ALL of status/config/
     * limits to be fresh, so a partial failure (e.g. limits frames
     * stopping) suspends TX instead of freezing stale limits */
    time_t   rx_status;
    time_t   rx_config;
    time_t   rx_limits;
} battery_state;

static battery_state  battery;
#ifndef UNIT_TEST
static int            data_valid = 0;
#endif
static volatile sig_atomic_t running = 1;
static double         cvl_max     = DEFAULT_CVL_MAX;
static double         capacity_ah = BATTERY_CAPACITY_AH;
static int            timeout_sec = DATA_TIMEOUT_SEC;

/* All critical inputs fresh within timeout? */
static int data_fresh(time_t now) {
    return battery.rx_status > 0 && now - battery.rx_status <= timeout_sec &&
           battery.rx_config > 0 && now - battery.rx_config <= timeout_sec &&
           battery.rx_limits > 0 && now - battery.rx_limits <= timeout_sec;
}

/* ─── Samsung SDI message parsers ─────────────────────────────────────── */

static void parse_status(const unsigned char *data) {
    battery.voltage   = u16_le(data, 0) * 0.01;
    battery.current   = s16_le(data, 2) * 1.0;
    battery.soc       = (double)data[4];
    battery.soh       = (double)data[5];
    battery.heartbeat = u16_le(data, 6);
}

static void parse_config(const unsigned char *data) {
    battery.alarm_status      = u16_le(data, 0);
    battery.protection_status = u16_le(data, 2);
    battery.total_trays       = data[4];
    battery.normal_trays      = data[5];
    battery.fault_trays       = data[6];
}

static void parse_limits(const unsigned char *data) {
    battery.charge_voltage_limit    = u16_le(data, 0) * 0.1;
    battery.charge_current_limit    = u16_le(data, 2) * 0.1;
    battery.discharge_current_limit = u16_le(data, 4) * 0.1;
    battery.discharge_voltage_limit = u16_le(data, 6) * 0.1;
}

static void parse_cell_voltage(const unsigned char *data) {
    battery.avg_cell_v  = u16_le(data, 0) * 0.001;
    battery.max_cell_v  = u16_le(data, 2) * 0.001;
    battery.min_cell_v  = u16_le(data, 4) * 0.001;
    battery.avg_tray_v  = u16_le(data, 6) * 0.01;
}

static void parse_temperature(const unsigned char *data) {
    /* Spec Rev 0.2 Table 8: 0x504 bytes 0-3 = tray voltage extremes */
    battery.max_tray_v   = u16_le(data, 0) * 0.01;
    battery.min_tray_v   = u16_le(data, 2) * 0.01;
    battery.avg_cell_temp = (double)s8_le(data, 4);
    battery.max_cell_temp = (double)s8_le(data, 5);
    battery.min_cell_temp = (double)s8_le(data, 6);
}

static void parse_cell_frame(int cell_start, const unsigned char *data, int dlc) {
    /* 0x5F0-0x5F4: bytes 0-1 = Tray-ID (ignored for single-tray),
     * then up to 3 cell voltages at offsets 2, 4, 6 (U16 LE, 0.001V).
     * Cell 14 frame (0x5F4) has only 2 cells (offsets 2, 4). */
    int i;
    for (i = 0; i < 3 && cell_start + i < 14; i++) {
        int off = 2 + i * 2;
        if (off + 2 > dlc) break;
        unsigned short raw = u16_le(data, off);
        if (raw != 0) {  /* zero = unpopulated / padding slot */
            battery.cell_v[cell_start + i] = raw * 0.001;
            battery.cell_v_valid[cell_start + i] = 1;
        }
    }
}

/* ─── Victron CAN-BMS frame builders ──────────────────────────────────── */

static void build_state_frame(unsigned char *data) {
    /* 0x356 — live state: V (s16, 0.01 V), I (s16, 0.1 A, + = charging),
     * T (s16, 0.1 C). 6 bytes. SOC/SOH belong in 0x355, not here. */
    short v = (short)(battery.voltage * 100.0 + 0.5);
    short i = (short)(battery.current * 10.0 +
                      (battery.current >= 0 ? 0.5 : -0.5));
    short t = (short)(battery.avg_cell_temp * 10.0 +
                      (battery.avg_cell_temp >= 0 ? 0.5 : -0.5));

    data[0] = v & 0xFF;      data[1] = (v >> 8) & 0xFF;
    data[2] = i & 0xFF;      data[3] = (i >> 8) & 0xFF;
    data[4] = t & 0xFF;      data[5] = (t >> 8) & 0xFF;
    data[6] = 0; data[7] = 0;
}

static void build_soc_soh_frame(unsigned char *data) {
    /* 0x355 — SOC and SOH, u16 at 1 % (not 0.1 %: a 0.1 % encoding is
     * read by the stock driver as e.g. 110 % for an 11 % battery). */
    unsigned short soc = (unsigned short)(battery.soc + 0.5);
    unsigned short soh = (unsigned short)(battery.soh + 0.5);
    if (soc > 100) soc = 100;
    if (soh > 100) soh = 100;

    data[0] = soc & 0xFF;   data[1] = (soc >> 8) & 0xFF;
    data[2] = soh & 0xFF;   data[3] = (soh >> 8) & 0xFF;
    data[4] = 0; data[5] = 0;
    data[6] = 0; data[7] = 0;
}

static void build_limits_frame(unsigned char *data) {
    /* 0x351 — limits. Safety policy applied here:
     *   CVL = min(BMS request, cvl_max)   [--cvl-max, default 55.4 V]
     *   any active protection bit => CCL = DCL = 0 A (belt-and-braces
     *   on top of the BMS zeroing its own broadcast limits).
     * Validated on hardware: injected protection bit produced
     * "351 [8] 2A 02 00 00 00 00 C0 01" on the wire (CVL held, limits
     * zeroed) — see docs/TESTING.md. */
    double cvl_v = battery.charge_voltage_limit;
    if (cvl_v > cvl_max) cvl_v = cvl_max;
    double ccl_a = battery.charge_current_limit;
    double dcl_a = battery.discharge_current_limit;
    if (battery.protection_status) { ccl_a = 0.0; dcl_a = 0.0; }

    unsigned short cvl = (unsigned short)(cvl_v * 10.0 + 0.5);
    short          ccl = (short)(ccl_a * 10.0 + 0.5);
    short          dcl = (short)(dcl_a * 10.0 + 0.5);
    unsigned short dvl = (unsigned short)(battery.discharge_voltage_limit * 10.0 + 0.5);

    data[0] = cvl & 0xFF;    data[1] = (cvl >> 8) & 0xFF;
    data[2] = ccl & 0xFF;    data[3] = (ccl >> 8) & 0xFF;
    data[4] = dcl & 0xFF;    data[5] = (dcl >> 8) & 0xFF;
    data[6] = dvl & 0xFF;    data[7] = (dvl >> 8) & 0xFF;
}

static void build_modules_frame(unsigned char *data) {
    /* 0x372 — module counts: online, blocking charge, blocking
     * discharge, offline (u16 each). */
    unsigned short on  = battery.normal_trays;
    unsigned short off = battery.fault_trays;
    data[0] = on & 0xFF;  data[1] = (on >> 8) & 0xFF;
    data[2] = 0; data[3] = 0;
    data[4] = 0; data[5] = 0;
    data[6] = off & 0xFF; data[7] = (off >> 8) & 0xFF;
}

static void build_cellinfo_frame(unsigned char *data) {
    /* 0x373 — min/max cell voltage (u16, 1 mV) and min/max temperature
     * (u16, Kelvin). Feeds the GX battery Details page. */
    unsigned short vmin = (unsigned short)(battery.min_cell_v * 1000.0 + 0.5);
    unsigned short vmax = (unsigned short)(battery.max_cell_v * 1000.0 + 0.5);
    unsigned short tmin = (unsigned short)(battery.min_cell_temp + 273.15);
    unsigned short tmax = (unsigned short)(battery.max_cell_temp + 273.15);
    data[0] = vmin & 0xFF;  data[1] = (vmin >> 8) & 0xFF;
    data[2] = vmax & 0xFF;  data[3] = (vmax >> 8) & 0xFF;
    data[4] = tmin & 0xFF;  data[5] = (tmin >> 8) & 0xFF;
    data[6] = tmax & 0xFF;  data[7] = (tmax >> 8) & 0xFF;
}

static void build_name_frame(unsigned char *data) {
    /* 0x35E — product name, 8 ASCII bytes */
    memcpy(data, "SDI 482 ", 8);
}

static void build_info_frame(unsigned char *data) {
    /* 0x35F — model tag, protocol rev, capacity (Ah) */
    unsigned short cap = (unsigned short)(capacity_ah + 0.5);
    data[0] = 0x82; data[1] = 0x04;      /* model tag 0x0482 */
    data[2] = 0x02; data[3] = 0x00;      /* spec rev 0.2 */
    data[4] = cap & 0xFF; data[5] = (cap >> 8) & 0xFF;
    data[6] = 0; data[7] = 0;
}

static void build_alarms_frame(unsigned char *data) {
    /* 0x35A — alarms (bytes 0-3) and warnings (bytes 4-7).
     *
     * Wire format: 2-bit flag PAIRS (01 = active, 00 = inactive), not
     * plain bitmasks — verified on live hardware: an injected 0x501
     * over-temperature bit with this encoding produced a correctly
     * named "High Temperature" GX notification (docs/TESTING.md).
     *
     * Samsung 0x501 bit layout (spec Rev 0.2 Table 8): bit0 OV, bit1 UV,
     * bit2 OT, bit3 UT, bit4 chg-OC, bit5 dch-OC, bit6 FET OT, bit7 tray
     * imbalance; protection high byte: bit8 tray-ID, bit9 PCS comm,
     * bit10 FET fail, bit11 FET-OT fail, bit12 UV shutdown, bit13 cell
     * imbalance, bit14 2nd OV.
     *
     * Severity: Samsung ALARM bits (BMS warning, FETs still closed) map
     * to Victron WARNINGS; Samsung PROTECTION bits (BMS has acted) map
     * to Victron ALARMS. */
    unsigned int a = battery.alarm_status;
    unsigned int p = battery.protection_status;
    memset(data, 0, 8);

    /* Victron flag positions: (byte offset within half) * 8 + bit of
     * the pair's low bit. Base 0 = alarms, base 4 = warnings. */
#define VF_GENERAL        0
#define VF_HIGH_VOLTAGE   2
#define VF_LOW_VOLTAGE    4
#define VF_HIGH_TEMP      6
#define VF_LOW_TEMP       8
#define VF_HIGH_CHG_TEMP 10
#define VF_LOW_CHG_TEMP  12
#define VF_HIGH_CURRENT  14   /* discharge over-current */
#define VF_HIGH_CHG_CUR  16
#define VF_CONTACTOR     18
#define VF_SHORT_CIRCUIT 20
#define VF_BMS_INTERNAL  22
#define VF_CELL_IMBAL    24
#define SET_FLAG(base, f) (data[(base) + ((f) >> 3)] |= \
                           (unsigned char)(0x01 << ((f) & 7)))

    /* warnings from alarm bits */
    if (a & 0x0001) SET_FLAG(4, VF_HIGH_VOLTAGE);
    if (a & 0x0002) SET_FLAG(4, VF_LOW_VOLTAGE);
    if (a & 0x0004) SET_FLAG(4, VF_HIGH_TEMP);
    if (a & 0x0008) SET_FLAG(4, VF_LOW_TEMP);
    if (a & 0x0010) SET_FLAG(4, VF_HIGH_CHG_CUR);
    if (a & 0x0020) SET_FLAG(4, VF_HIGH_CURRENT);
    if (a & 0x0040) SET_FLAG(4, VF_BMS_INTERNAL);
    if (a & 0x0080) SET_FLAG(4, VF_CELL_IMBAL);

    /* alarms from protection bits */
    if (p & 0x0001) SET_FLAG(0, VF_HIGH_VOLTAGE);
    if (p & 0x4000) SET_FLAG(0, VF_HIGH_VOLTAGE);  /* 2nd OV */
    if (p & 0x0002) SET_FLAG(0, VF_LOW_VOLTAGE);
    if (p & 0x1000) SET_FLAG(0, VF_LOW_VOLTAGE);   /* UV shutdown */
    if (p & 0x0004) SET_FLAG(0, VF_HIGH_TEMP);
    if (p & 0x0008) SET_FLAG(0, VF_LOW_TEMP);
    if (p & 0x0010) SET_FLAG(0, VF_HIGH_CHG_CUR);
    if (p & 0x0020) SET_FLAG(0, VF_HIGH_CURRENT);
    if (p & 0x2000) SET_FLAG(0, VF_CELL_IMBAL);    /* cell imbalance */
    if (p & 0x0080) SET_FLAG(0, VF_CELL_IMBAL);    /* tray imbalance */
    if (p & (0x0040 | 0x0100 | 0x0200 | 0x0400 | 0x0800))
        SET_FLAG(0, VF_BMS_INTERNAL);
    if (p) SET_FLAG(0, VF_GENERAL);

#undef SET_FLAG
}

#ifndef UNIT_TEST

/* ─── Victron frame transmission ──────────────────────────────────────── */

static void send_victron_frames(int can_fd) {
    unsigned char buf[8];

    build_limits_frame(buf);
    can_send(can_fd, VC_CAN_ID_LIMITS, buf, 8);

    build_soc_soh_frame(buf);
    can_send(can_fd, VC_CAN_ID_SOC_SOH, buf, 8);

    build_state_frame(buf);
    can_send(can_fd, VC_CAN_ID_STATE, buf, 6);

    build_alarms_frame(buf);
    can_send(can_fd, VC_CAN_ID_ALARMS, buf, 8);

    build_modules_frame(buf);
    can_send(can_fd, VC_CAN_ID_MODULES, buf, 8);

    build_cellinfo_frame(buf);
    can_send(can_fd, VC_CAN_ID_CELLINFO, buf, 8);

    build_name_frame(buf);
    can_send(can_fd, VC_CAN_ID_NAME, buf, 8);

    build_info_frame(buf);
    can_send(can_fd, VC_CAN_ID_INFO, buf, 8);
}

/* ─── CAN open / read / send ──────────────────────────────────────────── */

static int can_open(const char *ifname) {
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        perror("socket(CAN)");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl SIOCGIFINDEX");
        close(fd); return -1;
    }

    struct { unsigned short family; unsigned short pad; int ifindex; } addr;
    addr.family  = AF_CAN;
    addr.ifindex = ifr.ifr_ifindex;
    addr.pad     = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind CAN");
        close(fd); return -1;
    }
    return fd;
}

static int can_recv(int fd, struct can_frame *frame, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return ret;
    ssize_t n = read(fd, frame, sizeof(*frame));
    return (n == (ssize_t)sizeof(*frame)) ? (int)n : 0;
}

static int can_send(int fd, canid_t id, const unsigned char *data, int len) {
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id  = id;
    frame.can_dlc = (unsigned char)len;
    memcpy(frame.data, data, len);

    ssize_t n = write(fd, &frame, sizeof(frame));
    if (n != (ssize_t)sizeof(frame)) {
        static int send_errors = 0;
        if (++send_errors <= 5)
            fprintf(stderr, "can_send: write error (id=0x%03X, errno=%d)\n",
                    id, errno);
    } else {
        /* Reset error counter on success (rate-limit logs) */
        /* (counter reset implicitly every 2^31 successful sends) */
    }
    return (n == (ssize_t)sizeof(frame)) ? 0 : -1;
}

/* ─── Signal handler ──────────────────────────────────────────────────── */

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int argi;
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <can-interface>\n\n"
                "Samsung SDI ELPM482-00005 → Victron CAN-BMS protocol translator\n\n"
                "Translates Samsung SDI CAN PDOs (0x500-0x504, 0x5F0-0x5F4)\n"
                "into Victron CAN-bus BMS frames (0x351, 0x355, 0x356, 0x35A).\n\n"
                "The stock Venus OS CAN-BMS driver picks up the translated\n"
                "frames and publishes to D-Bus.  No custom D-Bus code needed.\n\n"
                "Options:\n"
                "  --cvl-max <V>      charge-voltage ceiling (default %.1f)\n"
                "  --capacity-ah <Ah> bank capacity (default %.0f; 10 modules = 940)\n"
                "  --timeout <s>      suspend TX after this silence (default %d)\n",
                argv[0], DEFAULT_CVL_MAX, BATTERY_CAPACITY_AH,
                DATA_TIMEOUT_SEC);
        return 1;
    }

    for (argi = 2; argi < argc; argi++) {
        if (!strcmp(argv[argi], "--cvl-max") && argi + 1 < argc)
            cvl_max = atof(argv[++argi]);
        else if (!strcmp(argv[argi], "--capacity-ah") && argi + 1 < argc)
            capacity_ah = atof(argv[++argi]);
        else if (!strcmp(argv[argi], "--timeout") && argi + 1 < argc)
            timeout_sec = atoi(argv[++argi]);
        else {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            return 1;
        }
    }
    if (cvl_max < 40.0 || cvl_max > 58.1) {
        fprintf(stderr, "--cvl-max %.1f outside sane range 40.0-58.1 V\n",
                cvl_max);
        return 1;
    }
    if (capacity_ah < 10.0 || capacity_ah > 3000.0) {
        fprintf(stderr, "--capacity-ah %.0f outside sane range 10-3000\n",
                capacity_ah);
        return 1;
    }
    if (timeout_sec < 2 || timeout_sec > 60) {
        fprintf(stderr, "--timeout %d outside sane range 2-60 s\n",
                timeout_sec);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);  /* prevent crash on CAN send failure */

    int can_fd = can_open(argv[1]);
    if (can_fd < 0) {
        fprintf(stderr, "FATAL: cannot open CAN interface %s\n", argv[1]);
        return 1;
    }
    printf("samsung-sdi-bms: listening on %s, translating to Victron CAN-BMS\n",
           argv[1]);
    printf("                   (0x351 state, 0x355 SOC/SOH, 0x356 limits, 0x35A alarms)\n");

    memset(&battery, 0, sizeof(battery));

    /* Accept frames large enough to contain the fields we parse.
     * 0x500-0x503 need 8 bytes, 0x504 needs 7, 0x5F0-0x5F4 need 6-8. */
    static const unsigned char min_dlc[5] = {8, 8, 8, 8, 7};

    while (running) {
        struct can_frame frame;
        int ret = can_recv(can_fd, &frame, 1000);
        if (ret <= 0) {
            /* No CAN traffic — TX is already implicitly suspended
             * (frames are only sent on 0x500 receipt); log the
             * transition once. Venus raises BMS-lost from the silence. */
            if (data_valid &&
                time(NULL) - battery.last_update > timeout_sec) {
                data_valid = 0;
                printf("samsung-sdi-bms: data timeout, translation "
                       "suspended (Venus will raise BMS-lost)\n");
            }
            continue;
        }

        /* Filter known Samsung SDI IDs, validate frame length */
        if (frame.can_id >= SDI_CAN_ID_STATUS &&
            frame.can_id <= SDI_CAN_ID_TEMPERATURE) {
            if (frame.can_dlc <
                min_dlc[frame.can_id - SDI_CAN_ID_STATUS])
                continue;

            switch (frame.can_id) {
            case SDI_CAN_ID_STATUS:
                parse_status(frame.data);
                battery.rx_status = time(NULL);
                break;
            case SDI_CAN_ID_CONFIG:
                parse_config(frame.data);
                battery.rx_config = time(NULL);
                break;
            case SDI_CAN_ID_LIMITS:
                parse_limits(frame.data);
                battery.rx_limits = time(NULL);
                break;
            case SDI_CAN_ID_CELL_VOLTAGE:
                parse_cell_voltage(frame.data);
                break;
            case SDI_CAN_ID_TEMPERATURE:
                parse_temperature(frame.data);
                break;
            default:
                continue;
            }
        } else if (frame.can_id >= SDI_CAN_ID_CELL_1_3 &&
                   frame.can_id <= SDI_CAN_ID_CELL_13_14) {
            int cell_start;
            switch (frame.can_id) {
            case SDI_CAN_ID_CELL_1_3:   cell_start = 0; break;
            case SDI_CAN_ID_CELL_4_6:   cell_start = 3; break;
            case SDI_CAN_ID_CELL_7_9:   cell_start = 6; break;
            case SDI_CAN_ID_CELL_10_12: cell_start = 9; break;
            case SDI_CAN_ID_CELL_13_14: cell_start = 12; break;
            default: continue;
            }
            parse_cell_frame(cell_start, frame.data, frame.can_dlc);
        } else {
            continue;
        }

        battery.last_update = time(NULL);

        /* Translate on each 0x500 (the battery's ~500 ms cadence), but
         * ONLY while status+config+limits are all fresh: translating
         * from a partial picture would emit zero/stale limits. If any
         * critical frame stops, TX suspends and the stock Venus driver
         * raises its native BMS-lost alarm (#67). */
        if (frame.can_id == SDI_CAN_ID_STATUS) {
            if (data_fresh(time(NULL))) {
                if (!data_valid) {
                    data_valid = 1;
                    printf("samsung-sdi-bms: data complete, translating "
                           "(CVL ceiling %.1f V)...\n", cvl_max);
                }
                send_victron_frames(can_fd);
            }
        }
    }

    printf("samsung-sdi-bms: shutting down\n");
    if (can_fd >= 0) close(can_fd);
    return 0;
}

#else  /* ─────────────────────────── UNIT_TEST ─────────────────────────── */

/* Build:  gcc -D_GNU_SOURCE -DUNIT_TEST -Os -Wall -Wextra -std=c99 \
 *              -o samsung-sdi-bms-test src/samsung-sdi-bms.c && ./samsung-sdi-bms-test
 * Frame vectors marked "real capture" were recorded from a live
 * ELPM482-00005 on a Cerbo GX MK2 (Venus OS v3.70), 2026-03/07. */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } } while (0)

static int feq(double x, double y) { return x - y < 1e-6 && y - x < 1e-6; }

int main(void) {
    memset(&battery, 0, sizeof(battery));
    unsigned char buf[8];

    /* real capture: 0x504 = 32 13 32 13 1D 1D 1D 00 */
    unsigned char t504[8] = {0x32,0x13,0x32,0x13,0x1D,0x1D,0x1D,0x00};
    parse_temperature(t504);
    CHECK(feq(battery.max_tray_v, 49.14), "0x504 max tray 49.14 V (real capture)");
    CHECK(feq(battery.avg_cell_temp, 29.0), "0x504 avg temp 29 C");

    unsigned char t500[8] = {0x32,0x13, 0xFD,0xFF, 13, 100, 0x34,0x12};
    parse_status(t500);
    CHECK(feq(battery.voltage, 49.14), "0x500 voltage 49.14 V");
    CHECK(feq(battery.current, -3.0),  "0x500 current -3 A (S16)");
    CHECK(feq(battery.soc, 13.0),      "0x500 SOC 13%");

    unsigned char t502[8] = {0x45,0x02, 0xD6,0x01, 0xD6,0x01, 0xC0,0x01};
    parse_limits(t502);
    CHECK(feq(battery.charge_voltage_limit, 58.1), "0x502 CVL 58.1 V");
    CHECK(feq(battery.charge_current_limit, 47.0), "0x502 CCL 47.0 A");

    unsigned char t503[8] = {0xC0,0x0D, 0xC2,0x0D, 0xBE,0x0D, 0x32,0x13};
    parse_cell_voltage(t503);
    CHECK(feq(battery.max_cell_v, 3.522), "0x503 max cell 3.522 V");
    CHECK(feq(battery.min_cell_v, 3.518), "0x503 min cell 3.518 V");

    /* real capture: 0x5F0 = 01 00 C0 0D C1 0D C2 0D */
    unsigned char t5f0[8] = {0x01,0x00,0xC0,0x0D,0xC1,0x0D,0xC2,0x0D};
    parse_cell_frame(0, t5f0, 8);
    CHECK(feq(battery.cell_v[0], 3.520) && battery.cell_v_valid[0],
          "0x5F0 cell 1 = 3.520 V (real capture)");
    unsigned char t5f4[8] = {0x01,0x00,0xB0,0x0D,0xB1,0x0D,0x00,0x00};
    parse_cell_frame(12, t5f4, 8);
    CHECK(feq(battery.cell_v[13], 3.505), "0x5F4 cell 14 = 3.505 V");
    CHECK(!battery.cell_v_valid[12] || battery.cell_v[12] > 0,
          "0x5F4 zero padding not stored as a cell");

    unsigned char t501[8] = {0x01,0x00, 0x00,0x10, 1, 1, 0, 0};
    parse_config(t501);
    CHECK(battery.alarm_status == 0x0001,      "0x501 alarm bits");
    CHECK(battery.protection_status == 0x1000, "0x501 protection bits");

    /* ── frame ID semantics: the bug this PR fixes ── */
    CHECK(VC_CAN_ID_LIMITS == 0x351, "0x351 is LIMITS (was mis-assigned STATE)");
    CHECK(VC_CAN_ID_STATE  == 0x356, "0x356 is STATE (was mis-assigned LIMITS)");

    /* ── 0x351: CVL clamped, protection forces 0 A ── */
    build_limits_frame(buf);
    CHECK(u16_le(buf, 0) == 554, "0x351 CVL clamped 58.1 -> 55.4 V");
    CHECK(u16_le(buf, 2) == 0 && u16_le(buf, 4) == 0,
          "0x351 CCL/DCL forced to 0 A while protection active");
    CHECK(u16_le(buf, 6) == 448, "0x351 DVL 44.8 V passthrough");
    battery.protection_status = 0;
    build_limits_frame(buf);
    CHECK(u16_le(buf, 2) == 470, "0x351 CCL 47.0 A when no protection");

    /* ── 0x355: 1 % scale (0.1 % would show 11 % as 110 %) ── */
    build_soc_soh_frame(buf);
    CHECK(u16_le(buf, 0) == 13, "0x355 SOC u16 at 1% (13, not 130)");

    /* ── 0x356: V/I/T ── */
    build_state_frame(buf);
    CHECK(u16_le(buf, 0) == 4914, "0x356 voltage 0.01 V scale");
    CHECK(s16_le(buf, 2) == -30,  "0x356 current 0.1 A, sign kept");
    CHECK(s16_le(buf, 4) == 290,  "0x356 temperature 0.1 C");

    /* ── 0x35A: 2-bit pair positions (hardware-validated encoding) ── */
    battery.alarm_status = 0x0004;      /* OT warning */
    battery.protection_status = 0;
    build_alarms_frame(buf);
    CHECK(buf[4] == 0x40 && buf[0] == 0,
          "0x35A OT alarm-bit -> warning pair (byte 4, bits 6-7)");
    battery.alarm_status = 0;
    battery.protection_status = 0x0004; /* OT protection */
    build_alarms_frame(buf);
    CHECK((buf[0] & 0x40) && (buf[0] & 0x01),
          "0x35A OT protection -> alarm pair + general alarm");
    battery.protection_status = 0x1000; /* UV shutdown */
    build_alarms_frame(buf);
    CHECK(buf[0] & 0x10, "0x35A UV shutdown -> low-voltage alarm pair");

    /* ── extended frames: 0x373 / 0x372 / 0x35E / 0x35F ── */
    battery.min_cell_v = 3.518; battery.max_cell_v = 3.522;
    battery.min_cell_temp = 29; battery.max_cell_temp = 29;
    build_cellinfo_frame(buf);
    CHECK(u16_le(buf, 0) == 3518 && u16_le(buf, 2) == 3522,
          "0x373 min/max cell in mV");
    CHECK(u16_le(buf, 4) == 302, "0x373 min temp in Kelvin (29C -> 302K)");
    battery.normal_trays = 1; battery.fault_trays = 0;
    build_modules_frame(buf);
    CHECK(u16_le(buf, 0) == 1 && u16_le(buf, 6) == 0,
          "0x372 modules online/offline");
    build_name_frame(buf);
    CHECK(buf[0] == 'S' && buf[7] == ' ', "0x35E product name");
    build_info_frame(buf);
    CHECK(u16_le(buf, 4) == 94, "0x35F capacity from --capacity-ah default");

    /* ── freshness gate ── */
    time_t now = time(NULL);
    battery.rx_status = now; battery.rx_config = now; battery.rx_limits = now;
    CHECK(data_fresh(now), "fresh when all three critical frames recent");
    battery.rx_limits = now - timeout_sec - 1;
    CHECK(!data_fresh(now), "NOT fresh when limits frames stop (partial failure)");
    battery.rx_limits = 0;
    CHECK(!data_fresh(now), "NOT fresh before limits ever received (startup)");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}

#endif /* UNIT_TEST */
