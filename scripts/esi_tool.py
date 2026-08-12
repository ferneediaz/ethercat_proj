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

# Port descriptor values, two bits per port in General category byte 4.
# The ESI spells these as a Physics string, one character per port:
# Y = MII, K = EBus, H = not connected, and absent = not implemented.
PHYSICS = {"Y": 3, "K": 2, "H": 1, " ": 0, "": 0}

# CoE details bits, General category byte 5.
COE_ENABLE_SDO = 1 << 0
COE_ENABLE_SDO_INFO = 1 << 1
COE_ENABLE_PDO_ASSIGN = 1 << 2
COE_ENABLE_PDO_CONFIG = 1 << 3
COE_ENABLE_COMPLETE_ACCESS = 1 << 5

MBXPROTO_COE = 0x0004       # mailbox protocol bit for CANopen over EtherCAT

GENERAL_SIZE = 32           # IgH rejects any other size

CONFIG_WORDS = 8            # words 0..7, the region we refuse to synthesise
CATEGORY_START_WORD = 0x40

#
# SII header word addresses.
#
# These are the single most dangerous constants in this file, because getting
# one wrong produces an image that looks perfectly well-formed to anything
# that shares the mistake. Taken from the masters that actually read them
# rather than from memory:
#
#   SOEM soem/ethercattype.h    ECT_SII_MANUF 0x0008, ECT_SII_ID 0x000a,
#                               ECT_SII_REV 0x000c, ECT_SII_BOOTRXMBX 0x0014,
#                               ECT_SII_BOOTTXMBX 0x0016,
#                               ECT_SII_RXMBXADR 0x0018, ECT_SII_MBXSIZE 0x0019,
#                               ECT_SII_TXMBXADR 0x001a, ECT_SII_MBXPROTO 0x001c
#   IgH master/fsm_slave_scan.c reads the same addresses
#
# "Receive" and "send" are from the slave's point of view: the receive mailbox
# is master -> slave, which the ESI calls MBoxOut.
#
W_VENDOR_ID = 0x0008
W_PRODUCT_CODE = 0x000A
W_REVISION = 0x000C
W_SERIAL = 0x000E
W_BOOT_RX_MBX = 0x0014
W_BOOT_TX_MBX = 0x0016
W_STD_RX_MBX_OFFSET = 0x0018
W_STD_RX_MBX_SIZE = 0x0019
W_STD_TX_MBX_OFFSET = 0x001A
W_STD_TX_MBX_SIZE = 0x001B
W_MBX_PROTOCOL = 0x001C
W_EEPROM_SIZE = 0x003E
W_SII_VERSION = 0x003F


def crc8(data):
    """CRC over the SII configuration area, stored in the low byte of word 7.

    Polynomial x^8 + x^2 + x + 1 (0x07), initial value 0xFF, computed over
    bytes 0..13. Only needed when synthesising the config area from scratch;
    the normal path copies words 0..7 out of a backup, which carries a valid
    CRC already."""
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


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

        self.physics = (dev.get("Physics") or "").strip()

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
        coe = mbox.find("CoE") if mbox is not None else None
        self.has_coe = coe is not None
        # Advertise in the SII only what the ESI claims, so the EEPROM and the
        # XML cannot promise a master different things.
        self.coe_pdo_assign = coe is not None and coe.get("PdoAssign", "0") == "1"
        self.coe_pdo_config = coe is not None and coe.get("PdoConfig", "0") == "1"
        self.coe_complete_access = (
            coe is not None and coe.get("CompleteAccess", "0") == "1")
        self.has_dc = dev.find("Dc") is not None

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

    # Exactly 32 bytes: IgH rejects a General category of any other size.
    general = bytearray(GENERAL_SIZE)
    general[0] = group_idx
    general[1] = 0                      # image index, none
    general[2] = order_idx
    general[3] = name_idx

    # Byte 4 is the port descriptor, two bits per port, ports 0..3 from the
    # low bits up: 0 not implemented, 1 not configured, 2 EBus, 3 MII.
    ports = 0
    for i, phys in enumerate(esi.physics[:4]):
        ports |= (PHYSICS[phys] & 0x03) << (i * 2)
    general[4] = ports

    # Byte 5 is CoE details. Advertise only what the dictionary actually
    # honours: SDO and SDO Info. Setting enable_pdo_assign or
    # enable_pdo_configuration here invites the master to write 0x1C12/0x1C13,
    # which are read-only in slave_objectlist.c and would abort.
    coe = 0
    if esi.has_coe:
        coe |= COE_ENABLE_SDO | COE_ENABLE_SDO_INFO
        if esi.coe_pdo_assign:
            coe |= COE_ENABLE_PDO_ASSIGN
        if esi.coe_pdo_config:
            coe |= COE_ENABLE_PDO_CONFIG
        if esi.coe_complete_access:
            coe |= COE_ENABLE_COMPLETE_ACCESS
    general[5] = coe

    general[10] = 0x01                  # SysmanClass / standard device

    # Byte 11 is the general flags byte. Bit 0 is enable_safeop, which asks
    # the master to leave the device in SAFEOP -- not a generic "enabled"
    # bit. This device runs in OP, so it stays clear. Bit 1 is enable_notLRW.
    general[11] = 0x00

    struct.pack_into("<h", general, 12, 0)       # current on E-bus, mA
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

    put32(W_VENDOR_ID, esi.vendor_id)
    put32(W_PRODUCT_CODE, esi.product_code)
    put32(W_REVISION, esi.revision)
    put32(W_SERIAL, 0)

    # Receive = master -> slave = the ESI's MBoxOut. Getting these two the
    # wrong way round leaves a slave whose mailbox replies go nowhere.
    mbox_rx, mbox_tx = esi.sm("MBoxOut"), esi.sm("MBoxIn")
    put16(W_STD_RX_MBX_OFFSET, mbox_rx["start"])
    put16(W_STD_RX_MBX_SIZE, mbox_rx["size"])
    put16(W_STD_TX_MBX_OFFSET, mbox_tx["start"])
    put16(W_STD_TX_MBX_SIZE, mbox_tx["size"])
    put16(W_MBX_PROTOCOL, MBXPROTO_COE if esi.has_coe else 0)

    put16(W_EEPROM_SIZE, (esi.eeprom_bytes * 8) // 1024 - 1)  # kbit - 1
    put16(W_SII_VERSION, 1)

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
    if len(image) < CATEGORY_START_WORD * 2:
        raise SystemExit(
            f"image is {len(image)} bytes; an SII needs at least "
            f"{CATEGORY_START_WORD * 2} before the categories start")

    def w16(word):
        return struct.unpack_from("<H", image, word * 2)[0]

    def w32(word):
        return struct.unpack_from("<I", image, word * 2)[0]

    out = {
        "vendor": w32(W_VENDOR_ID),
        "product": w32(W_PRODUCT_CODE),
        "revision": w32(W_REVISION),
        "pdi": image[0],
        "mbox_rx": w16(W_STD_RX_MBX_OFFSET),
        "mbox_rx_size": w16(W_STD_RX_MBX_SIZE),
        "mbox_tx": w16(W_STD_TX_MBX_OFFSET),
        "mbox_tx_size": w16(W_STD_TX_MBX_SIZE),
        "mbox_proto": w16(W_MBX_PROTOCOL),
        "ports": None, "coe_details": None, "general_flags": None,
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
        elif code == CAT_GENERAL and len(data) >= 12:
            out["ports"] = data[4]
            out["coe_details"] = data[5]
            out["general_flags"] = data[11]
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
        # ESI ConfigData conventionally carries words 0..6 (14 bytes); word 7
        # is the CRC over them, which the writing tool supplies.
        cfg = esi.config_data
        if len(cfg) < 14:
            raise SystemExit(
                f"ESI ConfigData is {len(cfg)} bytes; need at least 14 "
                "(words 0..6, with word 7 the CRC this tool computes)")
        body = cfg[:14]
        config = body + bytes([crc8(body), 0x00])
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
        if len(backup) != esi.eeprom_bytes:
            raise SystemExit(
                f"{args.preserve_config} is {len(backup)} bytes but the ESI "
                f"declares ByteSize {esi.eeprom_bytes}.\n"
                "The backup is what the hardware actually has, so the ESI is "
                "wrong. Fix <Eeprom><ByteSize> before writing -- otherwise the\n"
                "size word at 0x3E would misreport the part and only some of "
                "it would be written.")
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
    if esi.has_coe and not (got["mbox_proto"] & MBXPROTO_COE):
        problems.append("CoE declared in the ESI but the mailbox protocol "
                        "word does not advertise it")
    if got["mbox_rx"] != esi.sm("MBoxOut")["start"]:
        problems.append(
            f"receive mailbox is {got['mbox_rx']:#06x}, expected "
            f"{esi.sm('MBoxOut')['start']:#06x} (master -> slave is MBoxOut)")
    if got["mbox_tx"] != esi.sm("MBoxIn")["start"]:
        problems.append(
            f"send mailbox is {got['mbox_tx']:#06x}, expected "
            f"{esi.sm('MBoxIn')['start']:#06x}")
    if got["general_flags"] & 0x01:
        problems.append("general flags set enable_safeop; this device runs in OP")
    if got["ports"] == 0:
        problems.append("port descriptor says no ports are implemented")
    if problems:
        raise SystemExit("generated image failed its own check:\n  - "
                         + "\n  - ".join(problems))

    Path(args.output).write_bytes(image)
    print(f"wrote {args.output} ({len(image)} bytes)")
    print(f"  identity  {got['vendor']:#010x} / {got['product']:#010x} "
          f"rev {got['revision']:#010x}")
    print(f"  SM2 {sm_out['len']} bytes out, SM3 {sm_in['len']} bytes in")
    print(f"  mailbox   rx {got['mbox_rx']:#06x} / tx {got['mbox_tx']:#06x}, "
          f"protocol {got['mbox_proto']:#06x}")
    if esi.has_dc:
        print("  NOTE: the ESI declares a Dc OpMode, but no DC category (60) is")
        print("        written to the SII. SOEM configures distributed clocks")
        print("        from the ESC registers so this changes nothing there;")
        print("        an ESI-driven tool reading only the EEPROM will not")
        print("        offer a SYNC0 mode. Import the XML into such tools.")
    return 0


def cmd_dump(args, _esi):
    image = Path(args.image).read_bytes()
    d = decode(image)
    print(f"PDI type      {d['pdi']:#04x}")
    print(f"vendor        {d['vendor']:#010x}")
    print(f"product       {d['product']:#010x}")
    print(f"revision      {d['revision']:#010x}")
    print(f"mailbox rx    {d['mbox_rx']:#06x} len {d['mbox_rx_size']}  (master -> slave)")
    print(f"mailbox tx    {d['mbox_tx']:#06x} len {d['mbox_tx_size']}  (slave -> master)")
    proto = d["mbox_proto"]
    print(f"mbox protocol {proto:#06x}"
          + ("  CoE" if proto & MBXPROTO_COE else "  NONE -- no SDO access!"))
    if d["ports"] is not None:
        names = {0: "-", 1: "unconf", 2: "EBus", 3: "MII"}
        p4 = [names[(d["ports"] >> (i * 2)) & 3] for i in range(4)]
        print(f"ports         {' '.join(p4)}")
        print(f"CoE details   {d['coe_details']:#04x}")
        print(f"general flags {d['general_flags']:#04x}"
              + ("  SAFEOP-ONLY" if d["general_flags"] & 0x01 else ""))
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
