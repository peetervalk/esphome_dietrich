#!/usr/bin/env python3
"""Turn a Recom <-> boiler capture into annotated Remeha frames.

Input is either a USBPcap capture of the FTDI service cable - the script calls
tshark itself - or a text file of hex bytes, one chunk per line, prefixed with
">" for PC -> boiler and "<" for boiler -> PC.

    py tools/decode_capture.py capture.pcapng
    py tools/decode_capture.py capture.pcapng --out trace.txt
    py tools/decode_capture.py hexdump.txt

Frame rules are from mapping/pcu05_p3_protocol.md; parameter names come from
mapping/pcu05_p3_datamap.json, so a WRITE_EPROM_BLOCK is reported as the
parameter it changes rather than as sixteen bytes.

What to look for in the output: any COMMAND the table does not name, anything
carrying EXT 0x52 (CODE_FACTORY), and COMMAND 0x37 (SERVICE_CODE) - that last
one is how you find out whether the 0012 PIN goes on the wire at all.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

COMMANDS = {
    0x01: "IDENTIFICATION", 0x02: "SAMPLES", 0x08: "CODE_SERVICE_START",
    0x09: "CODE_FACTORY_COMMANDO", 0x10: "READ_EPROM_BLOCK",
    0x11: "WRITE_EPROM_BLOCK", 0x1F: "CODE_SERVICE_STOP", 0x31: "RESET",
    0x32: "SET_DFDU", 0x33: "AUTO_DETECT", 0x37: "SERVICE_CODE",
    0x66: "SCU_C_SAMPLES", 0x67: "CALIBRATE_SCOT",
}
EXTS = {0x00: "NONE", 0x01: "SAMPLES_FORMAT", 0x0B: "IDENTIFICATION",
        0x0C: "CODE_SERVICE", 0x52: "CODE_FACTORY"}
DEVICES = {0x00: "PSU", 0x01: "PCU", 0x02: "SCU_C", 0x03: "SU", 0x08: "SCU_S",
           0xFE: "PC", 0xFF: "NO_DEVICE"}
TYPES = {0x05: "REQ", 0x06: "ACK", 0x15: "NAK"}
EEPROM_CMDS = (0x10, 0x11)
PARAM_FIRST_BLOCK = 0x14          # blocks 0x14..0x1B are the 128 byte image
PARAM_LAST_BLOCK = 0x1B


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def tshark_path():
    found = shutil.which("tshark")
    if found:
        return found
    for guess in (r"C:\Program Files\Wireshark\tshark.exe",
                  r"C:\Program Files (x86)\Wireshark\tshark.exe"):
        if os.path.exists(guess):
            return guess
    sys.exit("tshark not found - add Wireshark to PATH, or pass a hex text file")


def hex_to_bytes(text):
    text = re.sub(r"[^0-9a-fA-F]", "", text)
    return bytes.fromhex(text) if len(text) % 2 == 0 else b""


def read_pcap(path):
    """(timestamp, direction, bytes) per USB transfer; '>' is PC -> boiler."""
    tshark = tshark_path()
    rows = []

    # Preferred: the FTDI dissector, which already strips the two modem-status
    # bytes FTDI prepends to every bulk IN transfer.
    out = subprocess.run(
        [tshark, "-r", path,
         "-Y", "ftdi-ft.if_a_rx_payload or ftdi-ft.if_a_tx_payload",
         "-T", "fields", "-e", "frame.time_epoch",
         "-e", "ftdi-ft.if_a_tx_payload", "-e", "ftdi-ft.if_a_rx_payload",
         "-E", "separator=|", "-E", "occurrence=a"],
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split("|")
        if len(parts) < 3:
            continue
        ts = float(parts[0])
        for chunk in parts[1].split(","):
            if chunk.strip():
                rows.append((ts, ">", hex_to_bytes(chunk)))
        for chunk in parts[2].split(","):
            if chunk.strip():
                rows.append((ts, "<", hex_to_bytes(chunk)))
    if rows:
        return rows

    # Fallback: raw bulk payloads. Here the FTDI status bytes are ours to strip.
    print("# ftdi-ft dissector produced nothing - falling back to usb.capdata",
          file=sys.stderr)
    out = subprocess.run(
        [tshark, "-r", path, "-Y", "usb.capdata", "-T", "fields",
         "-e", "frame.time_epoch", "-e", "usb.endpoint_address.direction",
         "-e", "usb.capdata", "-E", "separator=|", "-E", "occurrence=a"],
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split("|")
        if len(parts) < 3 or not parts[2].strip():
            continue
        ts = float(parts[0])
        inbound = parts[1].strip().endswith("1")
        data = hex_to_bytes(parts[2].split(",")[0])
        if inbound:
            data = data[2:]          # FTDI modem/line status
        rows.append((ts, "<" if inbound else ">", data))
    return rows


def read_hexfile(path):
    rows = []
    ts = 0.0
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            direction = "<" if line[0] == "<" else ">"
            data = hex_to_bytes(line.lstrip("<>"))
            if data:
                rows.append((ts, direction, data))
                ts += 0.001
    return rows


def assemble(rows, direction):
    """Reassemble one direction's byte stream into frames, resyncing on junk."""
    stream = []
    for ts, d, chunk in rows:
        if d == direction:
            stream.extend((ts, b) for b in chunk)

    frames = []
    i = 0
    while i < len(stream):
        if stream[i][1] != 0x02:
            i += 1
            continue
        if i + 5 > len(stream):
            break
        total = stream[i + 4][1] + 2
        if total < 10 or i + total > len(stream):
            i += 1
            continue
        frame = bytes(b for _, b in stream[i:i + total])
        if frame[-1] != 0x03:
            i += 1               # not a frame after all - rescan from the next byte
            continue
        frames.append((stream[i][0], direction, frame))
        i += total
    return frames


def load_params():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "mapping", "pcu05_p3_datamap.json")
    by_offset = {}
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except OSError:
        return by_offset
    for p in data.get("blocks", {}).get("parameter", []):
        by_offset.setdefault(p["byte"], []).append("p%s %s" % (p["code"], p["name"]))
    return by_offset


def describe(frame, params, shadow):
    src, dst, typ, cmd, ext = frame[1], frame[2], frame[3], frame[5], frame[6]
    data = frame[7:-3]

    name = COMMANDS.get(cmd, "COMMAND 0x%02X (UNKNOWN)" % cmd)
    if cmd in EEPROM_CMDS:
        extname = "blk 0x%02X" % ext
    else:
        extname = EXTS.get(ext, "ext 0x%02X (UNKNOWN)" % ext)
    src_name = DEVICES.get(src, "0x%02X" % src)
    dst_name = DEVICES.get(dst, "0x%02X" % dst)
    head = "%-5s -> %-5s %-4s %s %s" % (
        src_name, dst_name, TYPES.get(typ, "0x%02X?" % typ), name, extname)

    notes = []
    if crc16(frame[1:-3]) != (frame[-2] << 8 | frame[-3]):
        notes.append("CRC BAD")
    if data:
        notes.append("%d B" % len(data))

    detail = []
    if cmd in EEPROM_CMDS and len(data) == 16 and PARAM_FIRST_BLOCK <= ext <= PARAM_LAST_BLOCK:
        base = (ext - PARAM_FIRST_BLOCK) * 16
        if typ == 0x06 and cmd == 0x10:          # read reply: remember it
            shadow[ext] = data
        elif cmd == 0x11:                        # write: diff against last read
            old = shadow.get(ext)
            for i, b in enumerate(data):
                if old is None or old[i] != b:
                    label = ", ".join(params.get(base + i, ["byte %d" % (base + i)]))
                    was = "%d" % old[i] if old else "?"
                    detail.append("%s: %s -> %d" % (label, was, b))
            if not detail:
                detail.append("identical to the block last read")
    elif data:
        detail.append(data.hex())
    return head, notes, detail


def eeprom_snapshot(frames):
    """(address, block) -> (timestamp, 16 bytes), as last stated on the wire.

    A read reply and a write request each say what a block holds, so a capture of
    a service tool at work is a partial EEPROM dump - of whatever it happened to
    read, which for Recom opening a connection is every block it has a use for.
    """
    snap = {}
    for ts, direction, f in frames:
        typ, cmd, ext, data = f[3], f[5], f[6], f[7:-3]
        if cmd not in EEPROM_CMDS or len(data) != 16:
            continue
        if cmd == 0x10 and typ == 0x06:
            addr = f[1]          # the device answering the read
        elif cmd == 0x11 and typ == 0x05:
            addr = f[2]          # the device being written to
        else:
            continue
        snap[(addr, ext)] = (ts, data)
    return snap


def print_eeprom(frames, t0, out):
    snap = eeprom_snapshot(frames)
    if not snap:
        print("no EEPROM blocks in this capture", file=out)
        return
    for addr in sorted({a for a, _ in snap}):
        blocks = {b: v for (a, b), v in snap.items() if a == addr}
        print("# device 0x%02X, %d block(s)" % (addr, len(blocks)), file=out)
        for blk in sorted(blocks):
            ts, data = blocks[blk]
            ascii_col = "".join(chr(c) if 32 <= c < 127 else "." for c in data)
            print("eeprom %02X:%02X %s |%s|  t=%.1f" % (addr, blk, data.hex().upper(),
                                                       ascii_col, ts - t0), file=out)

        # the parameter image, if the capture happened to cover all of it
        if all(b in blocks for b in range(PARAM_FIRST_BLOCK, PARAM_LAST_BLOCK + 1)):
            image = b"".join(blocks[b][1] for b in range(PARAM_FIRST_BLOCK, PARAM_LAST_BLOCK + 1))
            for half, name in ((0, "low"), (64, "high")):
                want = crc16(image[half:half + 62])
                have = image[half + 62] | image[half + 63] << 8
                print("# %s half CRC: stored %04X, computes %04X - %s"
                      % (name, have, want, "ok" if want == have else "MISMATCH"), file=out)
        print("", file=out)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help=".pcapng from USBPcap, or a hex text file")
    ap.add_argument("--out", help="write the trace here instead of stdout")
    ap.add_argument("--eeprom", action="store_true",
                    help="reconstruct the EEPROM blocks the capture saw, instead of the frame trace")
    args = ap.parse_args()

    if args.capture.lower().endswith((".pcap", ".pcapng")):
        rows = read_pcap(args.capture)
    else:
        rows = read_hexfile(args.capture)
    if not rows:
        sys.exit("no serial payload found in the capture")

    frames = sorted(assemble(rows, ">") + assemble(rows, "<"), key=lambda f: f[0])
    if not frames:
        sys.exit("payload found, but no complete frames in it - wrong endpoint?")

    params = load_params()
    shadow = {}
    seen = {}
    flagged = []
    t0 = frames[0][0]

    out = open(args.out, "w", encoding="utf-8") if args.out else sys.stdout
    if args.eeprom:
        print_eeprom(frames, t0, out)
        if out is not sys.stdout:
            out.close()
            print("wrote %s" % args.out)
        return

    for ts, direction, frame in frames:
        head, notes, detail = describe(frame, params, shadow)
        cmd = frame[5]
        seen[cmd] = seen.get(cmd, 0) + 1
        if cmd not in COMMANDS or frame[6] == 0x52 or cmd in (0x09, 0x32, 0x37):
            flagged.append((ts - t0, head))
        suffix = "   [%s]" % ", ".join(notes) if notes else ""
        print("%9.3f  %s  %s%s" % (ts - t0, direction, head, suffix), file=out)
        for line in detail:
            print("%13s%s" % ("", line), file=out)
        print("%13sraw %s" % ("", frame.hex()), file=out)

    print("\n--- command totals ---", file=out)
    for cmd in sorted(seen):
        print("  0x%02X  %-24s %d" % (cmd, COMMANDS.get(cmd, "UNKNOWN"), seen[cmd]),
              file=out)
    if flagged:
        print("\n--- worth reading closely (factory / unknown / PIN traffic) ---",
              file=out)
        for ts, head in flagged:
            print("  %9.3f  %s" % (ts, head), file=out)
    if out is not sys.stdout:
        out.close()
        print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
