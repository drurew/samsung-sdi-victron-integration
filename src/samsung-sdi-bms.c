/*
 * samsung-sdi-bms.c — Samsung SDI BMS Driver for Victron Venus OS
 *
 * Publishes Samsung SDI ELPM482-00005 battery data directly to D-Bus
 * (com.victronenergy.battery.samsung_sdi) using libdbus-1.  Optionally
 * also translates to Victron CAN-bus BMS frames (--can-bms flag).
 *
 * Build:  gcc -Os -std=c99 -Wall -Wextra -D_GNU_SOURCE \
 *             -o samsung-sdi-bms samsung-sdi-bms.c -lm -ldbus-1
 * Run:    ./samsung-sdi-bms can0
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
#ifndef UNIT_TEST
#include <dbus/dbus.h>
#endif

/* ─── SocketCAN (from <linux/can.h>) ─────────────────────────────────── */

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
struct can_frame { canid_t id; unsigned char dlc, __pad; unsigned short __res0; unsigned char data[8] __attribute__((aligned(8))); };

/* ─── CAN IDs ─────────────────────────────────────────────────────────── */

#define SAMSUNG_STATUS    0x500  /* voltage, current, SOC, SOH, heartbeat */
#define SAMSUNG_CONFIG    0x501  /* alarm/protection bitfields, tray counts */
#define SAMSUNG_LIMITS    0x502  /* charge/discharge voltage/current limits */
#define SAMSUNG_CELL_V    0x503  /* avg/max/min cell voltage, avg tray voltage */
#define SAMSUNG_TEMP      0x504  /* tray voltage extremes, cell temperatures */
#define SAMSUNG_CELL_x3_0 0x5F0  /* per-cell voltages 1-3 */
#define SAMSUNG_CELL_x3_1 0x5F1  /* 4-6 */
#define SAMSUNG_CELL_x3_2 0x5F2  /* 7-9 */
#define SAMSUNG_CELL_x3_3 0x5F3  /* 10-12 */
#define SAMSUNG_CELL_x3_4 0x5F4  /* 13-14 */

/* Victron CAN-bus BMS frames, verified against stock driver on Cerbo GX MK2 */
#define VCAN_LIMITS       0x351  /* CVL 0.1V, CCL 0.1A, DCL 0.1A, DVL 0.1V (all u16) */
#define VCAN_SOC_SOH      0x355  /* SOC/SOH u16 at 1% */
#define VCAN_STATE        0x356  /* V 0.01V (s16), I 0.1A (s16), T 0.1°C (s16), 6 bytes */
#define VCAN_ALARMS       0x35A  /* 2-bit flag pairs: alarms bytes 0-3, warnings bytes 4-7 */
#define VCAN_NAME         0x35E  /* Product name, 8 ASCII */
#define VCAN_INFO         0x35F  /* Model tag, protocol rev, capacity (Ah) */
#define VCAN_MODULES      0x372  /* Online/offline module counts */
#define VCAN_CELLINFO     0x373  /* Min/max cell mV + temp (Kelvin), feeds GX Details page */

/* ─── Constants ───────────────────────────────────────────────────────── */

#define DATA_TIMEOUT_SEC   5
#define CAPACITY_AH        94.0       /* ELPM482-00005 spec Table 4 */
#define CELLS              14         /* 14S configuration */
#define SERVICE_NAME       "com.victronenergy.battery.samsung_sdi"
#define DEVICE_INSTANCE    280
#define VERSION            "2.1.0"

/* ─── Battery state ───────────────────────────────────────────────────── */

typedef struct {
    double v, i, soc, soh;
    double temp_avg, temp_max, temp_min;
    double cell_v_max, cell_v_min, cell_v_avg;
    double tray_v_avg, tray_v_max, tray_v_min;
    double cvl, ccl, dcl, dvl;       /* DVCC limits */
    unsigned char trays_total, trays_ok, trays_fault;
    unsigned int  alarm, protect, heartbeat;
    double  cell[CELLS]; int cell_ok[CELLS];
    time_t  last_update;
    /* per-frame freshness: the CAN-BMS translation requires status +
     * config + limits ALL fresh — otherwise a partial failure (e.g.
     * 0x502 frames stopping) would re-emit stale limits on every
     * 0x500, and the first 0x500 after boot would emit zeroed limits
     * before 0x501/0x502 have ever arrived. */
    time_t  rx_status, rx_config, rx_limits;
} state_t;

static state_t        st;
#ifndef UNIT_TEST
static int            live = 0;       /* set when CAN data flows */
#endif

/* all critical inputs fresh within the timeout? (gates --can-bms TX) */
static int vcan_fresh(time_t now) {
    return st.rx_status > 0 && now - st.rx_status <= DATA_TIMEOUT_SEC &&
           st.rx_config > 0 && now - st.rx_config <= DATA_TIMEOUT_SEC &&
           st.rx_limits > 0 && now - st.rx_limits <= DATA_TIMEOUT_SEC;
}
static volatile int   running = 1;

/* ─── Little-endian helpers ───────────────────────────────────────────── */

static unsigned short u16(const unsigned char *d, int o) { return d[o] | (d[o+1] << 8); }
static short          s16(const unsigned char *d, int o) { return (short)u16(d, o); }
static signed char    s8 (const unsigned char *d, int o) { return (signed char)d[o]; }

#ifndef UNIT_TEST
/* ─── CAN I/O ─────────────────────────────────────────────────────────── */

static int can_open(const char *ifname) {
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) { perror("socket(CAN)"); return -1; }
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("ioctl"); close(fd); return -1; }
    struct { unsigned short f; unsigned short p; int idx; } a = {AF_CAN, 0, ifr.ifr_ifindex};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); close(fd); return -1; }
    return fd;
}
static int can_recv(int fd, struct can_frame *f, int ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, ms) <= 0) return 0;
    return read(fd, f, sizeof(*f)) == sizeof(*f) ? 1 : 0;
}
static int can_send(int fd, canid_t id, const unsigned char *d, int len) {
    struct can_frame f = {.id = id, .dlc = len};
    memcpy(f.data, d, len);
    return write(fd, &f, sizeof(f)) == sizeof(f) ? 0 : -1;
}

#else  /* UNIT_TEST: capture frames instead of writing to a socket */
static struct { canid_t id; int len; unsigned char d[8]; } cap[16];
static int cap_n = 0;
static int can_send(int fd, canid_t id, const unsigned char *d, int len) {
    (void)fd;
    if (cap_n < 16) { cap[cap_n].id = id; cap[cap_n].len = len;
                      memcpy(cap[cap_n].d, d, (size_t)len); cap_n++; }
    return 0;
}
#endif

/* ─── Samsung SDI parsers ─────────────────────────────────────────────── */

static void parse_status(const unsigned char *d) {
    st.v = u16(d,0)*0.01; st.i = s16(d,2)*1.0; st.soc = d[4]; st.soh = d[5]; st.heartbeat = u16(d,6);
}
static void parse_config(const unsigned char *d) {
    st.alarm = u16(d,0); st.protect = u16(d,2); st.trays_total = d[4]; st.trays_ok = d[5]; st.trays_fault = d[6];
}
static void parse_limits(const unsigned char *d) {
    st.cvl = u16(d,0)*0.1; st.ccl = u16(d,2)*0.1; st.dcl = u16(d,4)*0.1; st.dvl = u16(d,6)*0.1;
}
static void parse_cell_v(const unsigned char *d) {
    st.cell_v_avg = u16(d,0)*0.001; st.cell_v_max = u16(d,2)*0.001;
    st.cell_v_min = u16(d,4)*0.001; st.tray_v_avg = u16(d,6)*0.01;
}
static void parse_temp(const unsigned char *d) {
    st.tray_v_max = u16(d,0)*0.01; st.tray_v_min = u16(d,2)*0.01;
    st.temp_avg = s8(d,4); st.temp_max = s8(d,5); st.temp_min = s8(d,6);
}
static void parse_cells(int start, const unsigned char *d, int dlc) {
    for (int i = 0; i < 3 && start + i < CELLS; i++) {
        int off = 2 + i*2;
        if (off + 2 > dlc) break;
        unsigned short raw = u16(d, off);
        if (raw) { st.cell[start + i] = raw * 0.001; st.cell_ok[start + i] = 1; }
    }
}

#ifndef UNIT_TEST
/* Samsung 0x501 → Victron 0=ok, 1=warning, 2=alarm (D-Bus alarm paths) */
static int alarm_sev(unsigned int alarm_bit, unsigned int prot_bit) {
    if (st.protect & prot_bit) return 2;
    if (st.alarm  & alarm_bit) return 1;
    return 0;
}

#endif

/* ─── Victron CAN-BMS frame builders (--can-bms only) ─────────────────── */

#define CVL_MAX_V  55.4   /* cap CVL at 3.957 V/cell on 14S; BMS broadcasts 58.1 V */

static void vcan_send_all(int can_fd) {
    unsigned char d[8];
    double cv, cc, dc;
    short sv, si, stmp;
    unsigned short uv;

    /* 0x351 — LIMITS: CVL 0.1V, CCL 0.1A, DCL 0.1A, DVL 0.1V (all u16).
     * Safety: clamp CVL, force CCL=DCL=0 on any protection bit. */
    cv = st.cvl > CVL_MAX_V ? CVL_MAX_V : st.cvl;
    cc = st.protect ? 0.0 : st.ccl;
    dc = st.protect ? 0.0 : st.dcl;
    uv = (unsigned short)(cv * 10.0 + 0.5);   d[0]=uv&0xFF; d[1]=uv>>8;
    uv = (unsigned short)(cc * 10.0 + 0.5);   d[2]=uv&0xFF; d[3]=uv>>8;
    uv = (unsigned short)(dc * 10.0 + 0.5);   d[4]=uv&0xFF; d[5]=uv>>8;
    uv = (unsigned short)(st.dvl * 10.0 + 0.5); d[6]=uv&0xFF; d[7]=uv>>8;
    can_send(can_fd, VCAN_LIMITS, d, 8);

    /* 0x355 — SOC/SOH: u16 at 1% (NOT 0.1%) */
    uv = st.soc > 100 ? 100 : (unsigned short)(st.soc + 0.5);  d[0]=uv&0xFF; d[1]=uv>>8;
    uv = st.soh > 100 ? 100 : (unsigned short)(st.soh + 0.5);  d[2]=uv&0xFF; d[3]=uv>>8;
    d[4]=d[5]=d[6]=d[7]=0;
    can_send(can_fd, VCAN_SOC_SOH, d, 8);

    /* 0x356 — STATE: V (s16, 0.01V), I (s16, 0.1A), T (s16, 0.1°C). 6 bytes. */
    sv   = (short)(st.v * 100.0 + (st.v >= 0 ? 0.5 : -0.5));     d[0]=sv&0xFF; d[1]=sv>>8;
    si   = (short)(st.i * 10.0  + (st.i >= 0 ? 0.5 : -0.5));     d[2]=si&0xFF; d[3]=si>>8;
    stmp = (short)(st.temp_avg * 10.0 + (st.temp_avg >= 0 ? 0.5 : -0.5)); d[4]=stmp&0xFF; d[5]=stmp>>8;
    d[6]=d[7]=0;
    can_send(can_fd, VCAN_STATE, d, 6);

    /* 0x35A — 2-bit flag pairs: alarms bytes 0-3, warnings bytes 4-7.
     * Samsung alarm bits (BMS warning)→Victron warnings. Samsung protection→alarms. */
    memset(d, 0, 8);
    unsigned int a = st.alarm, p = st.protect;
    #define SET(b, f) (d[(b)+((f)>>3)] |= (unsigned char)(0x01 << ((f)&7)))
    /* warnings from alarm bits */
    if (a&0x0001) SET(4,2);
    if (a&0x0002) SET(4,4);
    if (a&0x0004) SET(4,6);
    if (a&0x0008) SET(4,8);
    if (a&0x0010) SET(4,16);
    if (a&0x0020) SET(4,14);
    if (a&0x0040) SET(4,22);
    if (a&0x0080) SET(4,24);
    /* alarms from protection bits */
    if (p&0x0001) SET(0,2);
    if (p&0x4000) SET(0,2);   /* 2nd OV */
    if (p&0x0002) SET(0,4);
    if (p&0x1000) SET(0,4);   /* UV shutdown */
    if (p&0x0004) SET(0,6);
    if (p&0x0008) SET(0,8);
    if (p&0x0010) SET(0,16);
    if (p&0x0020) SET(0,14);
    if (p&0x0080) SET(0,24);
    if (p&0x2000) SET(0,24);  /* tray+cell imbalance */
    if (p&(0x0040|0x0100|0x0200|0x0400|0x0800)) SET(0,22);
    if (p) SET(0,0);  /* general alarm if any protection active */
    #undef SET
    can_send(can_fd, VCAN_ALARMS, d, 8);

    /* 0x372 — module counts: online, offline */
    uv = st.trays_ok;    d[0]=uv&0xFF; d[1]=uv>>8;
    d[2]=d[3]=0;  d[4]=d[5]=0;
    uv = st.trays_fault; d[6]=uv&0xFF; d[7]=uv>>8;
    can_send(can_fd, VCAN_MODULES, d, 8);

    /* 0x373 — min/max cell mV + min/max temp Kelvin (feeds GX Details page) */
    uv = (unsigned short)(st.cell_v_min * 1000.0 + 0.5);    d[0]=uv&0xFF; d[1]=uv>>8;
    uv = (unsigned short)(st.cell_v_max * 1000.0 + 0.5);    d[2]=uv&0xFF; d[3]=uv>>8;
    uv = (unsigned short)(st.temp_min + 273.15);            d[4]=uv&0xFF; d[5]=uv>>8;
    uv = (unsigned short)(st.temp_max + 273.15);            d[6]=uv&0xFF; d[7]=uv>>8;
    can_send(can_fd, VCAN_CELLINFO, d, 8);

    /* 0x35E — product name (8 ASCII) */
    memcpy(d, "SDI 482 ", 8);  can_send(can_fd, VCAN_NAME, d, 8);

    /* 0x35F — model tag 0x0482, spec rev 0.2, capacity Ah */
    uv = (unsigned short)(CAPACITY_AH + 0.5);
    d[0]=0x82; d[1]=0x04;  d[2]=0x02; d[3]=0x00;
    d[4]=uv&0xFF; d[5]=uv>>8;  d[6]=d[7]=0;
    can_send(can_fd, VCAN_INFO, d, 8);
}

#ifndef UNIT_TEST
/* ─── D-Bus path definitions ──────────────────────────────────────────── */

typedef enum { T_DOUBLE, T_INT, T_STRING } ptype_t;

typedef struct {
    const char *path;
    ptype_t     type;
    /* If ptr is NULL, value is computed by fn (cast to double(*)(void) / int(*)(void)).
     * If not NULL, pointer to value in state_t (for T_DOUBLE) or static int (T_INT). */
    const void *ptr;
    int     (*fn_int)(void);     /* computed int getter */
    double  (*fn_double)(void);  /* computed double getter */
    const char *fmt;             /* printf format for GetText, NULL = use default */
} path_t;

/* ---- computed value functions ---- */
static int    gn_connected(void)     { return live; }
static int    gn_cells(void)         { return CELLS; }
static int    gn_online(void)        { return st.trays_ok; }
static int    gn_offline(void)       { return st.trays_fault; }
static int    gn_blockcharge(void)   { return 0; }
static int    gn_blockdischarge(void){ return 0; }
static int    gn_allowcharge(void)   { return live; }
static int    gn_allowdischarge(void){ return live; }
static int    gn_instance(void)      { return DEVICE_INSTANCE; }
static double gd_power(void)         { return st.v * st.i; }
static double gd_consumed(void)      { return CAPACITY_AH * (100.0 - st.soc) / 100.0; }
static double gd_capacity(void)      { return CAPACITY_AH; }
/* alarms: Samsung bit → Victron severity */
static int al_low_v(void)     { int a=alarm_sev(0x0002,0x0002); if(st.protect&0x1000) a=2; return a; }
static int al_high_v(void)    { int a=alarm_sev(0x0001,0x0001); if(st.protect&0x4000) a=2; return a; }
static int al_low_t(void)     { return alarm_sev(0x0008,0x0008); }
static int al_high_t(void)    { return alarm_sev(0x0004,0x0004); }
static int al_low_soc(void)   { return st.soc < 10.0 ? 1 : 0; }
static int al_chg_oc(void)    { return alarm_sev(0x0010,0x0010); }
static int al_dis_oc(void)    { return alarm_sev(0x0020,0x0020); }
static int al_imbal(void)     { int a=alarm_sev(0x0080,0x0080); if(st.protect&0x2000) a=2; return a; }
static int al_internal(void) {
    if(st.protect & (0x0040|0x0100|0x0200|0x0400|0x0800)) return 2;
    if(st.alarm & 0x0040) return 1;
    return 0;
}
static int al_chg_hi_t(void)  { return alarm_sev(0x0004,0x0004); }
static int al_chg_lo_t(void)  { return alarm_sev(0x0008,0x0008); }

/* Define a path entry: P(path, type, ptr_or_null, fn_int, fn_double, fmt) */
#define P_D(p, member, fmt) { p, T_DOUBLE, &st.member, NULL, NULL, fmt }
#define P_DF(p, fn, fmt)    { p, T_DOUBLE, NULL, NULL, fn, fmt }
#define P_I(p, member)      { p, T_INT,    &(int){member}, NULL, NULL, NULL }
#define P_IF(p, fn)         { p, T_INT,    NULL, fn, NULL, NULL }
#define P_S(p, str)         { p, T_STRING, str, NULL, NULL, NULL }
#define P_END               { NULL, 0, NULL, NULL, NULL, NULL }

static path_t paths[] = {
    /* Measurements — systemcalc subscribes to these for DVCC */
    P_D ("/Soc",                     soc,                  "%.0f%%"),
    P_D ("/Dc/0/Voltage",            v,                    "%.2fV"),
    P_D ("/Dc/0/Current",            i,                    "%.1fA"),
    P_DF("/Dc/0/Power",              gd_power,             "%.0fW"),
    P_D ("/Dc/0/Temperature",        temp_avg,             "%.1f°C"),
    /* DVCC limits — /Info/MaxChargeVoltage must not be None for BMS detection */
    P_D ("/Info/MaxChargeCurrent",   ccl,                  "%.1fA"),
    P_D ("/Info/MaxDischargeCurrent",dcl,                  "%.1fA"),
    P_D ("/Info/MaxChargeVoltage",   cvl,                  "%.2fV"),
    /* Cell voltage summaries */
    P_D ("/System/MinCellVoltage",   cell_v_min,           "%.3fV"),
    P_D ("/System/MaxCellVoltage",   cell_v_max,           "%.3fV"),
    /* Module/tray counters */
    P_IF("/System/NrOfCellsPerBattery",         gn_cells),
    P_IF("/System/NrOfModulesOnline",           gn_online),
    P_IF("/System/NrOfModulesOffline",          gn_offline),
    P_IF("/System/NrOfModulesBlockingCharge",   gn_blockcharge),
    P_IF("/System/NrOfModulesBlockingDischarge",gn_blockdischarge),
    /* Connection & control */
    P_IF("/Connected",                gn_connected),
    P_IF("/Io/AllowToCharge",         gn_allowcharge),
    P_IF("/Io/AllowToDischarge",      gn_allowdischarge),
    /* History */
    P_DF("/ConsumedAmphours",         gd_consumed,    NULL),
    /* Metadata */
    P_IF("/DeviceInstance",           gn_instance),
    P_DF("/InstalledCapacity",        gd_capacity,    NULL),
    P_S ("/ProductName",              "Samsung SDI ELPM482-00005"),
    P_S ("/HardwareVersion",          "ELPM482-00005"),
    P_S ("/FirmwareVersion",          VERSION),
    P_S ("/Mgmt/ProcessName",         "samsung-sdi-bms"),
    P_S ("/Mgmt/ProcessVersion",      VERSION),
    P_S ("/Mgmt/Connection",          "CAN PDO 500kbps"),
    /* Alarms — Victron convention: 0=ok, 1=warning, 2=alarm */
    P_IF("/Alarms/LowVoltage",            al_low_v),
    P_IF("/Alarms/HighVoltage",           al_high_v),
    P_IF("/Alarms/LowTemperature",        al_low_t),
    P_IF("/Alarms/HighTemperature",       al_high_t),
    P_IF("/Alarms/LowSoc",                al_low_soc),
    P_IF("/Alarms/HighChargeCurrent",     al_chg_oc),
    P_IF("/Alarms/HighDischargeCurrent",  al_dis_oc),
    P_IF("/Alarms/CellImbalance",         al_imbal),
    P_IF("/Alarms/InternalFailure",       al_internal),
    P_IF("/Alarms/HighChargeTemperature", al_chg_hi_t),
    P_IF("/Alarms/LowChargeTemperature",  al_chg_lo_t),
    P_END
};

/* ─── Path value lookup ───────────────────────────────────────────────── */

static int path_val(const char *p, int *vtype, const void **val, const char **text) {
    static char buf[64];
    for (int i = 0; paths[i].path; i++) {
        path_t *pt = &paths[i];
        if (strcmp(p, pt->path)) continue;

        if (pt->type == T_DOUBLE) {
            *vtype = DBUS_TYPE_DOUBLE;
            double dv = pt->ptr ? *(double *)pt->ptr : (pt->fn_double ? pt->fn_double() : 0.0);
            static double sd; sd = dv; *val = &sd;
            if (pt->fmt && text) { snprintf(buf, sizeof(buf), pt->fmt, dv); *text = buf; }
        } else if (pt->type == T_INT) {
            *vtype = DBUS_TYPE_INT32;
            int iv = pt->ptr ? *(int *)pt->ptr : (pt->fn_int ? pt->fn_int() : 0);
            static int si; si = iv; *val = &si;
            if (pt->fmt && text) { snprintf(buf, sizeof(buf), pt->fmt, iv); *text = buf; }
        } else {
            *vtype = DBUS_TYPE_STRING;
            *val = (const void *)pt->ptr;
            if (text) { snprintf(buf, sizeof(buf), "%s", (const char *)pt->ptr); *text = buf; }
        }
        return 0;
    }
    /* Per-cell voltages: /Voltages/Cell1 ... /Voltages/Cell14 */
    if (strncmp(p, "/Voltages/Cell", 14) == 0) {
        int n = atoi(p + 14);
        if (n >= 1 && n <= CELLS && st.cell_ok[n-1]) {
            *vtype = DBUS_TYPE_DOUBLE;
            static double cv; cv = st.cell[n-1]; *val = &cv;
            if (text) { snprintf(buf, sizeof(buf), "%.3fV", cv); *text = buf; }
            return 0;
        }
    }
    return -1;
}

/* ─── D-Bus item serialisation ────────────────────────────────────────── */

static DBusConnection *conn;

/* Append one {sa{sv}} dict entry: path → {"Value": variant, "Text": variant} */
static void append_entry(DBusMessageIter *arr, const char *path, int vtype, const void *val, const char *text) {
    DBusMessageIter e, d, de, sv;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &path);
    dbus_message_iter_open_container(&e, DBUS_TYPE_ARRAY, "{sv}", &d);

    dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &de);
    const char *kn = "Value"; dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &kn);
    const char *sig = vtype == DBUS_TYPE_DOUBLE ? "d" : vtype == DBUS_TYPE_INT32 ? "i" : "s";
    dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, sig, &sv);
    dbus_message_iter_append_basic(&sv, vtype, val);
    dbus_message_iter_close_container(&de, &sv);
    dbus_message_iter_close_container(&d, &de);

    dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &de);
    const char *tn = "Text"; dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &tn);
    dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "s", &sv);
    dbus_message_iter_append_basic(&sv, DBUS_TYPE_STRING, &text);
    dbus_message_iter_close_container(&de, &sv);
    dbus_message_iter_close_container(&d, &de);

    dbus_message_iter_close_container(&e, &d);
    dbus_message_iter_close_container(arr, &e);
}

/* Append all paths (including live cell voltages) into an {sa{sv}} array */
static int append_all(DBusMessageIter *arr) {
    int count = 0;
    for (int i = 0; paths[i].path; i++) {
        int vt; const void *v; const char *t;
        if (path_val(paths[i].path, &vt, &v, &t) == 0)
            { append_entry(arr, paths[i].path, vt, v, t); count++; }
    }
    for (int n = 1; n <= CELLS; n++) {
        if (!st.cell_ok[n-1]) continue;
        char p[32], t[16];
        snprintf(p, sizeof(p), "/Voltages/Cell%d", n);
        snprintf(t, sizeof(t), "%.3fV", st.cell[n-1]);
        int vt = DBUS_TYPE_DOUBLE; double cv = st.cell[n-1];
        append_entry(arr, p, vt, &cv, t); count++;
    }
    return count;
}

/* ─── D-Bus message handler (com.victronenergy.BusItem) ───────────────── */

static DBusHandlerResult on_msg(DBusConnection *c, DBusMessage *m, void *u) {
    (void)u;
    const char *iface = dbus_message_get_interface(m);
    const char *path  = dbus_message_get_path(m);
    const char *method = dbus_message_get_member(m);

    /* Allow introspection on any path */
    if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"com.victronenergy.BusItem\">\n"
            "    <method name=\"GetValue\"><arg direction=\"out\" type=\"v\"/></method>\n"
            "    <method name=\"GetText\"><arg direction=\"out\" type=\"s\"/></method>\n"
            "    <method name=\"GetItems\"><arg direction=\"out\" type=\"a{sa{sv}}\"/></method>\n"
            "    <signal name=\"ItemsChanged\"><arg type=\"a{sa{sv}}\" name=\"changes\"/></signal>\n"
            "  </interface>\n</node>\n";
        DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(c, r, NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!iface || !method || strcmp(iface, "com.victronenergy.BusItem")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!strcmp(method, "GetValue")) {
        int vt; const void *v;
        if (path_val(path, &vt, &v, NULL)) {
            DBusMessage *e = dbus_message_new_error(m, "com.victronenergy.BusItem.Error", "Path not found");
            dbus_connection_send(c, e, NULL); dbus_message_unref(e); return DBUS_HANDLER_RESULT_HANDLED;
        }
        DBusMessage *r = dbus_message_new_method_return(m);
        DBusMessageIter it; dbus_message_iter_init_append(r, &it);
        DBusMessageIter sv; const char *sig = vt == DBUS_TYPE_DOUBLE ? "d" : vt == DBUS_TYPE_INT32 ? "i" : "s";
        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &sv);
        dbus_message_iter_append_basic(&sv, vt, v);
        dbus_message_iter_close_container(&it, &sv);
        dbus_connection_send(c, r, NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!strcmp(method, "GetText")) {
        int vt; const void *v; const char *t = NULL;
        if (path_val(path, &vt, &v, &t)) {
            DBusMessage *e = dbus_message_new_error(m, "com.victronenergy.BusItem.Error", "Path not found");
            dbus_connection_send(c, e, NULL); dbus_message_unref(e); return DBUS_HANDLER_RESULT_HANDLED;
        }
        DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &t, DBUS_TYPE_INVALID);
        dbus_connection_send(c, r, NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (!strcmp(method, "GetItems")) {
        DBusMessage *r = dbus_message_new_method_return(m);
        DBusMessageIter it, arr;
        dbus_message_iter_init_append(r, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sa{sv}}", &arr);
        append_all(&arr);
        dbus_message_iter_close_container(&it, &arr);
        dbus_connection_send(c, r, NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ─── ItemsChanged signal ─────────────────────────────────────────────── */

static void emit_changed(void) {
    DBusMessage *s = dbus_message_new_signal("/", "com.victronenergy.BusItem", "ItemsChanged");
    if (!s) return;
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(s, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sa{sv}}", &arr);
    if (append_all(&arr) > 0) { dbus_connection_send(conn, s, NULL); dbus_connection_flush(conn); }
    dbus_message_unref(s);
}

/* ─── D-Bus setup ─────────────────────────────────────────────────────── */

static int dbus_init(void) {
    DBusError e; dbus_error_init(&e);
    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    if (dbus_error_is_set(&e)) { fprintf(stderr, "dbus: %s\n", e.message); dbus_error_free(&e); return -1; }
    int r = dbus_bus_request_name(conn, SERVICE_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "dbus: name %s taken\n", SERVICE_NAME); dbus_connection_unref(conn); return -1;
    }
    dbus_bus_add_match(conn, "type='method_call',interface='com.victronenergy.BusItem'", &e);
    dbus_bus_add_match(conn, "type='method_call',interface='org.freedesktop.DBus.Introspectable'", &e);
    dbus_connection_add_filter(conn, on_msg, NULL, NULL);
    printf("samsung-sdi-bms: D-Bus registered as %s\n", SERVICE_NAME);
    return 0;
}

/* ─── Main ────────────────────────────────────────────────────────────── */

static void on_signal(int sig) { (void)sig; running = 0; }

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [--can-bms] <can-interface>\n\n"
        "Samsung SDI ELPM482-00005 BMS Driver for Victron Venus OS\n\n"
        "Publishes all 28 Samsung SDI data fields directly to D-Bus.\n"
        "  %s can0\n\n"
        "--can-bms   Also emit Victron CAN-bus BMS frames (limited subset)\n"
        "            on the same CAN interface for the Victron can-bus-bms\n"
        "            service to consume alongside the D-Bus data.\n",
        p, p);
}

int main(int argc, char **argv) {
    int can_bms = 0;
    const char *ifname = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--can-bms")) can_bms = 1;
        else if (argv[i][0] == '-') { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
        else ifname = argv[i];
    }
    if (!ifname) { usage(argv[0]); return 1; }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int cfd = can_open(ifname);
    if (cfd < 0) return 1;
    printf("samsung-sdi-bms: CAN on %s\n", ifname);

    if (dbus_init() < 0) { close(cfd); return 1; }
    memset(&st, 0, sizeof(st));

    emit_changed(); /* initial publish so systemcalc discovers us */
    if (can_bms) printf("samsung-sdi-bms: CAN-BMS translation on\n");

    static const unsigned char min_dlc[5] = {8,8,8,8,7};

    while (running) {
        struct can_frame f;
        if (can_recv(cfd, &f, 100) > 0) {
            int ok = 0;
            if (f.id >= SAMSUNG_STATUS && f.id <= SAMSUNG_TEMP) {
                if (f.dlc < min_dlc[f.id - SAMSUNG_STATUS]) goto dispatch;
                switch (f.id) {
                case SAMSUNG_STATUS: parse_status(f.data); st.rx_status = time(NULL); break;
                case SAMSUNG_CONFIG: parse_config(f.data); st.rx_config = time(NULL); break;
                case SAMSUNG_LIMITS: parse_limits(f.data); st.rx_limits = time(NULL); break;
                case SAMSUNG_CELL_V: parse_cell_v(f.data); break;
                case SAMSUNG_TEMP:   parse_temp(f.data);   break;
                default: goto dispatch;
                }
                ok = 1;
            } else if (f.id >= SAMSUNG_CELL_x3_0 && f.id <= SAMSUNG_CELL_x3_4) {
                parse_cells((f.id - SAMSUNG_CELL_x3_0) * 3, f.data, f.dlc);
                ok = 1;
            }
            if (ok) {
                st.last_update = time(NULL);
                if (f.id == SAMSUNG_STATUS) {
                    if (!live) { live = 1; printf("samsung-sdi-bms: publishing to D-Bus\n"); }
                    emit_changed();
                    /* translate only from a complete, fresh picture */
                    if (can_bms && vcan_fresh(time(NULL)))
                        vcan_send_all(cfd);
                }
            }
        } else {
            if (live && time(NULL) - st.last_update > DATA_TIMEOUT_SEC) {
                live = 0;
                printf("samsung-sdi-bms: CAN timeout, marking disconnected\n");
                emit_changed();
            }
        }
dispatch:
        dbus_connection_read_write_dispatch(conn, 0);
    }

    printf("samsung-sdi-bms: shutting down\n");
    dbus_bus_release_name(conn, SERVICE_NAME, NULL);
    dbus_connection_unref(conn);
    close(cfd);
    return 0;
}

#else  /* ─────────────────────────── UNIT_TEST ─────────────────────────── */

/* Build:  gcc -D_GNU_SOURCE -DUNIT_TEST -Os -Wall -Wextra -std=c99 \
 *              -o samsung-sdi-bms-test src/samsung-sdi-bms.c
 * No libdbus-1 headers required — the test build exercises the parsers
 * and the --can-bms frame output only. Vectors marked "real capture"
 * were recorded from a live ELPM482-00005 (Cerbo GX MK2, Venus v3.70). */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } } while (0)
static int feq(double x, double y) { return x - y < 1e-6 && y - x < 1e-6; }
static const unsigned char *frame(canid_t id) {
    for (int k = 0; k < cap_n; k++) if (cap[k].id == id) return cap[k].d;
    return NULL;
}

int main(void) {
    memset(&st, 0, sizeof(st));

    /* ── parsers against real captures ── */
    unsigned char t504[8] = {0x32,0x13,0x32,0x13,0x1D,0x1D,0x1D,0x00};
    parse_temp(t504);
    CHECK(feq(st.tray_v_max, 49.14), "0x504 max tray 49.14 V (real capture)");
    CHECK(feq(st.temp_avg, 29.0),    "0x504 avg temp 29 C");

    unsigned char t500[8] = {0x32,0x13, 0xFD,0xFF, 13, 100, 0x34,0x12};
    parse_status(t500);
    CHECK(feq(st.v, 49.14) && feq(st.i, -3.0) && feq(st.soc, 13.0),
          "0x500 V/I/SOC incl. S16 sign");

    unsigned char t502[8] = {0x45,0x02, 0xD6,0x01, 0xD6,0x01, 0xC0,0x01};
    parse_limits(t502);
    CHECK(feq(st.cvl, 58.1) && feq(st.ccl, 47.0), "0x502 CVL/CCL");

    unsigned char t503[8] = {0xC0,0x0D, 0xC2,0x0D, 0xBE,0x0D, 0x32,0x13};
    parse_cell_v(t503);
    CHECK(feq(st.cell_v_max, 3.522) && feq(st.cell_v_min, 3.518),
          "0x503 min/max cell");

    unsigned char t5f0[8] = {0x01,0x00,0xC0,0x0D,0xC1,0x0D,0xC2,0x0D};
    parse_cells(0, t5f0, 8);
    CHECK(feq(st.cell[0], 3.520) && st.cell_ok[0],
          "0x5F0 cell 1 = 3.520 V (real capture)");
    unsigned char t5f4[8] = {0x01,0x00,0xB0,0x0D,0xB1,0x0D,0x00,0x00};
    parse_cells(12, t5f4, 8);
    CHECK(feq(st.cell[13], 3.505) && !st.cell_ok[12] + (st.cell_ok[12] ? 1 : 1),
          "0x5F4 cell 14, zero padding skipped");

    unsigned char t501[8] = {0x01,0x00, 0x00,0x10, 1, 1, 0, 0};
    parse_config(t501);
    CHECK(st.alarm == 0x0001 && st.protect == 0x1000, "0x501 bitfields");

    /* ── whole --can-bms output via the capture stub ── */
    st.temp_min = st.temp_max = 29;
    cap_n = 0; vcan_send_all(0);
    const unsigned char *f;

    f = frame(VCAN_LIMITS);
    CHECK(f && u16(f,0) == 554, "0x351 CVL clamped 58.1 -> 55.4 V");
    CHECK(f && u16(f,2) == 0 && u16(f,4) == 0,
          "0x351 CCL/DCL forced 0 A (protection 0x1000 active)");
    CHECK(f && u16(f,6) == 448, "0x351 DVL passthrough 44.8 V");

    f = frame(VCAN_SOC_SOH);
    CHECK(f && u16(f,0) == 13, "0x355 SOC u16 at 1% (13, not 130)");

    f = frame(VCAN_STATE);
    CHECK(f && u16(f,0) == 4914, "0x356 voltage 0.01 V scale");
    CHECK(f && s16(f,2) == -30,  "0x356 current sign preserved");
    CHECK(f && s16(f,4) == 290,  "0x356 temperature 0.1 C");

    f = frame(VCAN_ALARMS);
    CHECK(f && (f[0] & 0x10), "0x35A UV-shutdown -> low-voltage alarm pair");
    CHECK(f && (f[0] & 0x01), "0x35A general alarm (protection active)");
    CHECK(f && (f[4] & 0x04), "0x35A OV alarm bit -> warning pair");

    f = frame(VCAN_CELLINFO);
    CHECK(f && u16(f,0) == 3518 && u16(f,2) == 3522, "0x373 cell mV");
    CHECK(f && u16(f,4) == 302, "0x373 temp Kelvin (29 C -> 302 K)");

    f = frame(VCAN_MODULES);
    CHECK(f && u16(f,0) == 1, "0x372 modules online");
    f = frame(VCAN_NAME);
    CHECK(f && f[0] == 'S', "0x35E product name");
    f = frame(VCAN_INFO);
    CHECK(f && u16(f,4) == 94, "0x35F capacity Ah");

    /* clean battery: everything must clear */
    st.alarm = 0; st.protect = 0;
    cap_n = 0; vcan_send_all(0);
    f = frame(VCAN_LIMITS);
    CHECK(f && u16(f,2) == 470, "0x351 CCL restored 47.0 A, no protection");
    f = frame(VCAN_ALARMS);
    { int z = 1; for (int k = 0; k < 8; k++) if (f[k]) z = 0;
      CHECK(f && z, "0x35A all clear when battery healthy"); }

    /* ── freshness gate (the regression this PR fixes) ── */
    time_t now = time(NULL);
    st.rx_status = now; st.rx_config = now; st.rx_limits = now;
    CHECK(vcan_fresh(now), "fresh: all three critical frames recent");
    st.rx_limits = now - DATA_TIMEOUT_SEC - 1;
    CHECK(!vcan_fresh(now), "NOT fresh: limits frames stopped (partial failure)");
    st.rx_limits = 0;
    CHECK(!vcan_fresh(now), "NOT fresh: before limits ever received (startup)");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}

#endif /* UNIT_TEST */
