#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone, timedelta

VPD_ENTRIES = [
    (0x0010000f, "mb_manuf",   "Mainboard manufacturer",                "BTC Systems"),
    (0x0010007c, "mb_vendor",  "Mainboard vendor",                      "QNAP Systems"),
    (0x00100020, "mb_name",    "Mainboard name",                        "SATA-6G-MB"),
    (0x00200042, "mb_model",   "Mainboard model",                       "70-0Q07D0250"),
    (0x00100031, "mb_serial",  "Mainboard serial number",               "M1234567890"),
    (0x0203000b, "mb_date",    "Mainboard manufacture date (YYYYMMDD)", "20200101"),
    (0x001000c3, "enc_ser_mb", "Enclosure serial (MB source)",          "Q000000000M"),
    (0x001000d6, "enc_nick",   "Enclosure nickname",                    ""),
    (0x04100037, "bp_manuf",   "Backplane manufacturer",                "BTC Systems"),
    (0x04100094, "bp_vendor",  "Backplane vendor",                      "QNAP Systems"),
    (0x04100048, "bp_name",    "Backplane name",                        "LF-SATA-BP"),
    (0x0420006a, "bp_model",   "Backplane model",                       "70-1Q07N0200"),
    (0x04100059, "bp_serial",  "Backplane serial number",               "B1234567890"),
    (0x06030033, "bp_date",    "Backplane manufacture date (YYYYMMDD)", "20200102"),
    (0x0410001d, "enc_ser_bp", "Enclosure serial (BP source)",          "Q000000000B"),
]

BASE_DATE = datetime(2013, 1, 1, tzinfo=timezone.utc)

def decode_location(loc):
    return (loc >> 26) & 0x3, (loc >> 24) & 0x3, (loc >> 16) & 0xff, loc & 0xffff

def encode_field(value, parse_type, length):
    if parse_type == 2:
        minutes = int((datetime.strptime(value, "%Y%m%d").replace(tzinfo=timezone.utc) - BASE_DATE).total_seconds() // 60)
        return minutes.to_bytes(length, "little")
    return value.encode("ascii")[:length].ljust(length, b"\x00")

def read_tables(path):
    data = open(path, "rb").read()
    return [bytearray(data[i * 512:(i + 1) * 512]) for i in range(4)]

def write_tables(path, tables):
    open(path, "wb").write(b"".join(tables))

def apply_fields(tables, values):
    for loc, name, _, _ in VPD_ENTRIES:
        if name not in values: continue
        table, parse_type, length, offset = decode_location(loc)
        tables[table][offset:offset + length] = encode_field(values[name], parse_type, length)

def parse_assignments(pairs):
    return dict(p.split("=", 1) for p in pairs if "=" in p)

def cmd_create(path, pairs):
    tables = [bytearray(512) for _ in range(4)]
    apply_fields(tables, {name: default for _, name, _, default in VPD_ENTRIES})
    apply_fields(tables, parse_assignments(pairs))
    write_tables(path, tables)
    print(f"Created '{path}'")

def cmd_amend(path, pairs):
    tables = read_tables(path)
    apply_fields(tables, parse_assignments(pairs))
    write_tables(path, tables)
    print(f"Amended '{path}'")

def decode_field(raw, parse_type):
    if parse_type == 2:
        return (BASE_DATE + timedelta(minutes=int.from_bytes(raw, "little"))).strftime("%Y-%m-%d %H:%M UTC")
    return raw.rstrip(b"\x00").decode("ascii") or "<empty>"

def cmd_dump(path):
    tables = read_tables(path)
    print(f"VPD dump of '{path}'")
    for loc, name, _, _ in VPD_ENTRIES:
        table, parse_type, length, offset = decode_location(loc)
        if offset + length > 512:
            print(f"  {name}: <out of range>")
        else:
            print(f"  {name}: {decode_field(bytes(tables[table][offset:offset + length]), parse_type)}")

def main():
    parser = argparse.ArgumentParser(prog="vpdutil")
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