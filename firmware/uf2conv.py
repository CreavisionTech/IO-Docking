#!/usr/bin/env python3
"""Minimal UF2 converter for RP2040.

Usage:
    python uf2conv.py <input.elf|input.bin> [-o out.uf2] [-f rp2040]

Input may be a raw binary or an ELF (ELF is converted via arm-none-eabi-objcopy
if available, or via binutils `objcopy`). Produces a UF2 image loadable by the
RP2040 bootrom.
"""
import struct
import subprocess
import sys
import tempfile
import os

FAMILY = {
    "rp2040": 0xE48BFF56,
}

BLOCK = 512
HEADER = 32
TRAILER = 4                    # end-of-block magic
# WARNING: this board's bootrom v3 accepts ONLY 256-byte-payload blocks
# (32 hdr + 256 data + 220 pad + 4 trailer). The standard 476-payload layout
# is SILENTLY rejected (file consumed, nothing written to flash) - verified
# in the field. Keep PAYLOAD at 256.
PAYLOAD = 256
BASE = 0x10000000  # RP2040 flash base

def read_input(path):
    data = open(path, "rb").read()
    if len(data) >= 4 and data[0:4] in (b"\x7fELF", b"\x02\x00\x00\x00"):
        # ELF -> convert to raw binary
        fd, tmp = tempfile.mkstemp(suffix=".bin")
        os.close(fd)
        for objcopy in ("arm-none-eabi-objcopy", "objcopy"):
            try:
                subprocess.run(
                    [objcopy, "-O", "binary", path, tmp], check=True,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                data = open(tmp, "rb").read()
                break
            except (OSError, subprocess.CalledProcessError):
                continue
        else:
            sys.exit("error: cannot convert ELF to binary (need objcopy)")
        os.unlink(tmp)
    return data

def to_uf2(data, family):
    num = (len(data) + PAYLOAD - 1) // PAYLOAD
    out = bytearray()
    for i in range(num):
        chunk = data[i * PAYLOAD: (i + 1) * PAYLOAD]
        if len(chunk) < PAYLOAD:
            chunk += bytes(PAYLOAD - len(chunk))   # pad last block to full payload
        out += struct.pack(
            "<IIIIIIII",
            0x0A324655,  # UF2\n
            0x9E5D5157,
            0x00002000,  # family id present
            BASE + i * PAYLOAD,
            PAYLOAD,                             # always full payload size
            i,
            num,
            family,
        )
        out += chunk
        # pad the block out to 512 bytes total (32 hdr + 256 data + 220 pad + 4 trailer)
        out += bytes(BLOCK - HEADER - len(chunk) - TRAILER)
        out += struct.pack("<I", 0x0AB16F30)
    return bytes(out)

def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    infile = args[0]
    outfile = "io_dock.uf2"
    family = FAMILY["rp2040"]
    i = 1
    while i < len(args):
        if args[i] == "-o" and i + 1 < len(args):
            outfile = args[i + 1]; i += 2
        elif args[i] == "-f" and i + 1 < len(args):
            family = FAMILY.get(args[i + 1].lower(), FAMILY["rp2040"]); i += 2
        else:
            i += 1
    data = read_input(infile)
    uf2 = to_uf2(data, family)
    open(outfile, "wb").write(uf2)
    print(f"{infile} -> {outfile} ({len(data)} bytes, {len(uf2)} bytes)")

if __name__ == "__main__":
    main()
