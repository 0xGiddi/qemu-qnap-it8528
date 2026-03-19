#!/usr/bin/env python3
import argparse

REG_FILE_SIZE = 0x700
FW_VER_OFFSET  = 0x308
FW_VER_LEN     = 8
FW_VER_DEFAULT = "QVEC0000"
SIMPLE_REGS = [
    (0x016, "pwr_recovery", "AC power recovery (0=off 1=on 2=last)", 2),
    (0x101, "eup_support", "EuP support flags", 0x08),
    (0x320, "cpld_ver", "CPLD version", 0x13),
    (0x155, "led_status", "Status LED (0=off 1=green 2=red 3=gblink 4=rblink 5=bicolor)", 1),
    (0x154, "led_usb", "USB LED (0=off 1=blink 2=on)", 0),
    (0x15e, "led_ident", "Ident LED (1=on 2=off)", 0),
    (0x156, "led_jbod", "JBOD LED (0=off 1=on)", 0),
    (0x167, "led_10g", "10GbE LED (0=off 1=on)", 0),
]
FAN_BANKS = [
    (0x220, 0x22e, list(range(0, 6))),       # bank 0: fans 0-5
    (0x223, 0x24b, [6, 7]),                  # bank 1: fans 6-7
    (0x221, 0x22f, list(range(0x14,0x1a))),  # bank 2: fans 0x14-0x19
    (0x222, 0x23b, list(range(0x1e,0x24))),  # bank 3: fans 0x1e-0x23
]
VALID_TEMP_IDS = [0, 1, 5, 6, 7, 0xa, 0xb] + list(range(0xf, 0x27))
SIMPLE_BY_NAME = {name: (off, default) for off, name, _, default in SIMPLE_REGS}

# Fan ID to bank index, or None
def fan_bank(fan):
    for i, (_, _, fans) in enumerate(FAN_BANKS):
        if fan in fans: return i
    return None

# Temperature sensor ID to register
def temp_reg(sid):
    if sid <= 1:            return 0x600 + sid
    if 5 <= sid <= 7:       return 0x5fd + sid
    if sid == 0x0a:         return 0x659
    if sid == 0x0b:         return 0x65c
    if 0x0f <= sid <= 0x26: return 0x5f7 + sid
    return None


# Fan ID to RPM reg_high, reg_low
def fan_rpm_regs(fan):
    if fan <= 5:              return (fan + 0x312) * 2,     (fan * 2) + 0x625
    if fan in (6, 7):         return (fan + 0x30a) * 2,     ((fan - 6) * 2) + 0x621
    if fan == 0x0a:           return 0x65b, 0x65a
    if fan == 0x0b:           return 0x65e, 0x65d
    if 0x14 <= fan <= 0x19:   return (fan + 0x30e) * 2,     ((fan - 0x14) * 2) + 0x645
    if 0x1e <= fan <= 0x23:   return (fan + 0x2f8) * 2,     ((fan - 0x1e) * 2) + 0x62d
    return None

# Fan ID to status (reg, bit); bit=0 means present
def fan_status_reg_bit(fan):
    if fan <= 5:              return 0x242, fan
    if fan in (6, 7):         return 0x244, fan - 6
    if 0x14 <= fan <= 0x19:   return 0x259, fan - 0x14
    if 0x1e <= fan <= 0x23:   return 0x25a, fan - 0x1e
    return None

# PWM 0-100 to RPM 0-5000 and vice versa
def pwm_to_rpm(pwm): return round(pwm * 5000 / 100)
def rpm_to_pwm(rpm): return min(100, round(rpm * 100 / 5000))

def read_regs(path):
    data = open(path, "rb").read()
    assert len(data) == REG_FILE_SIZE, f"Expected {REG_FILE_SIZE} bytes, got {len(data)}"
    return bytearray(data)

def write_regs(path, regs): open(path, "wb").write(regs)

def set_fan_rpm(regs, fan_id, rpm):
    pair = fan_rpm_regs(fan_id)
    if not pair: return
    rh, rl = pair
    regs[rh] = (rpm >> 8) & 0xff
    regs[rl] = rpm & 0xff
    # back-compute PWM for the bank this fan belongs to
    bidx = fan_bank(fan_id)
    if bidx is not None:
        mode_reg, pwm_reg, _ = FAN_BANKS[bidx]
        regs[mode_reg] = 0x10
        regs[pwm_reg]  = rpm_to_pwm(rpm)

def set_fan_status(regs, fan_id, present):
    pair = fan_status_reg_bit(fan_id)
    if not pair: return
    reg, bit = pair
    if present: regs[reg] &= ~(1 << bit)
    else:       regs[reg] |=  (1 << bit)

def set_pwm_bank(regs, bank_idx, pwm):
    pwm = max(0, min(100, pwm))
    mode_reg, pwm_reg, fans = FAN_BANKS[bank_idx]
    regs[mode_reg] = 0x10
    regs[pwm_reg]  = pwm
    rpm = pwm_to_rpm(pwm)
    for fan in fans:
        pair = fan_rpm_regs(fan)
        if not pair: continue
        rh, rl = pair
        regs[rh] = (rpm >> 8) & 0xff
        regs[rl] = rpm & 0xff

def apply_fields(regs, values):
    for name, raw in values.items():
        # fw_ver is a string, handle before int() conversion
        if name == "fw_ver":
            regs[FW_VER_OFFSET:FW_VER_OFFSET + FW_VER_LEN] = raw.encode("ascii")[:FW_VER_LEN].ljust(FW_VER_LEN, b"\x00")
            continue
        val = int(raw, 0)
        # simple named registers
        if name in SIMPLE_BY_NAME:
            off, _ = SIMPLE_BY_NAME[name]
            regs[off] = val & 0xff
            continue
        # eup_mode: stored as bit 3 (0x08=on, 0x00=off)
        if name == "eup_mode":
            regs[0x121] = 0x08 if val else 0x00
            continue
        # panel brightness: two-phase commit via 0x245, write A and C identically
        if name == "panel_bright":
            regs[0x243] = val & 0xff
            regs[0x246] = val & 0xff
            regs[0x245] = 0xff # Does not really matter
            continue
        # fan_pwm_<0-3>=<0-100>
        if name.startswith("fan_pwm_") and name[-1].isdigit():
            bidx = int(name[-1])
            if 0 <= bidx <= 3: set_pwm_bank(regs, bidx, val)
            continue
        # temp<id>=<celsius>
        if name.startswith("temp"):
            reg = temp_reg(int(name[4:], 0))
            if reg: regs[reg] = val & 0x7f
            continue
        # fan<id>_rpm=<rpm> sets RPM regs and back-computes PWM bank
        if name.startswith("fan") and name.endswith("_rpm"):
            set_fan_rpm(regs, int(name[3:-4], 0), val)
            continue
        # fan<id>_status=<0|1> sets status bit (0=present, 1=absent)
        if name.startswith("fan") and name.endswith("_status"):
            set_fan_status(regs, int(name[3:-7], 0), val == 0)
            continue
        print(f"WARNING: unknown field '{name}'")

def parse_assignments(pairs):
    return dict(p.split("=", 1) for p in pairs if "=" in p)

def cmd_dump(path):
    regs = read_regs(path)
    print(f"Registers dump of '{path}'")
    fw = regs[FW_VER_OFFSET:FW_VER_OFFSET + FW_VER_LEN].rstrip(b"\x00").decode("ascii")
    print(f"  {'fw_ver':<20} {fw}")
    print(f"  {'eup_mode':<20} {1 if regs[0x121] & 0x08 else 0:#04x}  (EuP mode 0=off 1=on)")
    print(f"  {'panel_bright':<20} {regs[0x243]:#04x}  (0-100)")
    for off, name, desc, _ in SIMPLE_REGS:
        print(f"  {name:<20} {regs[off]:#04x}  ({desc})")
    print("--- fan banks ---")
    for bidx, (mode_reg, pwm_reg, fans) in enumerate(FAN_BANKS):
        pwm = regs[pwm_reg]
        print(f"  bank{bidx}  mode={regs[mode_reg]:#04x}  pwm={pwm:3d}/100  (~{pwm_to_rpm(pwm)} RPM computed)")
    print("--- fan RPMs ---")
    for _, _, fans in FAN_BANKS:
        for fid in fans:
            pair = fan_rpm_regs(fid)
            if not pair: continue
            rh, rl = pair
            rpm = (regs[rh] << 8) | regs[rl]
            if rpm: print(f"fan{fid:<5} {rpm:5d} RPM  (~pwm {rpm_to_pwm(rpm)})")
    print("--- temperatures ---")
    for sid in VALID_TEMP_IDS:
        reg = temp_reg(sid)
        val = regs[reg]
        if val: print(f"temp{sid:<5} (reg {reg:#06x})  {val} °C")

def cmd_create(path, pairs):
    regs = bytearray(REG_FILE_SIZE)
    regs[FW_VER_OFFSET:FW_VER_OFFSET + FW_VER_LEN] = FW_VER_DEFAULT.encode("ascii")
    for off, _, _, default in SIMPLE_REGS:
        regs[off] = default
    # Fan defaults
    regs[0x242] = 0x01
    regs[0x244] = 0x01
    set_fan_rpm(regs, 0, 554)
    set_fan_rpm(regs, 6,  2221)
    # Temperature defaults
    regs[temp_reg(0)]  = 30
    regs[temp_reg(5)]  = 35
    regs[temp_reg(6)]  = 36
    regs[temp_reg(7)] = 37
    apply_fields(regs, parse_assignments(pairs))
    write_regs(path, regs)
    print(f"Created '{path}'")

def cmd_amend(path, pairs):
    regs = read_regs(path)
    apply_fields(regs, parse_assignments(pairs))
    write_regs(path, regs)
    print(f"Amended '{path}'")

def main():
    parser = argparse.ArgumentParser(
        prog="regsutil",
        epilog=(
            "Special fields:\n"
            "fw_ver=QE010000        firmware version string (8 chars)\n"
            "eup_mode=1             EuP mode (0=off, 1=om stored as bit 3)\n"
            "panel_bright=75        panel brightness 0-100 (writes A, B commit, C)\n"
            "fan_pwm_<0-3>=<0-100>  sets PWM (0-100) + mode reg + computes RPM for bank\n"
            "fan<id>_rpm=<rpm>      sets RPM regs and back-computes PWM for that bank\n"
            "fan<id>_status=<0|1>   sets status bit (0=present, 1=absent)\n"
            "temp<id>=<celsius>     e.g. temp0=45  (valid IDs: 0,1,5-7,10,11,15-38)\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="cmd")
    sub.required = True

    p = sub.add_parser("dump");   p.add_argument("file")
    p = sub.add_parser("create"); p.add_argument("file"); p.add_argument("fields", nargs="*")
    p = sub.add_parser("amend");  p.add_argument("file"); p.add_argument("fields", nargs="+")

    args = parser.parse_args()
    if args.cmd == "dump":   cmd_dump(args.file)
    if args.cmd == "create": cmd_create(args.file, args.fields)
    if args.cmd == "amend":  cmd_amend(args.file, args.fields)

if __name__ == "__main__":
    main()
