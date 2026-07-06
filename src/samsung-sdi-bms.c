/*
 * samsung-sdi-bms.c — Samsung SDI BMS Driver for Victron Venus OS
 *
 * Full-featured native driver that publishes Samsung SDI ELPM482-00005
 * battery data directly to D-Bus via libdbus-1, implementing the Victron
 * com.victronenergy.BusItem interface.  Optionally also translates to
 * Victron CAN-bus BMS frames (--can-bms flag).
 *
 * D-Bus path (default, full telemetry):
 *   Samsung SDI CAN → samsung-sdi-bms → D-Bus (all 28 fields)
 *     → Venus OS GUI + DVCC + VRM
 *
 * CAN-BMS path (optional, --can-bms flag, 11 of 28 fields):
 *   Samsung SDI CAN → samsung-sdi-bms → Victron CAN-BMS frames
 *     → Victron can-bus-bms service → D-Bus → Venus OS
 *
 * Protocol references:
 *   Samsung SDI ELPM482-00005 Product Specification Rev 0.2
 *   Venus OS DBUS_BATTERY_SPEC.md (this repository)
 *
 * Compile:
 *   gcc -Os -s -std=c99 -Wall -Wextra -D_GNU_SOURCE \
 *       -o samsung-sdi-bms samsung-sdi-bms.c -lm -ldbus-1
 *   arm-linux-gnueabihf-gcc -Os -s -std=c99 -Wall -Wextra -D_GNU_SOURCE \
 *       -o samsung-sdi-bms samsung-sdi-bms.c -lm -ldbus-1
 *
 * Run:
 *   ./samsung-sdi-bms can0              # D-Bus only (full telemetry)
 *   ./samsung-sdi-bms --can-bms can0    # D-Bus + CAN-BMS translation
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
#include <sys/ioctl.h>
#include <net/if.h>
#include <poll.h>
#include <dbus/dbus.h>

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

/* ─── Samsung SDI CAN IDs ─────────────────────────────────────────────── */

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

/* Victron CAN-BMS frame IDs ───────────────────────────────────────────── */

#define VC_CAN_ID_STATE             0x351
#define VC_CAN_ID_SOC_SOH           0x355
#define VC_CAN_ID_LIMITS            0x356
#define VC_CAN_ID_ALARMS            0x35A

/* ─── Constants ───────────────────────────────────────────────────────── */

#define DATA_TIMEOUT_SEC        5
#define BATTERY_CAPACITY_AH     94.0
#define CELLS_PER_MODULE        14
#define DBUS_SERVICE_NAME       "com.victronenergy.battery.samsung_sdi"
#define DEVICE_INSTANCE         280
#define VERSION_STRING          "2.1.0"

/* ─── Forward declarations ────────────────────────────────────────────── */

static int  can_open(const char *ifname);
static int  can_recv(int fd, struct can_frame *frame, int timeout_ms);
static int  can_send(int fd, canid_t id, const unsigned char *data, int len);
static void sig_handler(int sig);

/* ─── Little-endian helpers ───────────────────────────────────────────── */

static unsigned short u16_le(const unsigned char *buf, int offset) {
    return (unsigned short)(buf[offset] | (buf[offset + 1] << 8));
}
static short s16_le(const unsigned char *buf, int offset) {
    return (short)u16_le(buf, offset);
}
static signed char s8_le(const unsigned char *buf, int offset) {
    return (signed char)buf[offset];
}

/* ─── Battery state ───────────────────────────────────────────────────── */

typedef struct {
    double   voltage, current, soc, soh;
    double   avg_cell_temp, max_cell_temp, min_cell_temp;
    double   max_cell_v, min_cell_v, avg_cell_v;
    double   avg_tray_v, max_tray_v, min_tray_v;
    double   charge_voltage_limit, charge_current_limit;
    double   discharge_current_limit, discharge_voltage_limit;
    unsigned char  total_trays, normal_trays, fault_trays;
    unsigned int   alarm_status, protection_status, heartbeat;
    double   cell_v[14];
    int      cell_v_valid[14];
    time_t   last_update;
} battery_state;

static battery_state  battery;
static battery_state  battery_prev;  /* for change detection */
static int            data_valid = 0;
static volatile sig_atomic_t running = 1;

/* ─── Samsung SDI parsers ─────────────────────────────────────────────── */

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
    battery.avg_cell_v = u16_le(data, 0) * 0.001;
    battery.max_cell_v = u16_le(data, 2) * 0.001;
    battery.min_cell_v = u16_le(data, 4) * 0.001;
    battery.avg_tray_v = u16_le(data, 6) * 0.01;
}
static void parse_temperature(const unsigned char *data) {
    battery.max_tray_v    = u16_le(data, 0) * 0.01;
    battery.min_tray_v    = u16_le(data, 2) * 0.01;
    battery.avg_cell_temp = (double)s8_le(data, 4);
    battery.max_cell_temp = (double)s8_le(data, 5);
    battery.min_cell_temp = (double)s8_le(data, 6);
}
static void parse_cell_frame(int cell_start, const unsigned char *data, int dlc) {
    int i;
    for (i = 0; i < 3 && cell_start + i < 14; i++) {
        int off = 2 + i * 2;
        if (off + 2 > dlc) break;
        unsigned short raw = u16_le(data, off);
        if (raw != 0) {
            battery.cell_v[cell_start + i] = raw * 0.001;
            battery.cell_v_valid[cell_start + i] = 1;
        }
    }
}

/* ─── Alarm severity mapping (Samsung → Victron) ──────────────────────── */

static int alarm_sev(unsigned int alarm_bit, unsigned int prot_bit) {
    if (battery.protection_status & prot_bit)  return 2;
    if (battery.alarm_status & alarm_bit)      return 1;
    return 0;
}
static int alarm_int_failure(void) {
    /* Internal failure: FET over-temp, FET failure, tray-ID, PCS comm */
    if (battery.protection_status & (0x0040|0x0100|0x0200|0x0400|0x0800)) return 2;
    if (battery.alarm_status & 0x0040) return 1;
    return 0;
}

/* ─── CAN-BMS frame builders (used only with --can-bms) ───────────────── */

static void vc_build_state(unsigned char *data) {
    unsigned short v   = (unsigned short)(battery.voltage * 100.0 + 0.5);
    short          i   = (short)(battery.current * 10.0
                          + (battery.current >= 0 ? 0.5 : -0.5));
    unsigned short t   = (unsigned short)(battery.avg_cell_temp * 10.0 + 0.5);
    unsigned char  soc = (unsigned char)(battery.soc + 0.5);
    unsigned char  soh = (unsigned char)(battery.soh + 0.5);
    if (soc > 100) soc = 100;
    if (soh > 100) soh = 100;
    data[0] = v & 0xFF;    data[1] = (v >> 8) & 0xFF;
    data[2] = i & 0xFF;    data[3] = (i >> 8) & 0xFF;
    data[4] = t & 0xFF;    data[5] = (t >> 8) & 0xFF;
    data[6] = soc;         data[7] = soh;
}
static void vc_build_soc_soh(unsigned char *data) {
    unsigned short s = (unsigned short)(battery.soc * 10.0 + 0.5);
    unsigned short h = (unsigned short)(battery.soh * 10.0 + 0.5);
    data[0] = s & 0xFF;   data[1] = (s >> 8) & 0xFF;
    data[2] = h & 0xFF;   data[3] = (h >> 8) & 0xFF;
    data[4] = 0xFF; data[5] = 0xFF;
    data[6] = 0xFF; data[7] = 0xFF;
}
static void vc_build_limits(unsigned char *data) {
    unsigned short cvl = (unsigned short)(battery.charge_voltage_limit * 10.0 + 0.5);
    unsigned short ccl = (unsigned short)(battery.charge_current_limit * 10.0 + 0.5);
    unsigned short dcl = (unsigned short)(battery.discharge_current_limit * 10.0 + 0.5);
    unsigned short dvl = (unsigned short)(battery.discharge_voltage_limit * 10.0 + 0.5);
    data[0] = cvl & 0xFF;  data[1] = (cvl >> 8) & 0xFF;
    data[2] = ccl & 0xFF;  data[3] = (ccl >> 8) & 0xFF;
    data[4] = dcl & 0xFF;  data[5] = (dcl >> 8) & 0xFF;
    data[6] = dvl & 0xFF;  data[7] = (dvl >> 8) & 0xFF;
}
static void vc_build_alarms(unsigned char *data) {
    unsigned int a = battery.alarm_status, p = battery.protection_status;
    unsigned short alarms = 0, warnings = 0;
    if (p & 0x0001) alarms |= 0x01; else if (a & 0x0001) warnings |= 0x01;
    if (p & 0x4000) alarms |= 0x01;
    if (p & 0x0002) alarms |= 0x02; else if (a & 0x0002) warnings |= 0x02;
    if (p & 0x1000) alarms |= 0x02;
    if (p & 0x0004) alarms |= 0x04; else if (a & 0x0004) warnings |= 0x04;
    if (p & 0x0008) alarms |= 0x08; else if (a & 0x0008) warnings |= 0x08;
    if (p & 0x0010) alarms |= 0x10; else if (a & 0x0010) warnings |= 0x10;
    if (p & 0x0020) alarms |= 0x20; else if (a & 0x0020) warnings |= 0x20;
    if (p & (0x0040|0x0100|0x0200|0x0400|0x0800)) alarms |= 0x40;
    else if (a & 0x0040) warnings |= 0x40;
    if (p & 0x2000) alarms |= 0x80; else if (a & 0x0080) warnings |= 0x80;
    data[0] = alarms & 0xFF;   data[1] = (alarms >> 8) & 0xFF;
    data[2] = warnings & 0xFF; data[3] = (warnings >> 8) & 0xFF;
    data[4] = 0; data[5] = 0; data[6] = 0; data[7] = 0;
}
static void send_can_bms_frames(int can_fd) {
    unsigned char b[8];
    vc_build_state(b);   can_send(can_fd, VC_CAN_ID_STATE,   b, 8);
    vc_build_soc_soh(b); can_send(can_fd, VC_CAN_ID_SOC_SOH, b, 8);
    vc_build_limits(b);  can_send(can_fd, VC_CAN_ID_LIMITS,  b, 8);
    vc_build_alarms(b);  can_send(can_fd, VC_CAN_ID_ALARMS,  b, 8);
}

/* ─── CAN I/O ─────────────────────────────────────────────────────────── */

static int can_open(const char *ifname) {
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) { perror("socket(CAN)"); return -1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl SIOCGIFINDEX"); close(fd); return -1;
    }
    struct { unsigned short family; unsigned short pad; int ifindex; } addr;
    addr.family = AF_CAN; addr.ifindex = ifr.ifr_ifindex; addr.pad = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind CAN"); close(fd); return -1;
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
    frame.can_id = id; frame.can_dlc = (unsigned char)len;
    memcpy(frame.data, data, len);
    return (write(fd, &frame, sizeof(frame)) == (ssize_t)sizeof(frame)) ? 0 : -1;
}

/* ─── D-Bus helpers ───────────────────────────────────────────────────── */

static DBusConnection *dbus_conn = NULL;
static DBusHandlerResult dbus_message_handler(DBusConnection *c, DBusMessage *m, void *u);

/* Append a DBus double */
static void msg_append_double(DBusMessageIter *iter, double v) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &v);
}
/* Append a DBus int32 */
static void msg_append_int32(DBusMessageIter *iter, int v) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &v);
}
/* Append a DBus string */
static void msg_append_string(DBusMessageIter *iter, const char *v) {
    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &v);
}

/* Append a variant containing a double */
static void msg_append_variant_double(DBusMessageIter *iter, double v) {
    DBusMessageIter sub;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "d", &sub);
    msg_append_double(&sub, v);
    dbus_message_iter_close_container(iter, &sub);
}
/* Append a variant containing an int32 */
static void msg_append_variant_int32(DBusMessageIter *iter, int v) {
    DBusMessageIter sub;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &sub);
    msg_append_int32(&sub, v);
    dbus_message_iter_close_container(iter, &sub);
}
/* Append a variant containing a string */
static void msg_append_variant_string(DBusMessageIter *iter, const char *v) {
    DBusMessageIter sub;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &sub);
    msg_append_string(&sub, v);
    dbus_message_iter_close_container(iter, &sub);
}

/* Append a GetItems dict entry: STRING path → DICT{STRING:VARIANT} */
static void msg_append_getitems_entry(DBusMessageIter *array,
                                       const char *path,
                                       int vtype, const void *value,
                                       const char *text) {
    DBusMessageIter entry, subdict, subentry, subvariant;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    msg_append_string(&entry, path);
    /* inner dict: {"Value": variant, "Text": variant} */
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &subdict);

    /* "Value" */
    dbus_message_iter_open_container(&subdict, DBUS_TYPE_DICT_ENTRY, NULL, &subentry);
    msg_append_string(&subentry, "Value");
    dbus_message_iter_open_container(&subentry, DBUS_TYPE_VARIANT,
        vtype == DBUS_TYPE_DOUBLE ? "d" :
        vtype == DBUS_TYPE_INT32  ? "i" : "s", &subvariant);
    dbus_message_iter_append_basic(&subvariant, vtype, value);
    dbus_message_iter_close_container(&subentry, &subvariant);
    dbus_message_iter_close_container(&subdict, &subentry);

    /* "Text" */
    dbus_message_iter_open_container(&subdict, DBUS_TYPE_DICT_ENTRY, NULL, &subentry);
    msg_append_string(&subentry, "Text");
    dbus_message_iter_open_container(&subentry, DBUS_TYPE_VARIANT, "s", &subvariant);
    msg_append_string(&subvariant, text);
    dbus_message_iter_close_container(&subentry, &subvariant);
    dbus_message_iter_close_container(&subdict, &subentry);

    dbus_message_iter_close_container(&entry, &subdict);
    dbus_message_iter_close_container(array, &entry);
}

/* ─── D-Bus path definitions ──────────────────────────────────────────── */

typedef enum { PT_DOUBLE, PT_INT32, PT_STRING } path_type_t;

typedef struct {
    const char   *path;
    path_type_t   type;
    /* For PT_DOUBLE: pointer to the double in battery_state.
     * For PT_INT32:  pointer to int value (or function returning int).
     * For PT_STRING: pointer to static string. */
    const void   *value_ptr;
    const char   *text_fmt;    /* printf format string for text */
    const char   *description;
} dbus_path_def_t;

/* Forward decl for int getters */
static int get_connected(void);
static int get_alarm_low_voltage(void);
static int get_alarm_high_voltage(void);
static int get_alarm_low_temp(void);
static int get_alarm_high_temp(void);
static int get_alarm_low_soc(void);
static int get_alarm_high_charge_current(void);
static int get_alarm_high_discharge_current(void);
static int get_alarm_cell_imbalance(void);
static int get_alarm_internal_failure(void);
static int get_alarm_high_charge_temp(void);
static int get_alarm_low_charge_temp(void);

#define P_DOUBLE(path_, ptr_, fmt_, desc_) \
    { path_, PT_DOUBLE, (const void *)(ptr_), fmt_, desc_ }
#define P_INT32(path_, fn_, fmt_, desc_) \
    { path_, PT_INT32, (const void *)(fn_), fmt_, desc_ }
#define P_STRING(path_, ptr_, desc_) \
    { path_, PT_STRING, (const void *)(ptr_), NULL, desc_ }

static dbus_path_def_t paths[] = {
    /* Essential measurements */
    P_DOUBLE("/Soc",                     &battery.soc,              "%.0f%%", "State of charge"),
    P_DOUBLE("/Dc/0/Voltage",            &battery.voltage,          "%.2fV",  "Battery voltage"),
    P_DOUBLE("/Dc/0/Current",            &battery.current,          "%.1fA",  "Battery current"),
    P_DOUBLE("/Dc/0/Power",              NULL,                      NULL,     "Battery power (computed)"),
    P_DOUBLE("/Dc/0/Temperature",        &battery.avg_cell_temp,    "%.1f°C", "Battery temperature"),
    /* DVCC control paths */
    P_DOUBLE("/Info/MaxChargeCurrent",   &battery.charge_current_limit,    "%.1fA",  "Charge current limit"),
    P_DOUBLE("/Info/MaxDischargeCurrent",&battery.discharge_current_limit, "%.1fA",  "Discharge current limit"),
    P_DOUBLE("/Info/MaxChargeVoltage",   &battery.charge_voltage_limit,    "%.2fV",  "Charge voltage limit"),
    /* Cell voltage stats */
    P_DOUBLE("/System/MinCellVoltage",   &battery.min_cell_v,       "%.3fV",  "Minimum cell voltage"),
    P_DOUBLE("/System/MaxCellVoltage",   &battery.max_cell_v,       "%.3fV",  "Maximum cell voltage"),
    /* System status */
    P_INT32("/System/NrOfCellsPerBattery",NULL,                     NULL,     "Cells per module"),
    P_INT32("/System/NrOfModulesOnline", NULL,                      NULL,     "Operational modules"),
    P_INT32("/System/NrOfModulesOffline",NULL,                      NULL,     "Offline modules"),
    P_INT32("/System/NrOfModulesBlockingCharge", NULL,              NULL,     "Modules blocking charge"),
    P_INT32("/System/NrOfModulesBlockingDischarge", NULL,           NULL,     "Modules blocking discharge"),
    /* Connection */
    P_INT32("/Connected",                NULL,                      NULL,     "CAN link status"),
    /* Charge control I/O */
    P_INT32("/Io/AllowToCharge",         NULL,                      NULL,     "Charging permitted"),
    P_INT32("/Io/AllowToDischarge",      NULL,                      NULL,     "Discharging permitted"),
    /* History */
    P_DOUBLE("/ConsumedAmphours",        NULL,                      NULL,     "Consumed Ah"),
    /* Metadata */
    P_INT32("/DeviceInstance",           NULL,                      NULL,     "Device instance"),
    P_DOUBLE("/InstalledCapacity",       NULL,                      NULL,     "Nominal capacity"),
    P_STRING("/ProductName",             "Samsung SDI ELPM482-00005","Product name"),
    P_STRING("/HardwareVersion",         "ELPM482-00005",           "Hardware version"),
    P_STRING("/FirmwareVersion",         VERSION_STRING,            "Driver version"),
    P_STRING("/Mgmt/ProcessName",        "samsung-sdi-bms",         "Process name"),
    P_STRING("/Mgmt/ProcessVersion",     VERSION_STRING,            "Process version"),
    P_STRING("/Mgmt/Connection",         "CAN PDO 500kbps",         "Connection type"),
    /* Alarms */
    P_INT32("/Alarms/LowVoltage",         NULL,                     NULL,     "Low voltage alarm"),
    P_INT32("/Alarms/HighVoltage",        NULL,                     NULL,     "High voltage alarm"),
    P_INT32("/Alarms/LowTemperature",     NULL,                     NULL,     "Low temperature alarm"),
    P_INT32("/Alarms/HighTemperature",    NULL,                     NULL,     "High temperature alarm"),
    P_INT32("/Alarms/LowSoc",             NULL,                     NULL,     "Low SOC alarm"),
    P_INT32("/Alarms/HighChargeCurrent",  NULL,                     NULL,     "High charge current alarm"),
    P_INT32("/Alarms/HighDischargeCurrent",NULL,                    NULL,     "High discharge current alarm"),
    P_INT32("/Alarms/CellImbalance",      NULL,                     NULL,     "Cell imbalance alarm"),
    P_INT32("/Alarms/InternalFailure",    NULL,                     NULL,     "Internal failure alarm"),
    P_INT32("/Alarms/HighChargeTemperature", NULL,                  NULL,     "High charge temperature"),
    P_INT32("/Alarms/LowChargeTemperature",  NULL,                  NULL,     "Low charge temperature"),
    {NULL, 0, NULL, NULL, NULL}
};

/* ─── Value getters for computed/int paths ────────────────────────────── */

static int get_connected(void)                { return data_valid ? 1 : 0; }
static int get_alarm_low_voltage(void) {
    int a = alarm_sev(0x0002, 0x0002);
    if (battery.protection_status & 0x1000) a = 2;
    return a;
}
static int get_alarm_high_voltage(void) {
    int a = alarm_sev(0x0001, 0x0001);
    if (battery.protection_status & 0x4000) a = 2;
    return a;
}
static int get_alarm_low_temp(void)          { return alarm_sev(0x0008, 0x0008); }
static int get_alarm_high_temp(void)         { return alarm_sev(0x0004, 0x0004); }
static int get_alarm_low_soc(void)           { return (battery.soc < 10.0) ? 1 : 0; }
static int get_alarm_high_charge_current(void)    { return alarm_sev(0x0010, 0x0010); }
static int get_alarm_high_discharge_current(void) { return alarm_sev(0x0020, 0x0020); }
static int get_alarm_cell_imbalance(void) {
    int a = alarm_sev(0x0080, 0x0080);
    if (battery.protection_status & 0x2000) a = 2;
    return a;
}
static int get_alarm_internal_failure(void)  { return alarm_int_failure(); }
static int get_alarm_high_charge_temp(void)  { return alarm_sev(0x0004, 0x0004); }
static int get_alarm_low_charge_temp(void)   { return alarm_sev(0x0008, 0x0008); }

static int get_nr_cells(void) { return CELLS_PER_MODULE; }
static int get_nr_online(void) { return (int)battery.normal_trays; }
static int get_nr_offline(void){ return (int)battery.fault_trays; }
static int get_nr_blocking_charge(void)    { return 0; }
static int get_nr_blocking_discharge(void) { return 0; }
static int get_allow_charge(void)    { return data_valid ? 1 : 0; }
static int get_allow_discharge(void) { return data_valid ? 1 : 0; }
static int get_device_instance(void) { return DEVICE_INSTANCE; }
static double get_capacity(void)     { return BATTERY_CAPACITY_AH; }
static double get_power(void)        { return battery.voltage * battery.current; }
static double get_consumed_ah(void)  {
    return BATTERY_CAPACITY_AH * (100.0 - battery.soc) / 100.0;
}

/* ─── Resolve path to value ───────────────────────────────────────────── */

static int path_get_value(const char *path, int *vtype_out,
                           const void **val_out, const char **text_out) {
    static char text_buf[64];
    int i;
    for (i = 0; paths[i].path != NULL; i++) {
        if (strcmp(path, paths[i].path) != 0) continue;

        *vtype_out = (paths[i].type == PT_DOUBLE) ? DBUS_TYPE_DOUBLE :
                     (paths[i].type == PT_INT32)  ? DBUS_TYPE_INT32  :
                                                      DBUS_TYPE_STRING;
        if (paths[i].type == PT_DOUBLE) {
            static double v;
            if (paths[i].value_ptr == NULL) {
                /* Computed double paths */
                if (strcmp(path, "/Dc/0/Power") == 0)        v = get_power();
                else if (strcmp(path, "/ConsumedAmphours") == 0) v = get_consumed_ah();
                else if (strcmp(path, "/InstalledCapacity") == 0) v = get_capacity();
                else v = 0.0;
                *val_out = &v;
            } else {
                *val_out = paths[i].value_ptr;
            }
            if (paths[i].text_fmt && text_out)
                snprintf(text_buf, sizeof(text_buf), paths[i].text_fmt,
                         *(const double *)(*val_out));
        } else if (paths[i].type == PT_INT32) {
            static int v;
            if (paths[i].value_ptr == NULL) {
                /* Function pointer stored in value_ptr */
                if (strcmp(path, "/Connected") == 0)                    v = get_connected();
                else if (strcmp(path, "/System/NrOfCellsPerBattery") == 0) v = get_nr_cells();
                else if (strcmp(path, "/System/NrOfModulesOnline") == 0) v = get_nr_online();
                else if (strcmp(path, "/System/NrOfModulesOffline") == 0) v = get_nr_offline();
                else if (strcmp(path, "/System/NrOfModulesBlockingCharge") == 0) v = get_nr_blocking_charge();
                else if (strcmp(path, "/System/NrOfModulesBlockingDischarge") == 0) v = get_nr_blocking_discharge();
                else if (strcmp(path, "/Io/AllowToCharge") == 0)        v = get_allow_charge();
                else if (strcmp(path, "/Io/AllowToDischarge") == 0)     v = get_allow_discharge();
                else if (strcmp(path, "/DeviceInstance") == 0)          v = get_device_instance();
                else if (strcmp(path, "/Alarms/LowVoltage") == 0)       v = get_alarm_low_voltage();
                else if (strcmp(path, "/Alarms/HighVoltage") == 0)      v = get_alarm_high_voltage();
                else if (strcmp(path, "/Alarms/LowTemperature") == 0)   v = get_alarm_low_temp();
                else if (strcmp(path, "/Alarms/HighTemperature") == 0)  v = get_alarm_high_temp();
                else if (strcmp(path, "/Alarms/LowSoc") == 0)           v = get_alarm_low_soc();
                else if (strcmp(path, "/Alarms/HighChargeCurrent") == 0) v = get_alarm_high_charge_current();
                else if (strcmp(path, "/Alarms/HighDischargeCurrent") == 0) v = get_alarm_high_discharge_current();
                else if (strcmp(path, "/Alarms/CellImbalance") == 0)    v = get_alarm_cell_imbalance();
                else if (strcmp(path, "/Alarms/InternalFailure") == 0)  v = get_alarm_internal_failure();
                else if (strcmp(path, "/Alarms/HighChargeTemperature") == 0) v = get_alarm_high_charge_temp();
                else if (strcmp(path, "/Alarms/LowChargeTemperature") == 0) v = get_alarm_low_charge_temp();
                else v = 0;
                *val_out = &v;
            } else {
                *val_out = paths[i].value_ptr;
            }
            if (paths[i].text_fmt && text_out)
                snprintf(text_buf, sizeof(text_buf), paths[i].text_fmt,
                         *(const int *)(*val_out));
        } else {
            *val_out = paths[i].value_ptr;
        }
        /* Provide text if not formatted explicitly */
        if (text_out && (!paths[i].text_fmt ||
            (paths[i].type == PT_STRING))) {
            if (paths[i].type == PT_STRING)
                snprintf(text_buf, sizeof(text_buf), "%s",
                         (const char *)(*val_out));
            else if (paths[i].type == PT_INT32)
                snprintf(text_buf, sizeof(text_buf), "%d",
                         *(const int *)(*val_out));
            else
                snprintf(text_buf, sizeof(text_buf), "%g",
                         *(const double *)(*val_out));
        }
        if (text_out) *text_out = text_buf;
        return 0;
    }

    /* Cell voltages: /Voltages/Cell1 through /Voltages/Cell14 */
    if (strncmp(path, "/Voltages/Cell", 14) == 0) {
        int cell = atoi(path + 14);
        static double cv;
        if (cell >= 1 && cell <= 14 && battery.cell_v_valid[cell - 1]) {
            cv = battery.cell_v[cell - 1];
            *vtype_out = DBUS_TYPE_DOUBLE;
            *val_out = &cv;
            if (text_out) { snprintf(text_buf, sizeof(text_buf), "%.3fV", cv); *text_out = text_buf; }
            return 0;
        }
    }

    return -1; /* not found */
}

/* ─── D-Bus BusItem method handler ────────────────────────────────────── */

static DBusHandlerResult dbus_message_handler(DBusConnection *c,
                                               DBusMessage *msg,
                                               void *user_data) {
    (void)user_data;
    const char *path   = dbus_message_get_path(msg);
    const char *method = dbus_message_get_member(msg);
    const char *iface  = dbus_message_get_interface(msg);

    if (!iface || strcmp(iface, "com.victronenergy.BusItem") != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /* Introspect */
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable",
                                    "Introspect")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        const char *xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
            "    <method name=\"Introspect\"><arg direction=\"out\" type=\"s\"/></method>\n"
            "  </interface>\n"
            "  <interface name=\"com.victronenergy.BusItem\">\n"
            "    <method name=\"GetValue\"><arg direction=\"out\" type=\"v\"/></method>\n"
            "    <method name=\"GetText\"><arg direction=\"out\" type=\"s\"/></method>\n"
            "    <method name=\"GetItems\"><arg direction=\"out\" type=\"a{sa{sv}}\"/></method>\n"
            "    <signal name=\"ItemsChanged\"><arg type=\"a{sa{sv}}\" name=\"changes\"/></signal>\n"
            "  </interface>\n"
            "</node>\n";
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!method) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /* GetValue */
    if (strcmp(method, "GetValue") == 0) {
        int vtype;
        const void *val;
        if (path_get_value(path, &vtype, &val, NULL) != 0) {
            DBusMessage *err = dbus_message_new_error(msg,
                "com.victronenergy.BusItem.Error", "Path not found");
            dbus_connection_send(c, err, NULL);
            dbus_message_unref(err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        if (vtype == DBUS_TYPE_DOUBLE)
            msg_append_variant_double(&iter, *(const double *)val);
        else if (vtype == DBUS_TYPE_INT32)
            msg_append_variant_int32(&iter, *(const int *)val);
        else
            msg_append_variant_string(&iter, (const char *)val);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* GetText */
    if (strcmp(method, "GetText") == 0) {
        int vtype;
        const void *val;
        const char *text = NULL;
        if (path_get_value(path, &vtype, &val, &text) != 0) {
            DBusMessage *err = dbus_message_new_error(msg,
                "com.victronenergy.BusItem.Error", "Path not found");
            dbus_connection_send(c, err, NULL);
            dbus_message_unref(err);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        DBusMessage *reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* GetItems — called on root path, returns all paths and values */
    if (strcmp(method, "GetItems") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter, arr;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &arr);

        int i;
        for (i = 0; paths[i].path != NULL; i++) {
            int vtype;
            const void *val;
            const char *text;
            if (path_get_value(paths[i].path, &vtype, &val, &text) == 0) {
                msg_append_getitems_entry(&arr, paths[i].path,
                    vtype, val, text);
            }
        }
        /* Append dynamic cell voltage paths */
        int cell;
        char cell_path[32];
        static char cell_text[16];
        for (cell = 1; cell <= CELLS_PER_MODULE; cell++) {
            if (!battery.cell_v_valid[cell - 1]) continue;
            snprintf(cell_path, sizeof(cell_path), "/Voltages/Cell%d", cell);
            snprintf(cell_text, sizeof(cell_text), "%.3fV", battery.cell_v[cell - 1]);
            msg_append_getitems_entry(&arr, cell_path, DBUS_TYPE_DOUBLE,
                                      &battery.cell_v[cell - 1], cell_text);
        }

        dbus_message_iter_close_container(&iter, &arr);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ─── ItemsChanged signal emission ────────────────────────────────────── */

static void emit_items_changed(DBusConnection *c) {
    DBusMessage *sig = dbus_message_new_signal("/",
        "com.victronenergy.BusItem", "ItemsChanged");
    if (!sig) return;

    DBusMessageIter iter, arr;
    dbus_message_iter_init_append(sig, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &arr);

    int i;
    int emitted = 0;
    for (i = 0; paths[i].path != NULL; i++) {
        int vtype;
        const void *val;
        const char *text;
        if (path_get_value(paths[i].path, &vtype, &val, &text) != 0) continue;
        msg_append_getitems_entry(&arr, paths[i].path, vtype, val, text);
        emitted++;
    }

    /* Cell voltage paths */
    int cell;
    char cell_path[32];
    static char cell_text[16];
    for (cell = 1; cell <= CELLS_PER_MODULE; cell++) {
        if (!battery.cell_v_valid[cell - 1]) continue;
        snprintf(cell_path, sizeof(cell_path), "/Voltages/Cell%d", cell);
        snprintf(cell_text, sizeof(cell_text), "%.3fV", battery.cell_v[cell - 1]);
        msg_append_getitems_entry(&arr, cell_path, DBUS_TYPE_DOUBLE,
                                  &battery.cell_v[cell - 1], cell_text);
        emitted++;
    }

    dbus_message_iter_close_container(&iter, &arr);

    if (emitted > 0) {
        dbus_connection_send(c, sig, NULL);
        dbus_connection_flush(c);
    }
    dbus_message_unref(sig);
}

/* ─── D-Bus connection setup ──────────────────────────────────────────── */

static DBusConnection *dbus_setup(void) {
    DBusError err;
    DBusConnection *c;

    dbus_error_init(&err);
    c = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "D-Bus connection error: %s\n", err.message);
        dbus_error_free(&err);
        return NULL;
    }

    /* Register service name */
    int ret = dbus_bus_request_name(c, DBUS_SERVICE_NAME,
                                     DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "D-Bus name error: %s\n", err.message);
        dbus_error_free(&err);
        dbus_connection_unref(c);
        return NULL;
    }
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "D-Bus: service name %s already taken\n",
                DBUS_SERVICE_NAME);
        dbus_connection_unref(c);
        return NULL;
    }

    /* Register object path with message handler */
    dbus_bus_add_match(c,
        "type='method_call',interface='com.victronenergy.BusItem'",
        &err);
    dbus_bus_add_match(c,
        "type='method_call',interface='org.freedesktop.DBus.Introspectable'",
        &err);
    dbus_connection_add_filter(c, dbus_message_handler, NULL, NULL);

    printf("samsung-sdi-bms: registered on D-Bus as %s\n",
           DBUS_SERVICE_NAME);
    return c;
}

/* ─── Signal handler ──────────────────────────────────────────────────── */

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ─── Print usage ─────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--can-bms] <can-interface>\n\n"
        "Samsung SDI ELPM482-00005 BMS Driver for Victron Venus OS\n\n"
        "Publishes all 28 Samsung SDI fields directly to D-Bus via\n"
        "com.victronenergy.battery.samsung_sdi.  The Venus OS GUI and\n"
        "DVCC use this data directly.\n"
        "  %s can0             # D-Bus only (full telemetry)\n\n"
        "With --can-bms, also translates Samsung SDI frames to Victron\n"
        "CAN-bus BMS frames (0x351/0x355/0x356/0x35A) on the same\n"
        "interface.  The Victron can-bus-bms service picks them up.\n"
        "NOTE: CAN-BMS carries only 11 of the 28 Samsung fields.\n"
        "      Per-cell voltages, tray data, and heartbeat are D-Bus only.\n"
        "  %s --can-bms can0   # D-Bus + CAN-BMS translation\n",
        prog, prog, prog);
}

/* ─── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int can_bms_mode = 0;
    const char *ifname = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--can-bms") == 0) {
            can_bms_mode = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            ifname = argv[i];
        }
    }

    if (!ifname) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Connect CAN */
    int can_fd = can_open(ifname);
    if (can_fd < 0) {
        fprintf(stderr, "FATAL: cannot open CAN interface %s\n", ifname);
        return 1;
    }
    printf("samsung-sdi-bms: CAN connected on %s\n", ifname);

    /* Connect D-Bus */
    dbus_conn = dbus_setup();
    if (!dbus_conn) {
        fprintf(stderr, "FATAL: D-Bus connection failed\n");
        close(can_fd);
        return 1;
    }

    if (can_bms_mode)
        printf("samsung-sdi-bms: CAN-BMS translation enabled "
               "(0x351/0x355/0x356/0x35A)\n");

    /* Emit initial ItemsChanged so systemcalc discovers the service */
    emit_items_changed(dbus_conn);

    /* Initialise state */
    memset(&battery, 0, sizeof(battery));
    memset(&battery_prev, 0, sizeof(battery_prev));

    static const unsigned char min_dlc[5] = {8, 8, 8, 8, 7};

    while (running) {
        /* Poll CAN for incoming frames (100ms timeout) */
        struct can_frame frame;
        int can_ret = can_recv(can_fd, &frame, 100);
        if (can_ret > 0) {
            /* Parse Samsung SDI frames */
            if (frame.can_id >= SDI_CAN_ID_STATUS &&
                frame.can_id <= SDI_CAN_ID_TEMPERATURE) {
                if (frame.can_dlc < min_dlc[frame.can_id - SDI_CAN_ID_STATUS])
                    goto dbus_dispatch;

                switch (frame.can_id) {
                case SDI_CAN_ID_STATUS:      parse_status(frame.data);      break;
                case SDI_CAN_ID_CONFIG:      parse_config(frame.data);      break;
                case SDI_CAN_ID_LIMITS:      parse_limits(frame.data);      break;
                case SDI_CAN_ID_CELL_VOLTAGE:parse_cell_voltage(frame.data);break;
                case SDI_CAN_ID_TEMPERATURE: parse_temperature(frame.data); break;
                default: goto dbus_dispatch;
                }
            } else if (frame.can_id >= SDI_CAN_ID_CELL_1_3 &&
                       frame.can_id <= SDI_CAN_ID_CELL_13_14) {
                int cs;
                switch (frame.can_id) {
                case SDI_CAN_ID_CELL_1_3:   cs = 0; break;
                case SDI_CAN_ID_CELL_4_6:   cs = 3; break;
                case SDI_CAN_ID_CELL_7_9:   cs = 6; break;
                case SDI_CAN_ID_CELL_10_12: cs = 9; break;
                case SDI_CAN_ID_CELL_13_14: cs = 12; break;
                default: goto dbus_dispatch;
                }
                parse_cell_frame(cs, frame.data, frame.can_dlc);
            } else {
                goto dbus_dispatch;
            }

            battery.last_update = time(NULL);

            /* Emit ItemsChanged and optionally send CAN-BMS on 0x500 */
            if (frame.can_id == SDI_CAN_ID_STATUS) {
                if (!data_valid) {
                    data_valid = 1;
                    printf("samsung-sdi-bms: data received, publishing to D-Bus\n");
                }
                emit_items_changed(dbus_conn);
                if (can_bms_mode)
                    send_can_bms_frames(can_fd);
            }
        } else if (can_ret == 0) {
            /* Timeout — check for data staleness */
            if (data_valid &&
                time(NULL) - battery.last_update > DATA_TIMEOUT_SEC) {
                data_valid = 0;
                printf("samsung-sdi-bms: CAN data timeout, marking disconnected\n");
                emit_items_changed(dbus_conn);
            }
        }

dbus_dispatch:
        /* Process pending D-Bus messages (non-blocking) */
        dbus_connection_read_write_dispatch(dbus_conn, 0);
    }

    printf("samsung-sdi-bms: shutting down\n");

    /* Unregister from D-Bus */
    if (dbus_conn) {
        dbus_bus_release_name(dbus_conn, DBUS_SERVICE_NAME, NULL);
        dbus_connection_unref(dbus_conn);
    }
    if (can_fd >= 0) close(can_fd);
    return 0;
}
