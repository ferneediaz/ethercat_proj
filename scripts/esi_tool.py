#!/usr/bin/env python3
"""
Turn the ESI into the two things that have to agree with it: the EEPROM
image the master reads at scan time, and the constants the slave firmware
compiles against.

    ./scripts/esi_tool.py header  -o firmware/soes/esi_generated.h
    ./scripts/esi_tool.py sii     --preserve-config backup.bin -o build/sii.bin
    ./scripts/esi_tool.py dump    build/sii.bin

Why a tool rather than three hand-maintained files: the EEPROM declares the
SyncManager lengths, the object dictionary declares the PDO mapping, and the
master declares a C struct. A master that finds those three disagreeing does
not limp — it refuses SAFEOP. They are the same facts written three ways, so
they are generated from one source.

    ESI XML ──┬─> esi_generated.h ─> slave_objectlist.c, pdo_layout.h
              └─> sii.bin ─────────> ESC EEPROM

No third-party imports on purpose: this has to run on a Pi that was set up by
scripts/pi-setup.sh, which installs no Python packages.

THE CONFIG AREA
---------------
Words 0..7 of the SII hold the PDI configuration, including which physical
interface the ESC exposes. On this board that is the SPI slave port the
ESP32-S3 is wired to. Write a wrong value there and the ESC stops answering
over SPI, which is the route you would use to fix it — recovery then needs an
external programmer on the EEPROM.

So by default this tool does not generate those words at all. It copies them
verbatim from a backup of the live device (--preserve-config). The CRC in
word 7 covers exactly that region, so copying the block keeps it valid without
recomputing anything. --use-esi-config exists for a board that is already
unreachable and has nothing left to lose.
"""

import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ESI = REPO / "esi" / "EthercatServoNode.xml"

# Category type codes, ETG.1000-6 Table 18.
CAT_STRINGS = 10
CAT_GENERAL = 30
CAT_FMMU = 40
CAT_SYNCM = 41
CAT_TXPDO = 50
CAT_RXPDO = 51
CAT_END = 0xFFFF

# CoE data type indices, used one byte wide inside PDO entries.
DATATYPES = {
    "BOOL": 0x01, "SINT": 0x02, "INT": 0x03, "DINT": 0x04,
    "USINT": 0x05, "UINT": 0x06, "UDINT": 0x07,
}

SM_TYPES = {"MBoxOut": 1, "MBoxIn": 2, "Outputs": 3, "Inputs": 4}
FMMU_TYPES = {"Outputs": 1, "Inputs": 2, "MBoxState": 3}

CONFIG_WORDS = 8            # words 0..7, the region we refuse to synthesise
CATEGORY_START_WORD = 0x40


def hexnum(text):
    """ESI writes numbers as #x1a2b or as decimal."""
    text = text.strip()
    if text.lower().startswith("#x"):
        return int(text[2:], 16)
    return int(text, 0)


class Esi:
    """The subset of the ESI this project actually uses."""

    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.vendor_id = hexnum(root.findtext("./Vendor/Id"))
        dev = root.find("./Descriptions/Devices/Device")
        if dev is None:
            raise SystemExit(f"{path}: no Device element")

        type_el = dev.find("Type")
        self.product_code = hexnum(type_el.get("ProductCode"))
        self.revision = hexnum(type_el.get("RevisionNo"))
        self.order_code = (type_el.text or "").strip()
        self.name = (dev.findtext("Name") or self.order_code).strip()
        self.group = (dev.findtext("GroupType") or "").strip()

        self.fmmus = [(e.text or "").strip() for e in dev.findall("Fmmu")]

        self.sms = []
        for sm in dev.findall("Sm"):
            self.sms.append({
                "type": (sm.text or "").strip(),
                "start": hexnum(sm.get("StartAddress")),
                "control": hexnum(sm.get("ControlByte")),
                "size": int(sm.get("DefaultSize", "0"), 0),
                "enable": int(sm.get("Enable", "1"), 0),
            })

        self.rxpdos = [self._pdo(p) for p in dev.findall("RxPdo")]
        self.txpdos = [self._pdo(p) for p in dev.findall("TxPdo")]

        self.eeprom_bytes = int(dev.findtext("./Eeprom/ByteSize") or "2048", 0)
        cfg = (dev.findtext("./Eeprom/ConfigData") or "").strip()
        self.config_data = bytes.fromhex(cfg) if cfg else b""

        mbox = dev.find("Mailbox")
        self.has_coe = mbox is not None and mbox.find("CoE") is not None

    @staticmethod
    def _pdo(el):
        entries = []
        for e in el.findall("Entry"):
            dt = (e.findtext("DataType") or "").strip()
            entries.append({
                "index": hexnum(e.findtext("Index")),
                "sub": int(e.findtext("SubIndex") or "0", 0),
                "bits": int(e.findtext("BitLen") or "0", 0),
                "name": (e.findtext("Name") or "").strip(),
                "type": DATATYPES.get(dt, 0) if dt else 0,
            })
        return {
            "index": hexnum(el.findtext("Index")),
            "name": (el.findtext("Name") or "").strip(),
            "sm": int(el.get("Sm", "0"), 0),
            "fixed": el.get("Fixed", "0") == "1",
            "entries": entries,
        }

    # -- derived facts, the ones everything downstream must agree on --------

    def pdo_bytes(self, pdos):
        bits = sum(e["bits"] for p in pdos for e in p["entries"])
        if bits % 8:
            raise SystemExit(f"PDO bit length {bits} is not a whole number of bytes")
        return bits // 8

    def sm(self, sm_type):
        for s in self.sms:
            if s["type"] == sm_type:
                return s
        raise SystemExit(f"ESI has no {sm_type} SyncManager")

    def check(self):
        """The consistency the ESI must have with itself before we emit
        anything. A mismatch here becomes a SAFEOP refusal on hardware, which
        is a far more expensive place to discover it."""
        out_bytes = self.pdo_bytes(self.rxpdos)
        in_bytes = self.pdo_bytes(self.txpdos)
        problems = []
        if self.sm("Outputs")["size"] != out_bytes:
            problems.append(
                f"SM2 declares {self.sm('Outputs')['size']} bytes but the "
                f"RxPDO entries total {out_bytes}")
        if self.sm("Inputs")["size"] != in_bytes:
            problems.append(
                f"SM3 declares {self.sm('Inputs')['size']} bytes but the "
                f"TxPDO entries total {in_bytes}")
        for p in self.rxpdos + self.txpdos:
            for e in p["entries"]:
                if e["index"] and not e["type"]:
                    problems.append(
                        f"PDO {p['index']:#06x} entry {e['index']:#06x}:"
                        f"{e['sub']} has an unrecognised DataType")
        if problems:
            raise SystemExit("ESI is inconsistent:\n  - " + "\n  - ".join(problems))
        return out_bytes, in_bytes


# --------------------------------------------------------------------------
# SII image
# --------------------------------------------------------------------------

class StringTable:
    """SII string indices are 1-based into the STRINGS category, and every
    other category refers to strings by that index."""

    def __init__(self):
        self.strings = []

    def add(self, s):
        if not s:
            return 0
        if s not in self.strings:
            self.strings.append(s)
        return self.strings.index(s) + 1

    def encode(self):
        body = bytes([len(self.strings)])
        for s in self.strings:
            raw = s.encode("ascii", "replace")
            if len(raw) > 255:
                raise SystemExit(f"string too long for SII: {s[:40]}...")
            body += bytes([len(raw)]) + raw
        return body


def category(code, data):
    if len(data) % 2:
        data += b"\x00"
    return struct.pack("<HH", code, len(data) // 2) + data


def build_sii(esi, config_words):
    esi.check()
    st = StringTable()

    # Strings must be registered before the categories that index them.
    group_idx = st.add(esi.group)
    order_idx = st.add(esi.order_code)
    name_idx = st.add(esi.name)
    pdo_name_idx = {}
    for p in esi.rxpdos + esi.txpdos:
        pdo_name_idx[p["index"]] = st.add(p["name"])
        for e in p["entries"]:
            e["name_idx"] = st.add(e["name"])

    strings_cat = category(CAT_STRINGS, st.encode())

    general = bytearray(32)
    general[0] = group_idx
    general[1] = 0                      # image index, none
    general[2] = order_idx
    general[3] = name_idx
    general[5] = 0x0F if esi.has_coe else 0x00   # CoE: SDO, SDO Info, PDO*
    general[10] = 0x01                  # SysmanClass / standard device
    general[11] = 0x01                  # Flags: Enable SafeOp
    struct.pack_into("<h", general, 12, 0)       # current on E-bus, mA
    struct.pack_into("<H", general, 16, 0x0303)  # both ports MII
    general_cat = category(CAT_GENERAL, bytes(general))

    fmmu_cat = category(CAT_FMMU, bytes(FMMU_TYPES[f] for f in esi.fmmus))

    sm_data = b""
    for s in esi.sms:
        sm_data += struct.pack(
            "<HHBBBB",
            s["start"], s["size"], s["control"],
            0,                       # status register, read-only, always 0
            s["enable"],
            SM_TYPES[s["type"]],
        )
    sm_cat = category(CAT_SYNCM, sm_data)

    def pdo_cat(pdos, code):
        out = b""
        for p in pdos:
            out += struct.pack(
                "<HBBBBH",
                p["index"], len(p["entries"]), p["sm"], 0,
                pdo_name_idx[p["index"]],
                0x0001 if p["fixed"] else 0x0000,
            )
            for e in p["entries"]:
                out += struct.pack(
                    "<HBBBBH",
                    e["index"], e["sub"], e["name_idx"],
                    e["type"], e["bits"], 0,
                )
        return category(code, out) if out else b""

    image = bytearray(b"\xff" * esi.eeprom_bytes)
    image[0:CONFIG_WORDS * 2] = config_words

    hdr = bytearray(b"\x00" * (CATEGORY_START_WORD * 2 - CONFIG_WORDS * 2))

    def put32(word, value):
        struct.pack_into("<I", hdr, word * 2 - CONFIG_WORDS * 2, value)

    def put16(word, value):
        struct.pack_into("<H", hdr, word * 2 - CONFIG_WORDS * 2, value)

    put32(0x08, esi.vendor_id)
    put32(0x0A, esi.product_code)
    put32(0x0C, esi.revision)
    put32(0x0E, 0)                      # serial number

    mbox_out, mbox_in = esi.sm("MBoxOut"), esi.sm("MBoxIn")
    put16(0x16, mbox_out["start"])
    put16(0x17, mbox_out["size"])
    put16(0x18, mbox_in["start"])
    put16(0x19, mbox_in["size"])
    put16(0x1A, 0x0004 if esi.has_coe else 0)    # mailbox protocol: CoE

    put16(0x3E, (esi.eeprom_bytes * 8) // 1024 - 1)   # size in kbit - 1
    put16(0x3F, 1)                                    # SII version

    image[CONFIG_WORDS * 2:CATEGORY_START_WORD * 2] = hdr

    cats = (strings_cat + general_cat + fmmu_cat + sm_cat
            + pdo_cat(esi.txpdos, CAT_TXPDO)
            + pdo_cat(esi.rxpdos, CAT_RXPDO)
            + struct.pack("<H", CAT_END))

    end = CATEGORY_START_WORD * 2 + len(cats)
    if end > esi.eeprom_bytes:
        raise SystemExit(
            f"image needs {end} bytes but the EEPROM is {esi.eeprom_bytes}")
    image[CATEGORY_START_WORD * 2:end] = cats
    return bytes(image)


def decode(image):
    """Read an SII image back out. Used by --check and by the dump command,
    so what we verify is the bytes themselves rather than our intent."""
    out = {
        "vendor": struct.unpack_from("<I", image, 0x08 * 2)[0],
        "product": struct.unpack_from("<I", image, 0x0A * 2)[0],
        "revision": struct.unpack_from("<I", image, 0x0C * 2)[0],
        "pdi": image[0],
        "mbox_out": struct.unpack_from("<H", image, 0x16 * 2)[0],
        "mbox_in": struct.unpack_from("<H", image, 0x18 * 2)[0],
        "sms": [], "pdos": [], "strings": [],
    }
    pos = CATEGORY_START_WORD * 2
    while pos + 4 <= len(image):
        code, words = struct.unpack_from("<HH", image, pos)
        if code == CAT_END or code == 0xFFFF:
            break
        data = image[pos + 4: pos + 4 + words * 2]
        if code == CAT_STRINGS and data:
            n, p = data[0], 1
            for _ in range(n):
                ln = data[p]
                out["strings"].append(data[p + 1:p + 1 + ln].decode("ascii", "replace"))
                p += 1 + ln
        elif code == CAT_SYNCM:
            for i in range(0, len(data), 8):
                start, ln, ctrl, _st, en, ty = struct.unpack_from("<HHBBBB", data, i)
                out["sms"].append({"start": start, "len": ln, "ctrl": ctrl,
                                   "enable": en, "type": ty})
        elif code in (CAT_TXPDO, CAT_RXPDO):
            p = 0
            while p + 8 <= len(data):
                idx, n, sm, _f, _nm, _fl = struct.unpack_from("<HBBBBH", data, p)
                p += 8
                entries = []
                for _ in range(n):
                    ei, es, _en, et, eb, _ef = struct.unpack_from("<HBBBBH", data, p)
                    entries.append({"index": ei, "sub": es, "bits": eb, "type": et})
                    p += 8
                out["pdos"].append({"dir": "Tx" if code == CAT_TXPDO else "Rx",
                                    "index": idx, "sm": sm, "entries": entries})
        pos += 4 + words * 2
    return out


# --------------------------------------------------------------------------
# C header
# --------------------------------------------------------------------------

HEADER_TEMPLATE = """\
/*
 * GENERATED FILE - DO NOT EDIT.
 *
 * Produced by scripts/esi_tool.py from esi/EthercatServoNode.xml.
 * Regenerate with:
 *
 *     ./scripts/esi_tool.py header -o firmware/soes/esi_generated.h
 *
 * These are the numbers the EEPROM image was built from. They describe the
 * device as the ESI declares it, which is only what the hardware reports
 * once that image has actually been written -- see device_identity.h for
 * the switch between this and the stock EEPROM the module ships with.
 */
#ifndef __ESI_GENERATED_H__
#define __ESI_GENERATED_H__

#define ESI_VENDOR_ID       {vendor:#010x}u
#define ESI_PRODUCT_CODE    {product:#010x}u
#define ESI_REVISION        {revision:#010x}u
#define ESI_DEVICE_NAME     "{order}"

/* SyncManager 2: master -> slave. */
#define ESI_SM2_ADDR        {sm2_addr:#06x}u
#define ESI_SM2_CONTROL     {sm2_ctrl:#04x}u
#define ESI_RXPDO_BYTES     {out_bytes}

/* SyncManager 3: slave -> master. */
#define ESI_SM3_ADDR        {sm3_addr:#06x}u
#define ESI_SM3_CONTROL     {sm3_ctrl:#04x}u
#define ESI_TXPDO_BYTES     {in_bytes}

/* Mapping entries, encoded as index<<16 | subindex<<8 | bitlength, in the
 * order the ESI lists them. The object dictionary must present exactly
 * these, and exactly this many. */
#define ESI_RXPDO_ENTRY_COUNT {n_rx}
#define ESI_TXPDO_ENTRY_COUNT {n_tx}
{mappings}
#endif /* __ESI_GENERATED_H__ */
"""


def emit_header(esi):
    out_bytes, in_bytes = esi.check()
    lines = []
    for tag, pdos in (("RX", esi.rxpdos), ("TX", esi.txpdos)):
        for p in pdos:
            for i, e in enumerate(p["entries"], 1):
                mapping = (e["index"] << 16) | (e["sub"] << 8) | e["bits"]
                lines.append(
                    f"#define ESI_{tag}PDO_ENTRY_{i}   {mapping:#010x}u"
                    f"  /* {e['name']} */")
    return HEADER_TEMPLATE.format(
        vendor=esi.vendor_id, product=esi.product_code, revision=esi.revision,
        order=esi.order_code,
        sm2_addr=esi.sm("Outputs")["start"], sm2_ctrl=esi.sm("Outputs")["control"],
        sm3_addr=esi.sm("Inputs")["start"], sm3_ctrl=esi.sm("Inputs")["control"],
        out_bytes=out_bytes, in_bytes=in_bytes,
        n_rx=sum(len(p["entries"]) for p in esi.rxpdos),
        n_tx=sum(len(p["entries"]) for p in esi.txpdos),
        mappings="\n".join(lines) + "\n",
    )


# --------------------------------------------------------------------------

def cmd_header(args, esi):
    text = emit_header(esi)
    if args.output:
        Path(args.output).write_text(text)
        print(f"wrote {args.output}")
    else:
        sys.stdout.write(text)
    return 0


def cmd_sii(args, esi):
    if args.use_esi_config:
        if len(esi.config_data) < CONFIG_WORDS * 2:
            raise SystemExit("ESI ConfigData is shorter than 16 bytes")
        config = esi.config_data[:CONFIG_WORDS * 2]
        print("WARNING: synthesising the PDI config area from the ESI.")
        print("         If it is wrong the ESC stops answering over SPI and")
        print("         only an external programmer will get it back.")
    else:
        if not args.preserve_config:
            raise SystemExit(
                "refusing to guess the PDI config area.\n"
                "Back the live device up first:\n"
                "    eepromtool <iface> 1 -r backup.bin\n"
                "then pass --preserve-config backup.bin.\n"
                "(--use-esi-config overrides this, for an already-dead board.)")
        backup = Path(args.preserve_config).read_bytes()
        if len(backup) < CONFIG_WORDS * 2:
            raise SystemExit(f"{args.preserve_config}: too short to be an SII image")
        config = backup[:CONFIG_WORDS * 2]
        pdi = config[0]
        print(f"config area copied from {args.preserve_config} (PDI type {pdi:#04x})")
        if pdi != 0x05:
            print(f"NOTE: PDI {pdi:#04x} is not SPI slave (0x05). That is what the")
            print("      backup says, and it is preserved as-is, but the ESP32-S3")
            print("      reaches this ESC over SPI -- worth understanding first.")

    image = build_sii(esi, config)

    # Verify by decoding the bytes we are about to write, not by trusting
    # the code above.
    got = decode(image)
    want_in = esi.pdo_bytes(esi.txpdos)
    want_out = esi.pdo_bytes(esi.rxpdos)
    problems = []
    if got["vendor"] != esi.vendor_id:
        problems.append("vendor id did not round-trip")
    if got["product"] != esi.product_code:
        problems.append("product code did not round-trip")
    sm_in = next((s for s in got["sms"] if s["type"] == 4), None)
    sm_out = next((s for s in got["sms"] if s["type"] == 3), None)
    if not sm_in or sm_in["len"] != want_in:
        problems.append(f"SM3 length in image is not {want_in}")
    if not sm_out or sm_out["len"] != want_out:
        problems.append(f"SM2 length in image is not {want_out}")
    if problems:
        raise SystemExit("generated image failed its own check:\n  - "
                         + "\n  - ".join(problems))

    Path(args.output).write_bytes(image)
    print(f"wrote {args.output} ({len(image)} bytes)")
    print(f"  identity  {got['vendor']:#010x} / {got['product']:#010x} "
          f"rev {got['revision']:#010x}")
    print(f"  SM2 {sm_out['len']} bytes out, SM3 {sm_in['len']} bytes in")
    return 0


def cmd_dump(args, _esi):
    image = Path(args.image).read_bytes()
    d = decode(image)
    print(f"PDI type      {d['pdi']:#04x}")
    print(f"vendor        {d['vendor']:#010x}")
    print(f"product       {d['product']:#010x}")
    print(f"revision      {d['revision']:#010x}")
    print(f"mailbox       out {d['mbox_out']:#06x} / in {d['mbox_in']:#06x}")
    names = {1: "MBoxOut", 2: "MBoxIn", 3: "Outputs", 4: "Inputs"}
    for i, s in enumerate(d["sms"]):
        print(f"SM{i}  addr {s['start']:#06x} len {s['len']:3d} "
              f"ctrl {s['ctrl']:#04x} {names.get(s['type'], '?')}")
    for p in d["pdos"]:
        total = sum(e["bits"] for e in p["entries"]) // 8
        print(f"{p['dir']}PDO {p['index']:#06x} on SM{p['sm']}: {total} bytes")
        for e in p["entries"]:
            print(f"    {e['index']:#06x}:{e['sub']:02d}  {e['bits']:2d} bits")
    if d["strings"]:
        print("strings       " + ", ".join(d["strings"]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--esi", default=str(DEFAULT_ESI))
    sub = ap.add_subparsers(dest="cmd", required=True)

    h = sub.add_parser("header", help="emit the C header for the firmware")
    h.add_argument("-o", "--output")
    h.set_defaults(func=cmd_header)

    s = sub.add_parser("sii", help="build the EEPROM image")
    s.add_argument("-o", "--output", required=True)
    s.add_argument("--preserve-config", metavar="BACKUP.BIN",
                   help="copy words 0..7 from a backup of the live device")
    s.add_argument("--use-esi-config", action="store_true",
                   help="synthesise the config area from the ESI instead")
    s.set_defaults(func=cmd_sii)

    d = sub.add_parser("dump", help="decode an SII image")
    d.add_argument("image")
    d.set_defaults(func=cmd_dump)

    args = ap.parse_args()
    esi = Esi(args.esi) if args.cmd != "dump" else None
    return args.func(args, esi)


if __name__ == "__main__":
    sys.exit(main())
