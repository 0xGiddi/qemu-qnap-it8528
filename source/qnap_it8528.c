// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU QNAP IT8528 Embedded Controller emulation
 *
 * Emulates the ITE IT8528 EC on QNAP NAS devices for testing with the 
 * qnap8528 Linux kernel module. The kernel module comminutes with this
 * device via 4 I/O ports, 2 for the chip ID detection and 2 for EC data.
 *
 * Version history:
 *  v1.0 - Initial version
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"   
#include "qom/object.h"
#include "monitor/monitor.h"
#include "qobject/qdict.h"
#include "qnap_it8528.h"

#define QNAP_IT8528_LOG_TAG        "QNAP-IT8528:"
#define QNAP_IT8528_LOG(fmt, ...) \
    do { qemu_log(QNAP_IT8528_LOG_TAG " " fmt "\n", ##__VA_ARGS__); } while (0)

#define QNAP_IT8528_ERROR(fmt, ...) \
    do { error_report(QNAP_IT8528_LOG_TAG " " fmt, ##__VA_ARGS__); } while (0)

#define QNAP_IT8528_WARN(fmt, ...) \
    do { warn_report(QNAP_IT8528_LOG_TAG " " fmt, ##__VA_ARGS__); } while (0)



#define QNAP_IT8528_DEFAULT_CHIP_ID 0x8528
#define QNAP_IT8528_SIO_INDEX_PORT  0x2e
#define QNAP_IT8528_SIO_DATA_PORT   0x2f
#define QNAP_IT8528_EC_CMD_PORT     0x6c
#define QNAP_IT8528_EC_DATA_PORT    0x68

#define QNAP_IT8528_STATUS_OBF      BIT(0)
#define QNAP_IT8528_STATUS_IBF      BIT(1)
#define QNAP_IT8528_CMD_START       0x88
#define QNAP_IT8528_CMD_WRITE_FLAG  0x8000

#define QNAP_IT8528_REG_FILE_SIZE   0x700
#define QNAP_IT8528_VPD_NUM_TABLES  4
#define QNAP_IT8528_VPD_TABLE_SIZE  512

typedef enum {
    EC_PHASE_IDLE,
    EC_PHASE_CMD_HIGH,
    EC_PHASE_CMD_LOW,
    EC_PHASE_WRITE_DATA
} QNAPIT8528ECPhase;

OBJECT_DECLARE_SIMPLE_TYPE(QNAPIT8528State, QNAPIT8528)
struct QNAPIT8528State {
    ISADevice parent_obj;

    MemoryRegion sio_io;
    uint8_t sio_index;
    uint16_t sio_chip_id;

    MemoryRegion cmd_io;
    MemoryRegion data_io;
    QNAPIT8528ECPhase phase;
    uint8_t status;
    uint8_t output;
    uint16_t cmd;
    uint8_t regs[QNAP_IT8528_REG_FILE_SIZE];
    uint8_t vpd_tables[QNAP_IT8528_VPD_NUM_TABLES][QNAP_IT8528_VPD_TABLE_SIZE];
    uint16_t vpd_offsets[QNAP_IT8528_VPD_NUM_TABLES];

    uint32_t led_disk_present; // Static green
    uint32_t led_disk_active;  // Blinking Green
    uint32_t led_disk_error;   // Static Red
    uint32_t led_disk_locate;  // Blinking Red(ish)

    char *regs_path;
    char *vpd_path;

    QEMUTimer *button_timer;
    uint8_t buttons;
};

static const uint16_t vpd_regs[QNAP_IT8528_VPD_NUM_TABLES][3] = {
    {0x56, 0x57, 0x58},
    {0x59, 0x5a, 0x5b},
    {0x5c, 0x5d, 0x5e},
    {0x60, 0x61, 0x62}
};

struct QNAPIT8528RegInfo {
    uint16_t reg;
    const char *name;
    const char *desc;
};

static const struct QNAPIT8528RegInfo qnap_it8528_reg_info[] = {
    {0x16, "PWR_RECOVERY", "AC power recovery mode"},
    {0x56, "VPD_T0_OFFS_H", "VPD table 0 offset high byte"},
    {0x57, "VPD_T0_OFFS_L", "VPD table 0 offset low byte"},
    {0x58, "VPD_T0_DATA", "VPD table 0 data read"},
    {0x59, "VPD_T1_OFFS_H", "VPD table 1 offset high byte"},
    {0x5a, "VPD_T1_OFFS_L", "VPD table 1 offset low byte"},
    {0x5b, "VPD_T1_DATA", "VPD table 1 data read"},
    {0x5c, "VPD_T2_OFFS_H", "VPD table 2 offset high byte"},
    {0x5d, "VPD_T2_OFFS_L", "VPD table 2 offset low byte"},
    {0x5e, "VPD_T2_DATA", "VPD table 2 data read"},
    {0x60, "VPD_T3_OFFS_H", "VPD table 3 offset high byte"},
    {0x61, "VPD_T3_OFFS_L", "VPD table 3 offset low byte"},
    {0x62, "VPD_T3_DATA", "VPD table 3 data read"},
    {0x101, "EUP_SUPPORT", "EuP support flags"},
    {0x121, "EUP_MODE", "EuP mode settings"},
    {0x143, "BUTTON_INPUT", "Button state"},
    {0x154, "LED_USB", "USB LED"},
    {0x155, "LED_STATUS", "STATUS LED"},
    {0x156, "LED_JBOD", "JBOD LED"},
    {0x157, "LED_DISK_ACT_OFF", "Disk activity LED off"},
    {0x158, "LED_DISK_LOC_ON", "Disk locate LED on"},
    {0x159, "LED_DISK_LOC_OFF", "Disk locate LED off"},
    {0x15a, "LED_DISK_PRES_ON", "Disk present LED on"},
    {0x15b, "LED_DISK_PRES_OFF", "Disk present LED off"},
    {0x15c, "LED_DISK_ERR_ON", "Disk error LED on"},
    {0x15d, "LED_DISK_ERR_OFF", "Disk error LED off"},
    {0x15e, "LED_IDENT", "Enclosure identify LED"},
    {0x15f, "LED_DISK_ACT_ON", "Disk activity LED on"},
    {0x167, "LED_10G", "10GbE LED"},
    {0x220, "FAN_PWM_MODE_BANK_0", "Fan PWM mode bank 0"},
    {0x221, "FAN_PWM_MODE_BANK_2", "Fan PWM mode bank 2"},
    {0x222, "FAN_PWM_MODE_BANK_3", "Fan PWM mode bank 3"},
    {0x223, "FAN_PWM_MODE_BANK_1", "Fan PWM mode bank 1"},
    {0x22e, "FAN_PWM_VAL_BANK_0", "Fan PWM value bank 0"},
    {0x22f, "FAN_PWM_VAL_BANK_2", "Fan PWM value bank 2"},
    {0x23b, "FAN_PWM_VAL_BANK_3", "Fan PWM value bank 3"},
    {0x24b, "FAN_PWM_VAL_BANK_1", "Fan PWM value bank 1"},
    {0x242, "FAN_STATUS_BANK_0", "Fan status bank 0 (fans 0-5)"},
    {0x243, "PANEL_BRIGHT_A", "Panel brightness value A"},
    {0x244, "FAN_STATUS_BANK_1", "Fan status bank 1 (fans 6-7)"},
    {0x245, "PANEL_BRIGHT_B", "Panel brightness control register"},
    {0x246, "PANEL_BRIGHT_C", "Panel brightness value C"},
    {0x259, "FAN_STATUS_BANK_2", "Fan status bank 2 (fans 20-25)"},
    {0x25a, "FAN_STATUS_BANK_3", "Fan status bank 3 (fans 30-35)"},
    {0x308, "FW_VERSION_0", "Firmware version byte 0" },
    {0x309, "FW_VERSION_1", "Firmware version byte 1" },
    {0x30a, "FW_VERSION_2", "Firmware version byte 2" },
    {0x30b, "FW_VERSION_3", "Firmware version byte 3" },
    {0x30c, "FW_VERSION_4", "Firmware version byte 4" },
    {0x30d, "FW_VERSION_5", "Firmware version byte 5" },
    {0x30e, "FW_VERSION_6", "Firmware version byte 6" },
    {0x30f, "FW_VERSION_7", "Firmware version byte 7" },
    {0x320, "CPLD_VERSION", "CPLD version register"},
    {0, NULL, NULL}
};

static QNAPIT8528State *qnap_it8528_global;


static bool it8528_fan_to_regs(unsigned int fan, uint16_t *rh, uint16_t *rl)
{
    if (fan <= 5) {
        *rh = (fan + 0x312) * 2;
        *rl = (fan * 2) + 0x625;
        return true;
    }
    if (fan == 6 || fan == 7) {
        *rh = (fan + 0x30a) * 2;
        *rl = ((fan - 6) * 2) + 0x621;
        return true;
    }
    if (fan == 0x0a) { *rh = 0x65b; *rl = 0x65a; return true; }
    if (fan == 0x0b) { *rh = 0x65e; *rl = 0x65d; return true; }
    if (fan >= 0x14 && fan <= 0x19) {
        *rh = (fan + 0x30e) * 2;
        *rl = ((fan - 0x14) * 2) + 0x645;
        return true;
    }
    if (fan >= 0x1e && fan <= 0x23) {
        *rh = (fan + 0x2f8) * 2;
        *rl = ((fan - 0x1e) * 2) + 0x62d;
        return true;
    }
    return false;
}

static const char *qnap_it8528_reg_name(uint16_t reg)
{
    static char dynamic_name[32];
    int i;

    /* Check static table first */
    for (i = 0; qnap_it8528_reg_info[i].name; i++) {
        if (qnap_it8528_reg_info[i].reg == reg) {
            return qnap_it8528_reg_info[i].name;
        }
    }

    /* Temperature sensor registers */
    static const struct { uint16_t reg; int sensor; } temp_map[] = {
        {0x600, 0}, {0x601, 1},
        {0x602, 5}, {0x603, 6}, {0x604, 7},
        {0x659, 0x0a}, {0x65c, 0x0b},
    };
    for (i = 0; i < (int)ARRAY_SIZE(temp_map); i++) {
        if (temp_map[i].reg == reg) {
            snprintf(dynamic_name, sizeof(dynamic_name),
                     "TEMP_SENSOR_%d", temp_map[i].sensor);
            return dynamic_name;
        }
    }

    if (reg >= 0x606 && reg <= 0x61d) {
        int sensor = reg - 0x5f7;   /* inverse of 0x5f7 + sensor */
        if (sensor >= 0x0f && sensor <= 0x26) {
            snprintf(dynamic_name, sizeof(dynamic_name),
                     "TEMP_SENSOR_%d", sensor);
            return dynamic_name;
        }
    }

    /* Fan RPM register (high and lo) */
    static const struct { int fan; } fan_list[] = {
        {0},{1},{2},{3},{4},{5},
        {6},{7},
        {0x0a},{0x0b},
        {0x14},{0x15},{0x16},{0x17},{0x18},{0x19},
        {0x1e},{0x1f},{0x20},{0x21},{0x22},{0x23},
    };
    for (i = 0; i < (int)ARRAY_SIZE(fan_list); i++) {
        uint16_t rh, rl;
        if (it8528_fan_to_regs(fan_list[i].fan, &rh, &rl)) {
            if (reg == rh) {
                snprintf(dynamic_name, sizeof(dynamic_name),
                         "FAN%d_RPM_H", fan_list[i].fan);
                return dynamic_name;
            }
            if (reg == rl) {
                snprintf(dynamic_name, sizeof(dynamic_name),
                         "FAN%d_RPM_L", fan_list[i].fan);
                return dynamic_name;
            }
        }
    }

    return NULL;
}

static int qnap_it8528_vpd_reg_lookup(uint16_t reg, int *position) {
    int i, j;

    for (i = 0; i < QNAP_IT8528_VPD_NUM_TABLES; i++) {
        for (j = 0; j < 3; j++) {
            if (vpd_regs[i][j] == reg) {
                *position = j;
                return i;
            }
        }
    }
    return -1;
}

static uint8_t qnap_it8528_read_register(QNAPIT8528State *s, uint16_t reg) {
    int vpd_table, vpd_reg_pos;
    if (reg >= QNAP_IT8528_REG_FILE_SIZE) {
        QNAP_IT8528_WARN("Read from out of range reg 0x%04x", reg);
        return 0;
    }

    QNAP_IT8528_LOG("Read %s (reg=0x%04x val=%02x)", qnap_it8528_reg_name(reg), reg, s->regs[reg]);

    vpd_table = qnap_it8528_vpd_reg_lookup(reg, &vpd_reg_pos);
    if (vpd_table >= 0 && vpd_reg_pos == 2) {
        if (s->vpd_offsets[vpd_table] >= QNAP_IT8528_VPD_TABLE_SIZE) {
            QNAP_IT8528_WARN("VPD read from out of range offset table=%d offs=0x%04x", vpd_table, s->vpd_offsets[vpd_table]);;
            return 0;
        }
        return s->vpd_tables[vpd_table][s->vpd_offsets[vpd_table]];
    }

    return s->regs[reg];
}


static void qnap_it8528_write_register(QNAPIT8528State *s, uint16_t reg, uint8_t val) {
    int vpd_table, vpd_reg_pos;

     if (reg >= QNAP_IT8528_REG_FILE_SIZE) {
        QNAP_IT8528_WARN("Write from out of range reg 0x%04x", reg);
        return;
    }

    QNAP_IT8528_LOG("Write %s (reg=0x%04x val=%02x)", qnap_it8528_reg_name(reg), reg, val);

    vpd_table = qnap_it8528_vpd_reg_lookup(reg, &vpd_reg_pos);
    if (vpd_table >= 0) {
        switch (vpd_reg_pos) {
            case 0: /* HIGH OFFS */
                s->vpd_offsets[vpd_table] = val << 8;
                break;
            case 1:
                s->vpd_offsets[vpd_table] = s->vpd_offsets[vpd_table] | val;
                break;
            case 2:
                QNAP_IT8528_LOG("Unexpected write to VPD table=%d, offs=%04x",vpd_table, s->vpd_offsets[vpd_table]);
                break;
        }
    }
    s->regs[reg] = val;

    // Update disk LEDs to keep track
    if (val < 32) {
        switch (reg) {
        case 0x15a: s->led_disk_present |=  BIT(val); break;
        case 0x15b: s->led_disk_present &= ~BIT(val); break;
        case 0x15c: s->led_disk_error   |=  BIT(val); break;
        case 0x15d: s->led_disk_error   &= ~BIT(val); break;
        case 0x158: s->led_disk_locate  |=  BIT(val); break;
        case 0x159: s->led_disk_locate  &= ~BIT(val); break;
        case 0x15f: s->led_disk_active  |=  BIT(val); break;
        case 0x157: s->led_disk_active  &= ~BIT(val); break;
        }
    }

    // Update all fan RPMs per bank PWM value
    static const struct {
        uint16_t pwm_reg;
        uint8_t fans[6];
        int nfans;
    } banks[] = {
        { 0x22e, {0,1,2,3,4,5}, 6 },
        { 0x24b, {6,7}, 2 },
        { 0x22f, {0x14,0x15,0x16,0x17,0x18,0x19}, 6 },
        { 0x23b, {0x1e,0x1f,0x20,0x21,0x22,0x23}, 6 },
    };

    for (int i = 0; i < 4; i++) {
        if (reg == banks[i].pwm_reg) {
            uint16_t rpm = (uint16_t)(val * 5000 / 100);
            for (int j = 0; j < banks[i].nfans; j++) {
                uint16_t rh, rl;
                if (it8528_fan_to_regs(banks[i].fans[j], &rh, &rl)) {
                    s->regs[rh] = (rpm >> 8) & 0xff;
                    s->regs[rl] = rpm & 0xff;
                }
            }
            break;
        }
    }
}

static void qnap_it8528_process_cmd(QNAPIT8528State *s) {
    if (s->cmd & 0x8000) {
        s->cmd &= ~0x8000;
        s->phase = EC_PHASE_WRITE_DATA;
        s->status &= ~BIT(1);
    } else {
        s->output = qnap_it8528_read_register(s, s->cmd);
        s->status |= BIT(0);
        s->status &= ~BIT(1);
        s->phase = EC_PHASE_IDLE;
    }
}

static uint64_t qnap_it8528_sio_read(void *opaque, hwaddr addr, unsigned size) {
    QNAPIT8528State *s = opaque;

    if (addr == 0)
        return s->sio_index;

    switch (s->sio_index) {
        case 0x20:
            return (s->sio_chip_id >> 8) & 0xff;
        case 0x21:
            return s->sio_chip_id & 0xff;
        default:
            return 0xff;
    }
}

static void qnap_it8528_sio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    QNAPIT8528State *s = opaque;

    if (addr == 0)
        s->sio_index = val & 0xff;
}

static uint64_t qnap_it8528_cmd_read(void *opaque, hwaddr addr, unsigned size) {
    QNAPIT8528State *s = opaque;
    return s->status;
}

static void qnap_it8528_cmd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    QNAPIT8528State *s = opaque;

    if ((val & 0xff) == 0x88) {
        s->phase = EC_PHASE_CMD_HIGH;
        s->status &= ~BIT(1);
    } else
        QNAP_IT8528_ERROR("Unexpected command 0x%04x (expected 0x88)",(unsigned int)(val & 0xff));
}

static uint64_t qnap_it8528_data_read(void *opaque, hwaddr addr, unsigned size) {
    QNAPIT8528State *s = opaque;
    uint8_t val = s->output;

    s->status &= ~BIT(0);
    s->output = 0;

    return val;
}

static void qnap_it8528_data_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    QNAPIT8528State *s = opaque;

    switch (s->phase) {
        case EC_PHASE_IDLE:
            QNAP_IT8528_ERROR("Unexpected data write in EC idle phase");
            break;
        case EC_PHASE_CMD_HIGH:
            s->cmd = (val & 0xff) << 8;
            s->phase = EC_PHASE_CMD_LOW;
            s->status &= ~BIT(1);
            break;
        case EC_PHASE_CMD_LOW:
            s->cmd |= (val & 0xff);
            qnap_it8528_process_cmd(s);
            break;
        case EC_PHASE_WRITE_DATA:
            qnap_it8528_write_register(s, s->cmd, (val & 0xff));
            s->status &= ~BIT(1);
            s->phase = EC_PHASE_IDLE;
            break;
    }

}

static const MemoryRegionOps qnap_it8528_sio_ops = {
    .read = qnap_it8528_sio_read,
    .write = qnap_it8528_sio_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN
};

static const MemoryRegionOps qnap_it8528_cmd_ops = {
    .read = qnap_it8528_cmd_read,
    .write = qnap_it8528_cmd_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN
};

static const MemoryRegionOps qnap_it8528_data_ops = {
    .read = qnap_it8528_data_read,
    .write = qnap_it8528_data_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN
};

static void qnap_it8528_init_registers(QNAPIT8528State *s) {
    FILE *f;
    size_t n;

    memset(s->regs, 0, QNAP_IT8528_REG_FILE_SIZE);
    
    if (!s->regs_path || !s->regs_path[0]) {
        QNAP_IT8528_WARN("No regs file, regs will be all zero");
        return;
    }

    f = fopen(s->regs_path, "rb");
    if (!f) {
        QNAP_IT8528_ERROR("Failed to open regs file '%s' (%s)", s->regs_path, strerror(errno));
        return;
    }

    n = fread(s->regs, 1, QNAP_IT8528_REG_FILE_SIZE, f);
    fclose(f);
    if (n < QNAP_IT8528_REG_FILE_SIZE)
        QNAP_IT8528_WARN("Regs file '%s' is truncated/empty, padded with zeros", s->regs_path);
}

static void qnap_it8528_init_vpd(QNAPIT8528State *s) {
    FILE *f;
    size_t n;

    memset(s->vpd_tables, 0, QNAP_IT8528_VPD_NUM_TABLES * QNAP_IT8528_VPD_TABLE_SIZE);
    
    if (!s->vpd_path || !s->vpd_path[0])
    {
        QNAP_IT8528_WARN("No VPD file, VPD will be all zero");
        return;
    }

    f = fopen(s->vpd_path, "rb");
    if (!f) {
        QNAP_IT8528_ERROR("Failed to open VPD file '%s' (%s)", s->vpd_path, strerror(errno));
        return;
    }

    n = fread(s->vpd_tables, 1, QNAP_IT8528_VPD_NUM_TABLES * QNAP_IT8528_VPD_TABLE_SIZE, f);
    fclose(f);
    if (n < (QNAP_IT8528_VPD_NUM_TABLES * QNAP_IT8528_VPD_TABLE_SIZE))
        QNAP_IT8528_WARN("VPD file '%s' is truncated/empty, padded with zeros", s->vpd_path);
}

static void qnap_it8528_press_button(QNAPIT8528State *s, uint8_t mask, int duration) {
    s->regs[0x143] |= mask;
    s->buttons = mask;

    timer_mod(s->button_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + duration);
}

static void qnap_it8528_button_timer_cb(void *opaque) {
    QNAPIT8528State *s = opaque;
    s->regs[0x143] &= ~s->buttons;

    if (s->buttons & BIT(0))
        QNAP_IT8528_LOG("Released CHASSIS button");
    if (s->buttons & BIT(1))
        QNAP_IT8528_LOG("Released COPY button");
    if (s->buttons & BIT(2))
        QNAP_IT8528_LOG("Released RESET button");

    s->buttons = 0;
}

void qnap_it8528_hmp_press(Monitor *mon, const QDict *qdict)
{
    const char *button = qdict_get_str(qdict, "button");
    int duration = qdict_get_int(qdict, "duration");
    uint8_t mask = 0;

    if (!qnap_it8528_global) {
        monitor_printf(mon, "QNAP IT8528 device not present\n");
        return;
    }

    if (!strcasecmp(button, "CHASSIS")) mask = BIT(0);
    else if (!strcasecmp(button, "COPY")) mask = BIT(1);
    else if (!strcasecmp(button, "RESET")) mask = BIT(2);
    else { monitor_printf(mon, "Unknown button '%s'\n", button); return; }

    qnap_it8528_press_button(qnap_it8528_global, mask, duration);
}

static void qnap_it8528_realize(DeviceState *ds, Error **errp) {
    ISADevice *isa = ISA_DEVICE(ds);
    QNAPIT8528State *s = QNAPIT8528(ds);

    memory_region_init_io(&s->sio_io, OBJECT(s), &qnap_it8528_sio_ops, s, "qnap-it8528-sio", 2);
    isa_register_ioport(isa, &s->sio_io, QNAP_IT8528_SIO_INDEX_PORT);

    memory_region_init_io(&s->cmd_io, OBJECT(s), &qnap_it8528_cmd_ops, s, "qnap-it8528-cmd", 1);
    isa_register_ioport(isa, &s->cmd_io, QNAP_IT8528_EC_CMD_PORT);

    memory_region_init_io(&s->data_io, OBJECT(s), &qnap_it8528_data_ops, s, "qnap-it8528-data", 1);
    isa_register_ioport(isa, &s->data_io, QNAP_IT8528_EC_DATA_PORT);

    qnap_it8528_init_registers(s);
    qnap_it8528_init_vpd(s);
    
    s->sio_index = 0;
    s->phase = EC_PHASE_IDLE;
    s->status = 0;
    s->output = 0;
    s->led_disk_present = 0;
    s->led_disk_active = 0;
    s->led_disk_error = 0;
    s->led_disk_locate = 0;
    s->buttons = 0;
    s->button_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, qnap_it8528_button_timer_cb, s);

    qnap_it8528_global = s;

    QNAP_IT8528_LOG("QNAP EC emulation device realized");
    QNAP_IT8528_LOG("SuperIO Chip ID = %04x", s->sio_chip_id);
}

static void qnap_it8528_unrealize(DeviceState *ds) {
    QNAPIT8528State *s = QNAPIT8528(ds);

    if (s->button_timer) {
        timer_free(s->button_timer);
        s->button_timer = NULL;
    }
    
    if (qnap_it8528_global == s)
        qnap_it8528_global = NULL;
}

static const Property qnap_it8528_properties[] = {
    DEFINE_PROP_STRING("vpd-file", QNAPIT8528State, vpd_path),
    DEFINE_PROP_STRING("regs-file", QNAPIT8528State, regs_path),
    DEFINE_PROP_UINT16("chip-id", QNAPIT8528State, sio_chip_id, QNAP_IT8528_DEFAULT_CHIP_ID),
};

static void qnap_it8528_class_init(ObjectClass *oc, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = qnap_it8528_realize;
    dc->unrealize = qnap_it8528_unrealize;
    dc->desc = "QNAP IT8528 Embedded Controller";
    device_class_set_props(dc, qnap_it8528_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo qnap_it8528_type_info = {
    .name = TYPE_QNAPIT8528,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(QNAPIT8528State),
    .class_init = qnap_it8528_class_init
};

static void qnap_it8528_register_types(void) {
    type_register_static(&qnap_it8528_type_info);
}
type_init(qnap_it8528_register_types)

static uint16_t it8528_temp_sensor_to_reg(unsigned int sensor)
{
    if (sensor <= 1)                        return 0x600 + sensor;
    if (sensor >= 5 && sensor <= 7)         return 0x5fd + sensor;
    if (sensor == 0x0a)                     return 0x659;
    if (sensor == 0x0b)                     return 0x65c;
    if (sensor >= 0x0f && sensor <= 0x26)   return 0x5f7 + sensor;
    return 0;
}


void qnap_it8528_hmp_info(Monitor *mon, const QDict *qdict) {
    static const char *phase_names[] = {"IDLE", "CMD_HIGH", "CMD_LOW", "WRITE_DATA"};
    QNAPIT8528State *s = qnap_it8528_global;
    int i;
    uint16_t reg;

    if (!s) {
        monitor_printf(mon, "QNAP IT8528 device not present\n");
        return;
    }

    monitor_printf(mon, "QNAP IT8528 QNAP EC Info\n");
    monitor_printf(mon, "Firmware: ");
    for (i = 0; i < 8; i++)
        monitor_printf(mon, "%c", s->regs[0x308 + i]);
    monitor_printf(mon, "\n");
    monitor_printf(mon, "CPLD version: 0x%02x\n",s->regs[0x320]);
    monitor_printf(mon, "EC phase: %s, status=0x%02x (OBF=%d IBF=%d)\n", phase_names[s->phase], s->status, !!(s->status & BIT(0)), !!(s->status & BIT(1)));
    monitor_printf(mon, "Power recovery: %d, EuP mode: 0x%02x\n", s->regs[0x16], s->regs[0x121]);
    monitor_printf(mon, "System LEDs: status=%d usb=%d ident=%d jbod=%d 10g=%d\n", s->regs[0x155], s->regs[0x154], s->regs[0x15e], s->regs[0x156], s->regs[0x167]);
    monitor_printf(mon, "Disk LEDs (active):\n");
    for (i = 0; i < 32; i++) {
        uint32_t bit = BIT(i);
        bool present = !!(s->led_disk_present & bit);
        bool error   = !!(s->led_disk_error   & bit);
        bool locate  = !!(s->led_disk_locate  & bit);
        bool active  = !!(s->led_disk_active  & bit);
        if (present | error | locate | active)
            monitor_printf(mon, "    ec index %2d: present=%d error=%d locate=%d active=%d\n", i, present, error, locate, active);
    }
    monitor_printf(mon, "Buttons (regs): chassis=%d copy=%d reset=%d\n", !!(s->regs[0x143] & BIT(0)), !!(s->regs[0x143] & BIT(1)), !!(s->regs[0x143] & BIT(2)));
    monitor_printf(mon, "Buttons (pressed): chassis=%d copy=%d reset=%d\n", !!(s->buttons & BIT(0)), !!(s->buttons & BIT(1)), !!(s->buttons & BIT(2)));
    monitor_printf(mon, "Temperature sensors (value > 0):\n");
    for (i = 0; i <= 0x26; i++) {
        reg = it8528_temp_sensor_to_reg(i);
        if (reg && reg < QNAP_IT8528_REG_FILE_SIZE) {
            if (s->regs[reg] > 0)
                monitor_printf(mon, "    sensor %2d (reg 0x%04x): %d°C\n", i, reg, s->regs[reg]);
        }
    }
    monitor_printf(mon, "Fan RPMs:\n");
    for (i = 0; i <= 0x23; i++) {
        uint16_t rh, rl;
        if (it8528_fan_to_regs(i, &rh, &rl) &&
            rh < QNAP_IT8528_REG_FILE_SIZE && rl < QNAP_IT8528_REG_FILE_SIZE) {
            uint16_t rpm = ((uint16_t)s->regs[rh] << 8) | s->regs[rl];
            if (rpm > 0) {
                monitor_printf(mon, "fan %2d (regs 0x%04x/0x%04x): ""%d RPM\n", i, rh, rl, rpm);
            }
        }
    }
    monitor_printf(mon, "Fan banks PWMs:\n");
    monitor_printf(mon, "    bank 0 (fans 0-5):   mode=0x%02x  pwm=%u%%\n", s->regs[0x220], s->regs[0x22e]);
    monitor_printf(mon, "    bank 1 (fans 6-7):   mode=0x%02x  pwm=%u%%\n", s->regs[0x223], s->regs[0x24b]);
    monitor_printf(mon, "    bank 2 (fans 20-25): mode=0x%02x  pwm=%u%%\n", s->regs[0x221], s->regs[0x22f]);
    monitor_printf(mon, "    bank 3 (fans 30-35): mode=0x%02x  pwm=%u%%\n", s->regs[0x222], s->regs[0x23b]);
}
