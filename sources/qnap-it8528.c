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


#define QNAP_IT8528_LOG_TAG        "QNAP-IT8528:"
#define QNAP_IT8528_LOG(fmt, ...) \
    do { qemu_log(QNAP_IT8528_LOG_TAG " " fmt "\n", ##__VA_ARGS__); } while (0)

#define QNAP_IT8528_ERROR(fmt, ...) \
    do { error_report(QNAP_IT8528_LOG_TAG " " fmt, ##__VA_ARGS__); } while (0)

#define QNAP_IT8528_WARN(fmt, ...) \
    do { warn_report(QNAP_IT8528_LOG_TAG " " fmt, ##__VA_ARGS__); } while (0)

#define TYPE_QNAP_IT8528           "qnap-it8528"

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

enum QNAPIT8528ECPhase {
    EC_PHASE_IDLE,
    EC_PHASE_CMD_HIGH,
    EC_PHASE_CMD_LOW,
    EC_PHASE_WRITE_DATA
};

OBJECT_DECLARE_SIMPLE_TYPE(IT8528State, QNAPIT8528)
struct QNAPIT8528State {
    ISADevice *parent_obj;

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

static QNAPIT8528State *qnap_it8528_global;

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
        QNAP_IT8528_WARN("Read from out of range reg 0x%04x")
        return 0;
    }

    vpd_table = qnap_it8528_vpd_reg_lookup(reg, &vpd_reg_pos);
    if (vpd_table >= 0 && vpd_reg_pos == 2) {
        if (s->vpd_offsets[vpd_table] >= QNAP_IT8528_VPD_TABLE_SIZE) {
            QNAP_IT8528_WARN("VPD read from out of range offset table=%d offs=0x%04x", vpd_table, s->vpd_offsets[vpd_table]);
            return 0;
        }
        return s->vpd_tables[vpd_table][s->vpd_offsets[vpd_table]];
    }

    return s->regs[reg];
}

static void qnap_it8528_write_register(QNAPIT8528State *s, uint16_t reg, uint8_t val) {
    int vpd_table, vpd_reg_pos;

     if (reg >= QNAP_IT8528_REG_FILE_SIZE) {
        QNAP_IT8528_WARN("Write from out of range reg 0x%04x")
        return;
    }

    QNAP_IT8528_LOG("Write reg=0x%04x val=%02x", reg, val)

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
                QNAP_IT8528_LOG("Unexpected write to VPD table=%d, offs=%04x",vpd_table, s->vpd_offsets[vpd_table])
                break;
        }
    }
    s->regs[reg] = val;
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
        QNAP_IT8528_ERROR("Unexpected command 0x%04x (expected 0x88)", (val & 0xff))
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
            QNAP_IT8528_ERROR("Unexpected data write in EC idle phase")
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
        QNAP_IT8528_WARN("No regs file, regs will be all zero")
        return;
    }

    f = fopen(s->regs_path, "rb");
    if (!f) {
        QNAP_IT8528_ERROR("Failed to open regs file '%s' (%s)", s->regs_path, strerror(errno))
        return;
    }

    n = fread(s->regs, 1, QNAP_IT8528_REG_FILE_SIZE, f);
    fclose(f);
    if (n < QNAP_IT8528_REG_FILE_SIZE)
        QNAP_IT8528_WARN("Regs file '%s' is truncated/empty, padded with zeros", s->regs_path)
}

static void qnap_it8528_init_vpd(QNAPIT8528State *s) {
    FILE *f;
    size_t n;

    memset(s->vpd_tables, 0, QNAP_IT8528_VPD_NUM_TABLES * QNAP_IT8528_VPD_TABLE_SIZE);
    
    if (!s->vpd_path || !s->vpd_path[0])
    {
        QNAP_IT8528_WARN("No VPD file, VPD will be all zero")
        return;
    }

    f = fopen(s->vpd_path, "rb");
    if (!f) {
        QNAP_IT8528_ERROR("Failed to open VPD file '%s' (%s)", s->vpd_path, strerror(errno))
        return;
    }

    n = fread(s->vpd_tables, 1, QNAP_IT8528_VPD_NUM_TABLES * QNAP_IT8528_VPD_TABLE_SIZE, f);
    fclose(f);
    if (n < (QNAP_IT8528_VPD_NUM_TABLES * QNAP_IT8528_VPD_TABLE_SIZE))
        QNAP_IT8528_WARN("VPD file '%s' is truncated/empty, padded with zeros", s->vpd_path)
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
        QNAP_IT8528_LOG("Released CHASSIS button")
    if (s->buttons & BIT(1))
        QNAP_IT8528_LOG("Released COPY button")
    if (s->buttons & BIT(2))
        QNAP_IT8528_LOG("Released RESET button")

    s->buttons = 0;
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
    
    s->button_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, qnap_it8528_button_timer_cb, s);

    qnap_it8528_global = s;

    QNAP_IT8528_LOG("QNAP EC emulation device realized")
    QNAP_IT8528_LOG("SuperIO Chip ID = %04x", s->sio_chip_id)
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

static Property qnap_it8528_properties[] = {
    DEFINE_PROP_STRING("vpd-file", QNAPIT8528State, vpd_path),
    DEFINE_PROP_STRING("regs-file", QNAPIT8528State, regs_path),
    DEFINE_PROP_UINT16("chip-id", QNAPIT8528State, sio_chip_id, QNAP_IT8528_DEFAULT_CHIP_ID),
    DEFINE_PROP_END_OF_LIST()
};

static void qnap_it8528_class_init(ObjectClass *oc, void *data) {
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = qnap_it8528_realize;
    dc->unrealize = qnap_it8528_unrealize;
    dc->desc = "QNAP IT8528 Embedded Controller";
    device_class_set_props(dc, qnap_it8528_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo qnap_it8528_type_info = {
    .name = TYPE_QNAP_IT8528,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(QNAPIT8528State),
    .class_init = qnap_it8528_class_init
};

static void qnap_it8528_register_types(void) {
    type_register_static(&qnap_it8528_type_info);
}
type_init(qnap_it8528_register_types)
















