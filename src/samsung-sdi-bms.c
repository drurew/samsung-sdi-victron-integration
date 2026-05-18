/*
 * samsung-sdi-bms.c — Native BMS driver for Samsung SDI ELPM482-00005
 *
 * Listens for PDO broadcasts from the Samsung SDI battery management system
 * and publishes data to the Victron D-Bus. The Samsung SDI BMS broadcasts
 * all data on fixed CAN IDs at 500 kbps; no SDO requests are required.
 *
 * Protocol: Samsung SDI ELPM482-00005 Product Specification Rev 0.2
 * CAN:       CAN 2.0A, 500 kbps, little-endian
 * D-Bus:     com.victronenergy.battery.samsung_sdi
 *
 * Compile:   arm-linux-gnueabihf-gcc -Os -s -std=c99 -D_GNU_SOURCE -Wall -lm -o samsung-sdi-bms samsung-sdi-bms.c
 * Run:       ./samsung-sdi-bms <can-interface>
 */

#define _GNU_SOURCE

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

/* ─── D-Bus wire protocol ────────────────────────────────────────────── */

#define DBUS_SYSTEM_BUS_PATH "/var/run/dbus/system_bus_socket"

static int dbus_fd    = -1;
static int dbus_serial = 0;
static volatile sig_atomic_t running = 1;

/* ─── Samsung SDI CAN protocol ────────────────────────────────────────── */

#define SDI_CAN_BITRATE   500000
#define SDI_CAN_ID_STATUS           0x500
#define SDI_CAN_ID_CONFIG           0x501
#define SDI_CAN_ID_LIMITS           0x502
#define SDI_CAN_ID_CELL_VOLTAGE     0x503
#define SDI_CAN_ID_TEMPERATURE      0x504

/* Data timeout: mark disconnected after 5 seconds without a message */
#define DATA_TIMEOUT_SEC  5

/* Battery service name on D-Bus */
#define DBUS_SERVICE_NAME "com.victronenergy.battery.samsung_sdi"

/* Forward declarations */
static int can_open(const char *ifname);
static int can_recv(int fd, struct can_frame *frame, int timeout_ms);
static int dbus_connect(void);
static int dbus_request_name(const char *name);
static int dbus_emit_property(const char *path, const char *iface,
                              const char *name, int type, const void *val);
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
    double voltage;
    double current;
    double soc;
    double soh;
    double avg_cell_temp;
    double max_cell_temp;
    double min_cell_temp;
    double max_cell_v;
    double min_cell_v;
    double avg_cell_v;
    double avg_tray_v;
    double max_tray_v;
    double min_tray_v;
    double charge_voltage_limit;
    double charge_current_limit;
    double discharge_current_limit;
    double discharge_voltage_limit;
    unsigned char total_trays;
    unsigned char normal_trays;
    unsigned char fault_trays;
    unsigned char alarm_status;
    unsigned int  heartbeat;
    time_t last_update;
} battery_state;

static battery_state battery;
static int           battery_connected = 0;

/* ─── PDO message parsers ─────────────────────────────────────────────── */

static void parse_status(const unsigned char *data) {
    battery.voltage  = u16_le(data, 0) * 0.01;
    battery.current  = s16_le(data, 2) * 1.0;
    battery.soc      = (double)data[4];
    battery.soh      = (double)data[5];
    battery.heartbeat = u16_le(data, 6);

    /* Alarm status: byte 0, bits 0-7 */
    battery.alarm_status = data[0];
}

static void parse_config(const unsigned char *data) {
    battery.total_trays  = data[4];
    battery.normal_trays = data[5];
    battery.fault_trays  = data[6];
}

static void parse_limits(const unsigned char *data) {
    battery.charge_voltage_limit    = u16_le(data, 0) * 0.1;
    battery.charge_current_limit    = u16_le(data, 2) * 0.1;
    battery.discharge_current_limit = u16_le(data, 4) * 0.1;
    battery.discharge_voltage_limit = u16_le(data, 6) * 0.1;
}

static void parse_cell_voltage(const unsigned char *data) {
    /* 0x503: 8 bytes — cell-level voltage summary */
    battery.avg_cell_v  = u16_le(data, 0) * 0.001;
    battery.max_cell_v  = u16_le(data, 2) * 0.001;
    battery.min_cell_v  = u16_le(data, 4) * 0.001;
    battery.avg_tray_v  = u16_le(data, 6) * 0.01;
    /* Note: MaxTrayV and MinTrayV are defined at offsets 8 and 10 in the
     * Samsung SDI specification but exceed the 8-byte CAN 2.0A frame.
     * These values are available on CAN IDs 0x510-0x55C (per-cell data)
     * or 0x505 (extended summary) and are not read by this driver. */
}

static void parse_temperature(const unsigned char *data) {
    battery.avg_cell_temp = (double)s8_le(data, 4);
    battery.max_cell_temp = (double)s8_le(data, 5);
    battery.min_cell_temp = (double)s8_le(data, 6);
}

/* ─── D-Bus publishing ────────────────────────────────────────────────── */

static void publish_all(void) {
    int one = 1;
    int zero = 0;

    if (!battery_connected) {
        dbus_emit_property("/", "", "Connected", 'i', &one);
        battery_connected = 1;
    }

    dbus_emit_property("/", "", "Soc",     'd', &battery.soc);
    dbus_emit_property("/", "", "Capacity", 'd', &(double){150.0});
    dbus_emit_property("/", "", "InstalledCapacity", 'd', &(double){150.0});

    double consumed = 150.0 * (100.0 - battery.soc) / 100.0;
    dbus_emit_property("/", "", "ConsumedAmphours", 'd', &consumed);

    dbus_emit_property("/", "Dc/0", "Voltage",     'd', &battery.voltage);
    dbus_emit_property("/", "Dc/0", "Current",     'd', &battery.current);
    dbus_emit_property("/", "Dc/0", "Power",
                       'd', &(double){battery.voltage * battery.current});
    dbus_emit_property("/", "Dc/0", "Temperature", 'd', &battery.avg_cell_temp);

    dbus_emit_property("/", "Info", "MaxChargeCurrent",
                       'd', &battery.charge_current_limit);
    dbus_emit_property("/", "Info", "MaxDischargeCurrent",
                       'd', &battery.discharge_current_limit);
    dbus_emit_property("/", "Info", "MaxChargeVoltage",
                       'd', &battery.charge_voltage_limit);

    dbus_emit_property("/", "System", "NrOfModulesOnline",
                       'i', &(int){battery.normal_trays});
    dbus_emit_property("/", "System", "NrOfModulesOffline",
                       'i', &(int){battery.fault_trays});
    dbus_emit_property("/", "System", "NrOfModulesBlockingCharge",
                       'i', &zero);
    dbus_emit_property("/", "System", "NrOfModulesBlockingDischarge",
                       'i', &zero);

    /* Alarms per Samsung SDI spec Rev 0.2, Table 8:
     * bit0=Over-Voltage, bit1=Under-Voltage, bit2=Over-Temperature,
     * bit3=Under-Temperature, bit4=Charge Over-Current,
     * bit5=Discharge Over-Current, bit6=FET Over-Temperature,
     * bit7=Tray Voltage Imbalance */
    int alarm;
    alarm = (battery.alarm_status & 0x01) ? 2 : 0; /* High Voltage */
    dbus_emit_property("/", "Alarms", "HighVoltage", 'i', &alarm);
    alarm = (battery.alarm_status & 0x02) ? 2 : 0; /* Low Voltage */
    dbus_emit_property("/", "Alarms", "LowVoltage", 'i', &alarm);
    alarm = (battery.alarm_status & 0x04) ? 2 : 0; /* High Temperature */
    dbus_emit_property("/", "Alarms", "HighTemperature", 'i', &alarm);
    alarm = (battery.alarm_status & 0x08) ? 2 : 0; /* Low Temperature */
    dbus_emit_property("/", "Alarms", "LowTemperature", 'i', &alarm);
    alarm = (battery.alarm_status & 0x10) ? 2 : 0; /* High Charge Current */
    dbus_emit_property("/", "Alarms", "HighChargeCurrent", 'i', &alarm);
    alarm = (battery.alarm_status & 0x20) ? 2 : 0; /* High Discharge Current */
    dbus_emit_property("/", "Alarms", "HighDischargeCurrent", 'i', &alarm);
    alarm = (battery.alarm_status & 0x40) ? 2 : 0; /* FET Over-Temp */
    dbus_emit_property("/", "Alarms", "InternalFailure", 'i', &alarm);
    alarm = (battery.alarm_status & 0x80) ? 2 : 0; /* Tray Voltage Imbalance */
    dbus_emit_property("/", "Alarms", "CellImbalance", 'i', &alarm);
}

/* ─── CAN open/read ───────────────────────────────────────────────────── */

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
    return read(fd, frame, sizeof(*frame));
}

/* ─── D-Bus low-level wire protocol ───────────────────────────────────── */

static int dbus_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket unix"); return -1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, DBUS_SYSTEM_BUS_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect dbus"); close(fd); return -1;
    }

    /* Read server greeting */
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0 || strncmp(buf, "OK ", 3) != 0) {
        fprintf(stderr, "D-Bus greeting error\n"); close(fd); return -1;
    }

    /* Send AUTH EXTERNAL */
    char auth[128];
    int uid = getuid();
    auth[0] = '\0';
    int len = 1 + snprintf(auth + 1, sizeof(auth) - 1,
                           "AUTH EXTERNAL %x\r\n", uid);
    if (write(fd, auth, len) < 0) { close(fd); return -1; }

    /* Read AUTH response */
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0 || strncmp(buf, "OK ", 3) != 0) {
        fprintf(stderr, "D-Bus auth failed\n"); close(fd); return -1;
    }

    write(fd, "BEGIN\r\n", 7);
    return fd;
}

static int dbus_msg_header(unsigned char *buf, int serial,
                           const char *dest, const char *path,
                           const char *iface, const char *member) {
    memset(buf, 0, 256);
    buf[0] = 'l'; buf[1] = 0; buf[2] = 0; buf[3] = 1;
    int pos = 12;

    /* PATH */
    buf[pos++] = 1; buf[pos++] = 1; buf[pos++] = 'o'; buf[pos] = 0;
    int plen = (int)strlen(path);
    buf[pos + 1] = plen;
    memcpy(buf + pos + 5, path, plen);
    pos += 5 + plen;
    while ((pos - 12) & 7) buf[pos++] = 0;

    /* DESTINATION */
    if (dest) {
        buf[pos++] = 6; buf[pos++] = 1; buf[pos++] = 's'; buf[pos] = 0;
        int dlen = (int)strlen(dest);
        buf[pos + 1] = dlen;
        memcpy(buf + pos + 5, dest, dlen);
        pos += 5 + dlen;
        while ((pos - 12) & 7) buf[pos++] = 0;
    }

    /* INTERFACE */
    buf[pos++] = 2; buf[pos++] = 1; buf[pos++] = 's'; buf[pos] = 0;
    int ilen = (int)strlen(iface);
    buf[pos + 1] = ilen;
    memcpy(buf + pos + 5, iface, ilen);
    pos += 5 + ilen;
    while ((pos - 12) & 7) buf[pos++] = 0;

    /* MEMBER */
    buf[pos++] = 3; buf[pos++] = 1; buf[pos++] = 's'; buf[pos] = 0;
    int mlen = (int)strlen(member);
    buf[pos + 1] = mlen;
    memcpy(buf + pos + 5, member, mlen);
    pos += 5 + mlen;
    while ((pos - 12) & 7) buf[pos++] = 0;

    /* Header length at byte 4, serial at byte 8 */
    int hlen = pos - 12;
    buf[4] = hlen & 0xFF; buf[5] = (hlen >> 8) & 0xFF;
    buf[6] = (hlen >> 16) & 0xFF; buf[7] = (hlen >> 24) & 0xFF;
    buf[8] = serial & 0xFF; buf[9] = (serial >> 8) & 0xFF;
    buf[10] = (serial >> 16) & 0xFF; buf[11] = (serial >> 24) & 0xFF;

    /* 8-byte padding at end of data */
    int total = pos + 8;
    memset(buf + pos, 0, 8);
    return total;
}

static int dbus_request_name(const char *name) {
    unsigned char buf[512];
    int serial = ++dbus_serial;
    int len = dbus_msg_header(buf, serial, "org.freedesktop.DBus",
                              "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "RequestName");
    buf[1] = 0x01; /* method call */

    int pos = len - 8;
    int nlen = (int)strlen(name);
    buf[pos++] = nlen;
    memcpy(buf + pos, name, nlen); pos += nlen;
    memset(buf + pos, 0, 8); pos += 8; /* flags=0 */

    int body_len = pos - (len - 8);
    buf[4] = body_len & 0xFF; buf[5] = (body_len >> 8) & 0xFF;
    buf[6] = (body_len >> 16) & 0xFF; buf[7] = (body_len >> 24) & 0xFF;
    return write(dbus_fd, buf, pos);
}

static int dbus_emit_property(const char *path, const char *iface,
                              const char *name, int type, const void *val) {
    unsigned char buf[1024];
    int serial = ++dbus_serial;
    int hpos = dbus_msg_header(buf, serial, NULL, path,
                               "org.freedesktop.DBus.Properties",
                               "PropertiesChanged");
    buf[1] = 0x04; /* signal */
    int pos = hpos - 8;

    /* Interface name string */
    int ilen = (int)strlen(iface);
    buf[pos++] = ilen;
    memcpy(buf + pos, iface, ilen); pos += ilen;
    buf[pos++] = 0;
    while (pos & 3) buf[pos++] = 0;

    /* Array of dict entries: length placeholder */
    int arr_len_pos = pos;
    pos += 4;
    while ((pos - (hpos - 8)) & 7) buf[pos++] = 0;

    /* Dict entry: key string */
    int klen = (int)strlen(name);
    buf[pos++] = klen;
    memcpy(buf + pos, name, klen); pos += klen;
    buf[pos++] = 0;
    while (pos & 3) buf[pos++] = 0;

    /* Variant: signature */
    buf[pos++] = 1; buf[pos++] = (unsigned char)type; buf[pos++] = 0;

    /* Value */
    switch (type) {
    case 'd':
        memcpy(buf + pos, val, 8); pos += 8;
        break;
    case 'i':
        buf[pos++] = ((const int *)val)[0] & 0xFF;
        buf[pos++] = (((const int *)val)[0] >> 8) & 0xFF;
        buf[pos++] = (((const int *)val)[0] >> 16) & 0xFF;
        buf[pos++] = (((const int *)val)[0] >> 24) & 0xFF;
        break;
    }

    /* Fill array length */
    int arr_end = pos;
    int arr_data_len = arr_end - (arr_len_pos + 4);
    buf[arr_len_pos] = arr_data_len & 0xFF;
    buf[arr_len_pos + 1] = (arr_data_len >> 8) & 0xFF;
    buf[arr_len_pos + 2] = (arr_data_len >> 16) & 0xFF;
    buf[arr_len_pos + 3] = (arr_data_len >> 24) & 0xFF;

    /* Empty invalidated array */
    memset(buf + pos, 0, 4); pos += 4;

    /* Fix body length */
    int body_len = pos - (hpos - 8);
    buf[4] = body_len & 0xFF; buf[5] = (body_len >> 8) & 0xFF;
    buf[6] = (body_len >> 16) & 0xFF; buf[7] = (body_len >> 24) & 0xFF;

    return write(dbus_fd, buf, pos);
}

/* ─── Signal handler ──────────────────────────────────────────────────── */

static void sig_handler(int sig) { (void)sig; running = 0; }

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <can-interface>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int can_fd = can_open(argv[1]);
    if (can_fd < 0) return 1;
    printf("samsung-sdi-bms: CAN connected on %s\n", argv[1]);

    dbus_fd = dbus_connect();
    if (dbus_fd < 0) {
        fprintf(stderr, "D-Bus connect failed\n");
        return 1;
    }
    printf("samsung-sdi-bms: D-Bus connected\n");

    dbus_request_name(DBUS_SERVICE_NAME);
    printf("samsung-sdi-bms: registered %s\n", DBUS_SERVICE_NAME);

    /* Publish static paths */
    dbus_emit_property("/", "", "ProductId", 'i',
                       &(int){0x0001});
    dbus_emit_property("/", "", "ProductName", 's',
                       "Samsung SDI ELPM482-00005");
    dbus_emit_property("/", "", "HardwareVersion", 's',
                       "ELPM482-00005");
    dbus_emit_property("/", "", "FirmwareVersion", 's',
                       "Rev 0.2");
    dbus_emit_property("/", "Mgmt", "ProcessName", 's',
                       "samsung-sdi-bms");
    dbus_emit_property("/", "Mgmt", "ProcessVersion", 's',
                       "1.0.0");
    dbus_emit_property("/", "Mgmt", "Connection", 's',
                       "CAN PDO 500kbps");
    dbus_emit_property("/", "System", "NrOfCellsPerBattery", 'i',
                       &(int){26});
    dbus_emit_property("/", "", "DeviceInstance", 'i', &(int){1});

    printf("samsung-sdi-bms: running, listening for PDOs\n");

    memset(&battery, 0, sizeof(battery));

    while (running) {
        struct can_frame frame;
        int ret = can_recv(can_fd, &frame, 1000);
        if (ret <= 0) {
            /* Check timeout */
            if (battery_connected &&
                time(NULL) - battery.last_update > DATA_TIMEOUT_SEC) {
                battery_connected = 0;
                dbus_emit_property("/", "", "Connected", 'i',
                                   &(int){0});
            }
            continue;
        }

        /* Only process known Samsung SDI CAN IDs */
        switch (frame.can_id) {
        case SDI_CAN_ID_STATUS:       parse_status(frame.data);       break;
        case SDI_CAN_ID_CONFIG:       parse_config(frame.data);       break;
        case SDI_CAN_ID_LIMITS:       parse_limits(frame.data);       break;
        case SDI_CAN_ID_CELL_VOLTAGE: parse_cell_voltage(frame.data); break;
        case SDI_CAN_ID_TEMPERATURE:  parse_temperature(frame.data);  break;
        default:
            continue;
        }

        battery.last_update = time(NULL);

        /* Publish after receiving the status message (0x500) which carries
         * the most critical data. Limits and cell data are published on
         * their respective messages as well, but we batch all on 0x500. */
        if (frame.can_id == SDI_CAN_ID_STATUS) {
            publish_all();
        }
    }

    printf("samsung-sdi-bms: shutting down\n");
    if (can_fd >= 0) close(can_fd);
    if (dbus_fd >= 0) close(dbus_fd);
    return 0;
}
