#!/usr/bin/env python3
"""Flash an FM-1 from the command line over USB-MIDI (macOS CoreMIDI).

Speaks the same SysEx update protocol as the Workbench page
(build/official-web/*.html, class Updater) and checks the package the same
way. Only for an FM-1 that already runs FM1 Open firmware (Official Open,
GLIDER, GLITCH ...). Stock V15, third-party firmware, recovery and going back
to stock stay with the Workbench page ("Recovery & force update").

    python3 tools/flash_fm1.py ports              # list MIDI ports
    python3 tools/flash_fm1.py info               # read-only: identity and installed build
    python3 tools/flash_fm1.py check [package]    # check the package, no device needed
    python3 tools/flash_fm1.py flash [package]    # flash (asks for confirmation)

Package: build/dx7/generated/FM1-GLITCH.fwsc with its metadata
build/dx7/web-package.json (written by tools/build_mac.py). Needs
python-rtmidi (python3 -m pip install python-rtmidi).

Keep power and USB connected while flashing. If anything goes wrong, open the
Workbench page and use "Recovery & force update".
"""
import argparse
import hashlib
import json
import queue
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PACKAGE = ROOT / 'build/dx7/generated/FM1-GLITCH.fwsc'
DEFAULT_META = ROOT / 'build/dx7/web-package.json'
VENDOR_LOADER = 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541'
START_UPDATE = [240, 34, 36, 53, 127, 247]       # same bytes as the Workbench
FINISH_LOADER, FINISH_FLASH = 0xE0000000, 0xF0000000
TIMING = {'idle': 10.0, 'write_idle': 180.0, 'reconnect': 40.0, 'settle': 0.6}


class FlashError(Exception):
    pass


def check(ok, text):
    if not ok:
        raise FlashError(text)


def le(p, at, n):
    return int.from_bytes(bytes(p[at:at + n]), 'little')


def ascii_z(p):
    return bytes(p).split(b'\0', 1)[0].decode('latin-1')


_CRC16 = [0, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
          0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef]


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        c = ((c << 4) ^ _CRC16[c >> 12]) & 0xFFFF
        c = ((c << 4) ^ _CRC16[c >> 12]) & 0xFFFF
    return c


def decode_header(data, key=0xFFFF):
    p = bytearray(data)
    k = key
    for i in range(len(p)):
        p[i] ^= k & 255
        k = ((k << 1) ^ (0x1021 if k & 0x8000 else 0)) & 0xFFFF
    return p


def open_identity(value):
    return re.fullmatch(r'FM1_OFFICIAL_OPEN|FM1_COMMUNITY_\d+|FM-1_8\d{6}', value) is not None


# ------------------------------------------------------------------ package
def inspect_package(data):
    """Port of inspectPackage() of the Workbench: returns product, image, loader."""
    check(1024 <= len(data) <= 2097152, 'package length invalid')
    image = bytearray(len(data) - 20)
    product, ended = '', False
    for i in range(20):
        image[i * 47:i * 47 + 47] = data[i * 48:i * 48 + 47]
        marker = data[i * 48 + 47]
        if marker == 125:
            ended = True
        else:
            c = (marker - i - 1) & 255
            check(not ended and 32 <= c <= 126, 'package identity invalid')
            product += chr(c)
    image[940:] = data[960:]
    head = decode_header(image[0:64])
    count = le(head, 8, 2)
    check(le(head, 0, 2) == crc16(head[2:]) and le(head, 4, 4) == len(image), 'package header CRC failed')
    check(ascii_z(head[16:32]) == 'AC791N' and 0 < count <= 12, 'chip type or directory invalid')
    table = image[64:64 + count * 80]
    check(crc16(table) == le(head, 2, 2), 'package directory CRC failed')
    entries = []
    for i in range(count):
        e = decode_header(table[i * 80:i * 80 + 80])
        typ, offset, size, name = le(e, 0, 2), le(e, 8, 4), le(e, 12, 4), ascii_z(e[64:])
        stored, encrypted = le(e, 16, 4), le(e, 24, 4)
        check(offset >= 64 + count * 80 and offset <= len(image) and size <= len(image) - offset, 'entry out of range')
        check(stored >= size and stored <= len(image) - offset and (not encrypted or encrypted == stored), 'entry storage invalid')
        check(not any(stored and p['stored'] and offset < p['offset'] + p['stored'] and p['offset'] < offset + stored
                      for p in entries), 'entries overlap')
        content = bytes(image[offset:offset + size])
        check(encrypted or crc16(content) == le(e, 4, 2), 'content CRC failed: ' + name)
        check(not encrypted or typ not in (0, 100), 'main image or loader format incompatible')
        entries.append({'type': typ, 'offset': offset, 'size': size, 'stored': stored, 'name': name, 'data': content})
    flash = [e for e in entries if e['type'] == 0]
    loader = [e for e in entries if e['type'] == 100]
    check(len(flash) == 1 and 16384 <= flash[0]['size'] <= 1044480 and flash[0]['size'] % 4096 == 0, 'flash image invalid')
    check(len(loader) == 1 and 0 < loader[0]['size'] < 32768, 'update loader missing or invalid')
    return {'product': product, 'image': bytes(image), 'loader': loader[0]['data']}


def load_package(package_path, meta_path):
    data = Path(package_path).read_bytes()
    meta = json.loads(Path(meta_path).read_text())
    check(meta.get('schema') == 1 and open_identity(meta.get('product', '')) and
          re.fullmatch(r'[0-9a-f]{64}', meta.get('build_id', '')), 'metadata invalid: ' + str(meta_path))
    check(meta.get('family') == 7, 'metadata: unknown device family')
    sha = hashlib.sha256(data).hexdigest()
    check(sha == meta.get('sha256') and len(data) == meta.get('bytes'),
          'package and metadata do not belong together (rebuild with tools/build_mac.py)')
    pkg = inspect_package(data)
    check(pkg['product'] == meta['product'] and hashlib.sha256(pkg['loader']).hexdigest() == meta['loader_sha256'],
          'package and update loader do not match')
    pkg.update(product=meta['product'], family=meta['family'], build_id=meta['build_id'], sha256=sha,
               loader_identity='ota-FM-1_015' if meta['loader_sha256'] == VENDOR_LOADER
               else 'ota-FM1C_' + meta['build_id'][:16])
    return pkg


# ------------------------------------------------------------------ SysEx frames
def pack7(data):
    out, bits, value = [], 0, 0
    for byte in data:
        value |= byte << bits
        bits += 8
        while bits >= 7:
            out.append(value & 127)
            value >>= 7
            bits -= 7
    if bits:
        out.append(value & 127)
    return out


def frame(command, payload=()):
    payload = list(payload)
    check(len(payload) <= 520, 'protocol payload too long')
    total = sum(payload) & 255
    return [240] + pack7([0, 89, command, len(payload) & 255, len(payload) >> 8, 0] + payload + [(~total) & 255]) + [247]


def parse(message):
    if not message or len(message) < 10 or len(message) > 608 or message[0] != 240 or message[-1] != 247:
        return None
    raw, bits, value = [], 0, 0
    for b in message[1:-1]:
        if b > 127:
            return None
        value |= b << bits
        bits += 7
        if bits >= 8:
            raw.append(value & 255)
            value >>= 8
            bits -= 8
    if bits >= 7 or value != 0 or len(raw) < 7 or raw[0] != 0 or raw[1] != 89 or le(raw, 3, 3) != len(raw) - 7:
        return None
    if sum(raw[6:]) & 255 != 255:
        return None
    return raw[2], bytes(raw[6:-1])


# ------------------------------------------------------------------ MIDI link
class Timeout(FlashError):
    pass


class Link:
    """One opened input/output pair; parsed replies arrive in a queue."""

    def __init__(self, port_in, port_out, name):
        self.name = name
        self.port_in, self.port_out = port_in, port_out
        self.replies = queue.Queue()
        port_in.ignore_types(sysex=False, timing=True, active_sense=True)
        port_in.set_callback(self._receive)

    def _receive(self, event, _data=None):
        reply = parse(event[0])
        if reply:
            self.replies.put(reply)

    def send(self, message):
        self.port_out.send_message(list(message))

    def read(self, seconds):
        try:
            return self.replies.get(timeout=max(0.0, seconds))
        except queue.Empty:
            return None

    def query(self, command, payload=(), seconds=1.5):
        while not self.replies.empty():
            self.replies.get_nowait()
        self.send(frame(command, payload))
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            reply = self.read(min(0.25, until - time.monotonic()))
            if reply and reply[0] == command:
                return reply[1]
        raise Timeout('device did not answer')

    def close(self):
        for port in (self.port_in, self.port_out):
            try:
                port.close_port()
            except Exception:
                pass


def rtmidi_module():
    try:
        import rtmidi
    except ImportError:
        raise FlashError('python-rtmidi is missing: python3 -m pip install python-rtmidi')
    return rtmidi


def port_pairs(loader):
    """Input/output pairs that may be an FM-1 (same rules as the Workbench)."""
    rtmidi = rtmidi_module()
    probe_in, probe_out = rtmidi.MidiIn(), rtmidi.MidiOut()   # fresh objects: re-enumerated ports
    inputs, outputs = probe_in.get_ports(), probe_out.get_ports()
    pattern = (r'fm.?1|felucca|sloop|clipper|ota|sinco|usb.?midi|composite' if loader
               else r'fm.?1|^usb.?midi(?:\s+\d+)?$')
    pairs = []
    for i, name in enumerate(inputs):
        if not re.search(pattern, name, re.I):
            continue
        named = [j for j, o in enumerate(outputs) if o == name]
        if len(named) == 1:
            pairs.append((i, named[0], name))
        elif not named and len(inputs) == 1 and len(outputs) == 1:
            pairs.append((i, 0, name))
    return pairs, inputs, outputs


def open_pair(i, o, name):
    rtmidi = rtmidi_module()
    port_in, port_out = rtmidi.MidiIn(), rtmidi.MidiOut()
    port_in.open_port(i)
    port_out.open_port(o)
    link = Link(port_in, port_out, name)
    try:
        reply = None
        for attempt in range(2):
            try:
                reply = link.query(17, seconds=0.75)
                break
            except Timeout:
                if attempt:
                    raise
        check(len(reply) == 27 and re.fullmatch(r'[\x20-\x7e]{1,27}', ascii_z(reply)), 'invalid identity reply')
        return link, ascii_z(reply)
    except Exception:
        link.close()
        raise


def discover(loader=True):
    pairs, inputs, outputs = port_pairs(loader)
    check(len(pairs) <= 16, 'too many MIDI ports; disconnect other devices')
    found, errors = [], []
    for i, o, name in pairs:
        try:
            found.append(open_pair(i, o, name))
        except FlashError as error:
            errors.append(f'{name}: {error}')
    if len(found) > 1:
        for link, _ in found:
            link.close()
        raise FlashError('more than one possible FM-1 found; connect only one')
    return (found[0] if found else None), inputs, outputs, errors


def capability(data):
    check(len(data) == 47 and ascii_z(data[0:4]) == 'FMC1' and data[4] == 1 and data[5] <= 7 and
          le(data, 8, 2) == 512 and le(data, 10, 4) == 1048576, 'device update protocol does not match')
    check(data[7] == 1, 'device uses an unknown update loader')
    return {'family': data[5], 'state': data[6], 'build': data[14:46].hex(), 'error': data[46]}


# ------------------------------------------------------------------ update
def serve(link, image, finish, idle, phase, progress):
    """Answer the device's block requests until it reports the phase finished."""
    covered = bytearray((len(image) + 7) // 8)
    total, started = 0, time.monotonic()
    last = started
    while True:
        check(time.monotonic() - started < max(idle * 3, 120), phase + ': took too long, not finished')
        check(time.monotonic() - last < idle, phase + ': device stopped requesting data, not finished')
        packet = link.read(0.25)
        if not packet:
            continue
        command, p = packet
        if command != 48 or len(p) != 8:
            continue
        address, count = le(p, 1, 4), le(p, 5, 3)
        check(0 < count <= 512, 'device requested an invalid length')
        if address in (FINISH_LOADER, FINISH_FLASH):
            check(address == finish and count == 8, 'device update phase does not match')
            done = frame(48, [p[0], *p[1:8], *b'success\0'])
            link.send(done)
            end = time.monotonic() + TIMING['settle']
            while time.monotonic() < end:     # answer duplicate finish requests
                again = link.read(min(0.1, end - time.monotonic()))
                if again and again[0] == 48 and len(again[1]) == 8 and le(again[1], 1, 4) == finish and le(again[1], 5, 3) == 8:
                    link.send(frame(48, [again[1][0], *again[1][1:8], *b'success\0']))
            progress(phase, 100)
            return
        check(address <= len(image) and count <= len(image) - address, 'device read beyond the package')
        last = time.monotonic()
        link.send(frame(48, [p[0], *p[1:8], *image[address:address + count]]))
        for i in range(address, address + count):
            if not covered[i >> 3] & (1 << (i & 7)):
                covered[i >> 3] |= 1 << (i & 7)
                total += 1
        progress(phase, min(99, total * 100 // len(image)))


def wait_for(identity, loader):
    end = time.monotonic() + TIMING['reconnect']
    while time.monotonic() < end:
        try:
            device, *_ = discover(loader=True)
        except FlashError as error:
            if 'more than one' in str(error):
                raise
            device = None
        if device:
            link, found = device
            if found == identity or (loader and found.startswith('ota-')):
                return link
            link.close()
        time.sleep(TIMING['settle'])
    raise FlashError(f'device did not come back as {identity}; keep it connected and use the Workbench recovery')


def connect(pkg):
    device, inputs, outputs, errors = discover(loader=True)
    if not device:
        if not inputs or not outputs:
            raise FlashError('no MIDI ports found: connect the FM-1 by USB and switch it on')
        raise FlashError('no FM-1 answered. Close other MIDI software and the Workbench page, then retry.'
                         + (' Tried: ' + '; '.join(errors) if errors else ' Ports: ' + ', '.join(inputs)))
    link, identity = device
    if identity == pkg['loader_identity']:
        return link, identity, 'loader', None
    if identity != pkg['product'] and not open_identity(identity):
        link.close()
        raise FlashError(f'device runs "{identity}", not FM1 Open firmware: use the Workbench page (force update)')
    try:
        cap = capability(link.query(49))
    except Timeout:
        link.close()
        raise FlashError(f'"{identity}" has no Open update command: use the Workbench page (force update)')
    if cap['family'] != pkg['family']:
        link.close()
        raise FlashError('device family does not match this package: use the Workbench page')
    return link, identity, 'tagged', cap


def flash(pkg, progress):
    link, identity, mode, cap = connect(pkg)
    try:
        if mode == 'tagged':
            check(cap['state'] == 0, 'device is busy with another update')
            reply = link.query(17)
            check(len(reply) == 27 and ascii_z(reply) == identity, 'device identity changed; retry')
            cap = capability(link.query(49))
            check(cap['family'] == pkg['family'] and cap['state'] == 0, 'device state changed; retry')
            check(cap['build'] != pkg['build_id'], 'the device already runs this build')
            n = len(pkg['image'])
            ack = link.query(50, [1, cap['family'], n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255,
                                  *bytes.fromhex(pkg['build_id'])])
            check(len(ack) == 1 and ack[0] == 0, 'device refused this package')
            link.send(START_UPDATE)
            serve(link, pkg['image'], FINISH_LOADER, TIMING['idle'], 'preparing the update loader', progress)
            link.close()
            progress('waiting for the update loader', 0)
            link = wait_for(pkg['loader_identity'], loader=False)
        link.send(START_UPDATE)
        serve(link, pkg['image'], FINISH_FLASH, TIMING['write_idle'], 'writing the firmware', progress)
        link.close()
        progress('restarting, checking the new build', 99)
        link = wait_for(pkg['product'], loader=False)
        after = capability(link.query(49))
        check(after['family'] == pkg['family'] and after['build'] == pkg['build_id'] and after['state'] == 0,
              'device restarted, but the installed build does not match')
    finally:
        link.close()


# ------------------------------------------------------------------ command line
def print_progress():
    shown = {}

    def progress(phase, percent):
        step = percent // 5
        if shown.get(phase) != step:
            shown[phase] = step
            print(f'  {phase}: {percent:3d} %', flush=True)
    return progress


def main(argv=None):
    ap = argparse.ArgumentParser(description='Flash an FM-1 over USB-MIDI (FM1 Open firmware only).')
    ap.add_argument('command', choices=['ports', 'info', 'check', 'flash'])
    ap.add_argument('package', nargs='?', default=str(DEFAULT_PACKAGE))
    ap.add_argument('--meta', default=str(DEFAULT_META), help='package metadata (web-package.json)')
    ap.add_argument('--yes', action='store_true', help='flash without asking again')
    args = ap.parse_args(argv)
    try:
        if args.command == 'ports':
            _, inputs, outputs = port_pairs(loader=True)
            print('inputs: ', inputs or '-')
            print('outputs:', outputs or '-')
            return 0
        if args.command == 'info':
            device, inputs, _, errors = discover(loader=True)
            check(device, 'no FM-1 answered' + (': ' + '; '.join(errors) if errors else ' (ports: ' + ', '.join(inputs) + ')'))
            link, identity = device
            try:
                print('port:    ', link.name)
                print('identity:', identity)
                try:
                    cap = capability(link.query(49))
                    print('build:   ', cap['build'])
                    print('state:   ', 'idle' if cap['state'] == 0 else cap['state'])
                except Timeout:
                    print('build:    (no Open update command)')
            finally:
                link.close()
            return 0
        pkg = load_package(args.package, args.meta)
        print('package: ', args.package)
        print('product: ', pkg['product'], ' build', pkg['build_id'][:16], ' image', len(pkg['image']), 'bytes')
        if args.command == 'check':
            print('package OK')
            return 0
        if not args.yes:
            try:
                answer = input('Flash this firmware to the connected FM-1? Type FLASH to continue: ')
            except EOFError:
                answer = ''
            if answer.strip() != 'FLASH':
                print('cancelled')
                return 1
        print('Flashing. Keep power and USB connected.')
        flash(pkg, print_progress())
        print('Done: the FM-1 runs build', pkg['build_id'][:16])
        return 0
    except FlashError as error:
        print('error:', error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
