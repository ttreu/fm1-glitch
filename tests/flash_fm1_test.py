#!/usr/bin/env python3
"""Test tools/flash_fm1.py against a simulated FM-1 (no MIDI hardware).

The fake device answers identity (17), capability (49) and arm (50), then
requests the package block by block (48) like the update loader, restarts
as the loader, is written, and restarts with the new build.
"""
import hashlib
import queue
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import flash_fm1 as f  # noqa: E402

fails = 0


def check(ok, text):
    global fails
    print(('PASS ' if ok else 'FAIL ') + text)
    if not ok:
        fails += 1


class FakeDevice:
    def __init__(self, pkg, identity=None, build='00' * 32, refuse=False, has_cap=True):
        self.pkg, self.identity, self.build = pkg, identity or pkg['product'], build
        self.refuse, self.has_cap = refuse, has_cap
        self.state = 'app'                 # app -> loader -> app (new build)
        self.armed = False
        self.phase = None
        self.received = {}
        self.link = None

    def reply(self, command, data):
        self.link.replies.put(f.parse(f.frame(command, data)))

    def request(self, address, count):
        self.reply(48, [7, *address.to_bytes(4, 'little'), *count.to_bytes(3, 'little')])

    def current_identity(self):
        return self.pkg['loader_identity'] if self.state == 'loader' else self.identity

    def handle(self, message):
        if message == f.START_UPDATE:
            self.phase = {'next': 0, 'end': len(self.pkg['image']) if self.state == 'loader' else 4096,
                          'finish': f.FINISH_FLASH if self.state == 'loader' else f.FINISH_LOADER}
            assert self.state == 'loader' or self.armed, 'started without arming'
            self.request(0, 512)
            return
        command, data = f.parse(message)
        if command == 17:
            name = self.current_identity().encode().ljust(27, b'\0')
            self.reply(17, list(name))
        elif command == 49 and self.state == 'app' and self.has_cap:
            cap = b'FMC1' + bytes([1, 7, 0, 1]) + (512).to_bytes(2, 'little') + (1048576).to_bytes(4, 'little')
            self.reply(49, list(cap + bytes.fromhex(self.build) + b'\0'))
        elif command == 50:
            n = len(self.pkg['image'])
            ok = (data[0] == 1 and data[1] == 7 and int.from_bytes(data[2:6], 'little') == n and
                  data[6:].hex() == self.pkg['build_id'] and not self.refuse)
            self.armed = ok
            self.reply(50, [0 if ok else 1])
        elif command == 48 and self.phase:
            address = int.from_bytes(data[1:5], 'little')
            if address == self.phase['finish']:
                assert data[8:] == b'success\0'
                if self.state == 'app':
                    self.state = 'loader'
                else:
                    self.state = 'app'
                    self.build = self.pkg['build_id']
                self.phase = None
                return
            block = data[8:]
            assert len(block) == int.from_bytes(data[5:8], 'little')
            self.received[address] = bytes(block)
            nxt = address + len(block)
            if nxt >= self.phase['end']:
                self.request(self.phase['finish'], 8)
            else:
                self.request(nxt, min(512, self.phase['end'] - nxt))


class FakeLink(f.Link):
    def __init__(self, device):
        self.name = 'FM-1 (simulated)'
        self.replies = queue.Queue()
        self.device = device
        device.link = self

    def send(self, message):
        self.device.handle(list(message))

    def close(self):
        pass


def run(device):
    f.discover = lambda loader=True: ((FakeLink(device), device.current_identity()), ['FM-1'], ['FM-1'], [])
    f.TIMING['settle'] = 0.05
    f.flash(pkg, lambda phase, percent: None)


pkg = f.load_package(f.DEFAULT_PACKAGE, f.DEFAULT_META)

# A device running another Open build: full update, every byte as in the package.
dev = FakeDevice(pkg)
run(dev)
image = bytearray(len(pkg['image']))
for address, block in dev.received.items():
    image[address:address + len(block)] = block
check(dev.state == 'app' and dev.build == pkg['build_id'], 'update runs through: loader, write, restart with the new build')
check(hashlib.sha256(image).digest() == hashlib.sha256(pkg['image']).digest(), 'the device received the package byte for byte')

# Refusals: same build, a foreign firmware, no Open command, the device says no.
for name, device, expect in [
    ('same build', FakeDevice(pkg, build=pkg['build_id']), 'already runs this build'),
    ('stock firmware', FakeDevice(pkg, identity='FM-1_015'), 'Workbench'),
    ('no Open update command', FakeDevice(pkg, identity='FM1_COMMUNITY_1', has_cap=False), 'Workbench'),
    ('device refuses the package', FakeDevice(pkg, refuse=True), 'refused'),
]:
    try:
        run(device)
        check(False, f'{name}: refused')
    except f.FlashError as error:
        check(expect in str(error) and device.state == 'app' and not device.received, f'{name}: refused before writing ({error})')

sys.exit(1 if fails else 0)
