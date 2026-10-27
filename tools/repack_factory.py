#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Re-encode app/fm_factory.inc with an optimal LZ parse.

The token format and the decoder in app/fm_factory.c are unchanged:
a byte < 128 is a literal; 128+n copies n+3 bytes from a 7- or 14-bit
distance. The decoded VMEM must keep the SHA-256 noted in the header.

  repack_factory.py            re-encode all voices in the file
  repack_factory.py --voices N keep the first N voices (groups of 32)
"""
import sys
import hashlib
import re
from pathlib import Path

PATH = Path(__file__).resolve().parent.parent / 'app/fm_factory.inc'


def decode(data):
    out, at = [], 0
    while at < len(data):
        token = data[at]; at += 1
        if token < 128:
            out.append(token)
            continue
        low = data[at]; at += 1
        distance = low & 127
        if low & 128:
            distance |= data[at] << 7; at += 1
        for _ in range(token - 128 + 3):
            out.append(out[-distance])
    return bytes(out)


def encode(raw):
    n, inf = len(raw), 1 << 30
    cost, choice = [inf] * (n + 1), [None] * (n + 1)
    cost[n] = 0
    for i in range(n - 1, -1, -1):
        best, pick = cost[i + 1] + 1, None
        for d in range(1, min(i, 16383) + 1):
            if raw[i - d] != raw[i]:
                continue
            length = 0
            while i + length < n and length < 130 and raw[i + length] == raw[i + length - d]:
                length += 1
            header = 2 if d < 128 else 3
            for size in range(3, length + 1):
                if header + cost[i + size] < best:
                    best, pick = header + cost[i + size], (size, d)
        cost[i], choice[i] = best, pick
    out, i = [], 0
    while i < n:
        if choice[i] is None:
            out.append(raw[i]); i += 1
            continue
        size, d = choice[i]
        out.append(128 + size - 3)
        out += [d] if d < 128 else [128 | (d & 127), d >> 7]
        i += size
    return out


def main():
    source = PATH.read_text()
    voices = int(sys.argv[sys.argv.index('--voices') + 1]) if '--voices' in sys.argv else None
    body = source[source.index('{') + 1:source.index('};')]
    raw = decode([int(x) for x in re.findall(r'\d+', body)])
    kept = re.search(r'Kept voices: (\d+), SHA-256: ([0-9a-f]{64})', source)
    full = re.search(r'VMEM SHA-256: ([0-9a-f]{64})', source).group(1)
    expected = kept.group(2) if kept else full
    if hashlib.sha256(raw).hexdigest() != expected:
        raise SystemExit('current bank does not match its SHA-256')
    if voices:
        if voices % 32 or voices * 128 > len(raw):
            raise SystemExit('--voices must be a multiple of 32 within the bank')
        raw = raw[:voices * 128]
    packed = encode(raw)
    if decode(packed) != raw:
        raise SystemExit('encoder error')
    head = source[:source.index('static const uint8_t factory_packed[]')]
    note = f' * Kept voices: {len(raw) // 128}, SHA-256: {hashlib.sha256(raw).hexdigest()}\n'
    head = re.sub(r' \* Kept voices: .*\n', '', head)
    if len(raw) != 128 * 128:
        head = head.replace(' * Source and license:', note + ' * Source and license:')
    rows = ['    ' + ','.join(map(str, packed[k:k + 24])) + ',' for k in range(0, len(packed), 24)]
    PATH.write_text(head + 'static const uint8_t factory_packed[] = {\n' + '\n'.join(rows) + '\n};\n')
    print(f'{len(packed)} bytes, {len(raw) // 128} voices, SHA-256 {hashlib.sha256(raw).hexdigest()}')


if __name__ == '__main__':
    main()
