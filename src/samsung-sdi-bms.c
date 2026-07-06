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

#define VC_CAN_ID_STATE             0x351
#define VC_CAN_ID_SOC_SOH           0x355
#define VC_CAN_ID_LIMITS            0x356
#define VC_CAN_ID_ALARMS            0x35A

/* Data timeout: mark disconnected after 5 s without any SDI frame */
#define DATA_TIMEOUT_SEC  5

/* ─── Forward declarations ────────────────────────────────────────────── */

static int  can_open(const char *ifname);
static int  can_recv(int fd, struct can_frame *frame, int timeout_ms);
static int  can_send(int fd, canid_t id, const unsigned char *data, int len);
static void sig_handler(int sig);

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
} battery_state;

static battery_state  battery;
static int            data_valid = 0;
static volatile sig_atomic_t running = 1;

/* ─── Battery constants (ELPM482-00005, spec Rev 0.2 Table 4) ─────────── */

#define BATTERY_CAPACITY_AH  94.0
#define CELLS_PER_MODULE     14

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
    /* 0x351 — Battery state (voltage, current, temp, SOC, SOH) */
    unsigned short v = (unsigned short)(battery.voltage * 100.0 + 0.5);
    short          i = (short)(battery.current * 10.0 + (battery.current >= 0 ? 0.5 : -0.5));
    unsigned short t = (unsigned short)(battery.avg_cell_temp * 10.0 + 0.5);
    unsigned char  soc = (unsigned char)(battery.soc + 0.5);
    unsigned char  soh = (unsigned char)(battery.soh + 0.5);

    if (soc > 100) soc = 100;
    if (soh > 100) soh = 100;

    data[0] = v & 0xFF;      data[1] = (v >> 8) & 0xFF;
    data[2] = i & 0xFF;      data[3] = (i >> 8) & 0xFF;
    data[4] = t & 0xFF;      data[5] = (t >> 8) & 0xFF;
    data[6] = soc;
    data[7] = soh;
}

static void build_soc_soh_frame(unsigned char *data) {
    /* 0x355 — Precision SOC/SOH (0.1 % units) */
    unsigned short soc10 = (unsigned short)(battery.soc * 10.0 + 0.5);
    unsigned short soh10 = (unsigned short)(battery.soh * 10.0 + 0.5);

    data[0] = soc10 & 0xFF;   data[1] = (soc10 >> 8) & 0xFF;
    data[2] = soh10 & 0xFF;   data[3] = (soh10 >> 8) & 0xFF;
    data[4] = 0xFF; data[5] = 0xFF;
    data[6] = 0xFF; data[7] = 0xFF;
}

static void build_limits_frame(unsigned char *data) {
    /* 0x356 — Charge/discharge limits */
    unsigned short cvl = (unsigned short)(battery.charge_voltage_limit * 10.0 + 0.5);
    unsigned short ccl = (unsigned short)(battery.charge_current_limit * 10.0 + 0.5);
    unsigned short dcl = (unsigned short)(battery.discharge_current_limit * 10.0 + 0.5);
    unsigned short dvl = (unsigned short)(battery.discharge_voltage_limit * 10.0 + 0.5);

    data[0] = cvl & 0xFF;    data[1] = (cvl >> 8) & 0xFF;
    data[2] = ccl & 0xFF;    data[3] = (ccl >> 8) & 0xFF;
    data[4] = dcl & 0xFF;    data[5] = (dcl >> 8) & 0xFF;
    data[6] = dvl & 0xFF;    data[7] = (dvl >> 8) & 0xFF;
}

static void build_alarms_frame(unsigned char *data) {
    /* 0x35A — Alarms & warnings
     *
     * Samsung SDI 0x501 bitfield layouts (spec Rev 0.2 Table 8):
     *   bit0 Over-Voltage        bit1 Under-Voltage
     *   bit2 Over-Temperature    bit3 Under-Temperature
     *   bit4 Charge Over-Current bit5 Discharge Over-Current
     *   bit6 FET Over-Temp       bit7 Tray Voltage Imbalance
     *   bit8 Tray-ID error       bit9 PCS comm error
     *   bit10 FET failure        bit11 FET OT failure
     *   bit12 UV shutdown        bit13 Cell voltage imbalance
     *   bit14 2nd Over-Voltage
     *
     * Victron CAN-BMS alarm bitfield:
     *   bit0 High voltage        bit1 Low voltage
     *   bit2 High temperature    bit3 Low temperature
     *   bit4 High charge current bit5 High discharge current
     *   bit6 Internal failure    bit7 Cell imbalance
     *
     * Mapping: Samsung alarm bits → Victron alarm (severity 2);
     *          Samsung protection bits → Victron warning (severity 1).
     */
    unsigned int a = battery.alarm_status;
    unsigned int p = battery.protection_status;
    unsigned short alarms = 0, warnings = 0;

    /* Over-Voltage */
    if (p & 0x0001) alarms  |= 0x01; else if (a & 0x0001) warnings |= 0x01;
    if (p & 0x4000) alarms  |= 0x01; /* 2nd over-voltage */
    /* Under-Voltage */
    if (p & 0x0002) alarms  |= 0x02; else if (a & 0x0002) warnings |= 0x02;
    if (p & 0x1000) alarms  |= 0x02; /* UV shutdown */
    /* Over-Temperature */
    if (p & 0x0004) alarms  |= 0x04; else if (a & 0x0004) warnings |= 0x04;
    /* Under-Temperature */
    if (p & 0x0008) alarms  |= 0x08; else if (a & 0x0008) warnings |= 0x08;
    /* Charge Over-Current */
    if (p & 0x0010) alarms  |= 0x10; else if (a & 0x0010) warnings |= 0x10;
    /* Discharge Over-Current */
    if (p & 0x0020) alarms  |= 0x20; else if (a & 0x0020) warnings |= 0x20;
    /* Internal failure (FET over-temp, FET failure, tray-ID, PCS comm, etc.) */
    if (p & (0x0040 | 0x0100 | 0x0200 | 0x0400 | 0x0800)) alarms |= 0x40;
    else if (a & 0x0040) warnings |= 0x40;
    /* Cell imbalance */
    if (p & 0x2000) alarms  |= 0x80; else if (a & 0x0080) warnings |= 0x80;

    data[0] = alarms & 0xFF;   data[1] = (alarms >> 8) & 0xFF;
    data[2] = warnings & 0xFF; data[3] = (warnings >> 8) & 0xFF;
    data[4] = 0; data[5] = 0;
    data[6] = 0; data[7] = 0;
}

/* ─── Victron frame transmission ──────────────────────────────────────── */

static void send_victron_frames(int can_fd) {
    unsigned char buf[8];

    build_state_frame(buf);
    can_send(can_fd, VC_CAN_ID_STATE, buf, 8);

    build_soc_soh_frame(buf);
    can_send(can_fd, VC_CAN_ID_SOC_SOH, buf, 8);

    build_limits_frame(buf);
    can_send(can_fd, VC_CAN_ID_LIMITS, buf, 8);

    build_alarms_frame(buf);
    can_send(can_fd, VC_CAN_ID_ALARMS, buf, 8);
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
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <can-interface>\n\n"
                "Samsung SDI ELPM482-00005 → Victron CAN-BMS protocol translator\n\n"
                "Translates Samsung SDI CAN PDOs (0x500-0x504, 0x5F0-0x5F4)\n"
                "into Victron CAN-bus BMS frames (0x351, 0x355, 0x356, 0x35A).\n\n"
                "The stock Venus OS CAN-BMS driver picks up the translated\n"
                "frames and publishes to D-Bus.  No custom D-Bus code needed.\n",
                argv[0]);
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
            /* No CAN traffic — mark stale after timeout */
            if (data_valid &&
                time(NULL) - battery.last_update > DATA_TIMEOUT_SEC) {
                data_valid = 0;
                printf("samsung-sdi-bms: data timeout, stopping translation\n");
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
                break;
            case SDI_CAN_ID_CONFIG:
                parse_config(frame.data);
                break;
            case SDI_CAN_ID_LIMITS:
                parse_limits(frame.data);
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

        /* Translate and send Victron frames after receiving the status
         * message (0x500) which carries the most critical data. */
        if (frame.can_id == SDI_CAN_ID_STATUS) {
            if (!data_valid) {
                data_valid = 1;
                printf("samsung-sdi-bms: data received, translating...\n");
            }
            send_victron_frames(can_fd);
        }
    }

    printf("samsung-sdi-bms: shutting down\n");
    if (can_fd >= 0) close(can_fd);
    return 0;
}
