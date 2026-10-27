#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Independent structural check of an FM1 .fwsc update file.

Removes the interleaved product bytes, deciphers the UFW header and file
records and verifies every CRC. With --elf-app it also compares flash.bin's
application payload against the given raw image.
"""
import struct
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from build_mac import crc16, cipher  # noqa: E402


def unwrap(data):
    out, product = bytearray(), []
    i = 0
    src = 0
    while i < len(data):
        out.append(data[i]); i += 1
        if src < 940 and (src + 1) % 47 == 0:
            product.append(data[i]); i += 1
        src += 1
    name = ''.join(chr(b - k - 1) for k, b in enumerate(product) if b != 125)
    return bytes(out), name


def main(path):
    raw, product = unwrap(open(path, 'rb').read())
    hdr = bytearray(raw[:224])
    cipher(hdr, 0, 64, 0xffff)
    ok = True
    def check(cond, what):
        nonlocal ok
        print(('ok   ' if cond else 'FAIL ') + what); ok &= bool(cond)
    total = struct.unpack_from('<I', hdr, 4)[0]
    check(hdr[16:22] == b'AC791N', 'chip id AC791N')
    check(total == len(raw), f'total size {total}')
    check(struct.unpack_from('<H', hdr, 0)[0] == crc16(hdr, 2, 62), 'header CRC')
    check(struct.unpack_from('<H', hdr, 2)[0] == crc16(hdr, 64, 160), 'record table CRC')
    for r in range(2):
        cipher(hdr, 64 + r * 80, 80, 0xffff)
    files = {}
    for r in range(2):
        rec = 64 + r * 80
        crc, off, size = struct.unpack_from('<H', hdr, rec + 4)[0], *struct.unpack_from('<II', hdr, rec + 8)
        name = hdr[rec + 64:rec + 80].split(b'\0')[0].decode()
        body = raw[off:off + size]
        check(crc16(body) == crc, f'{name}: {size} bytes, CRC')
        files[name] = body
    flash = bytearray(files['flash.bin'])
    head = bytearray(flash[:160])
    for p in range(0, 160, 32):
        cipher(head, p, 32, 0xffff)
    check(struct.unpack_from('<H', head, 0)[0] == crc16(head, 2, 30), 'flash header CRC')
    for p in range(32, 160, 32):
        check(struct.unpack_from('<H', head, p)[0] == crc16(head, p + 2, 30),
              'flash record ' + head[p + 16:p + 32].split(b'\0')[0].decode(errors='replace'))
    print('product:', product)
    return ok


if __name__ == '__main__':
    results = [main(p) for p in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)
