#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""macOS firmware build for FM1 Open.

A port of sdk/Build.ps1 (-FirmwareOnly) and sdk/Package.cs. The JieLi
Windows toolchain 2.5.3 runs under Wine; all SDK, toolchain and image checks
of the original script are kept.

Environment:
  FM1_PI32_TOOLCHAIN  pi32 directory of the extracted JieLi toolchain
  FM1_WINE            path to the wine executable
  WINEPREFIX          optional Wine prefix (default: build/wineprefix)
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / 'sdk'
PROFILE = 'dx7'
BUILD = ROOT / 'build' / PROFILE
USER_BYTES = 536576
UPDATE_FAMILY = 7
FROZEN = 'sdk/lib/libfm1_engine.a'
# APP audio-path functions added by this fork; they must run from SRAM.
APP_AUDIO_SYMBOLS = ['glitch_render', 'fx_echo', 'fx_reverb', 'trigger', 'voice_sample', 'on_tick', 'schedule',
                     'adopt_staged', 'engage', 'perform']
# APP audio functions whose direct calls must all stay inside internal SRAM.
SRAM_CALL_CHECK = APP_AUDIO_SYMBOLS + ['groove_midi_note', 'groove_midi_track_reset', 'fm1_output_mix',
                                       'fm1_audio_irq', 'fm1_audio_copy']


def fail(message):
    raise SystemExit('error: ' + message)


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha256_text(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


class Wine:
    def __init__(self, toolchain):
        exe = os.environ.get('FM1_WINE')
        if not exe:
            default = ROOT.parent / 'toolchains/wine/Wine Stable.app/Contents/Resources/wine/bin/wine'
            exe = str(default) if default.exists() else shutil.which('wine')
        if not exe or not Path(exe).exists():
            fail('Wine not found. Set FM1_WINE to the wine executable.')
        self.exe = exe
        self.bin = Path(toolchain) / 'bin'
        self.env = dict(os.environ)
        self.env.setdefault('WINEPREFIX', str(ROOT / 'build' / 'wineprefix'))
        self.env['WINEDEBUG'] = '-all'
        self.env['MVK_CONFIG_LOG_LEVEL'] = '0'
        server = Path(exe).with_name('wineserver')
        if server.exists():
            # A persistent server avoids a startup cost per compiler call.
            subprocess.run([str(server), '-p'], env=self.env, check=False, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def run(self, tool, args, cwd, capture=False):
        command = [self.exe, str(self.bin / tool)] + [str(a) for a in args]
        result = subprocess.run(command, cwd=cwd, env=self.env, text=True,
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
        if result.returncode != 0:
            if capture:
                sys.stderr.write(result.stderr or '')
            fail(f'{tool} failed ({result.returncode})')
        return result.stdout if capture else None


# ---------------------------------------------------------------- packaging
AREA, ENTRY_POINT, APP_DATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, BT_SIZE, MINIMUM_VM = 0xff000, 4096, 32768
PUBLIC_CODE = 0x980f


def _crc_table():
    table = []
    for i in range(256):
        value = i << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 0xffff
        table.append(value)
    return table


CRC_TABLE = _crc_table()


def crc16(data, begin=0, length=None):
    if length is None:
        length = len(data) - begin
    value = 0
    for b in data[begin:begin + length]:
        value = ((value << 8) & 0xffff) ^ CRC_TABLE[(value >> 8) ^ b]
    return value


def cipher(buf, begin, length, state):
    for i in range(begin, begin + length):
        buf[i] ^= state & 0xff
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xffff


def round_up(size, alignment):
    return (size + alignment - 1) & -alignment


class Image:
    def __init__(self, size, fill):
        self.b = bytearray([fill]) * size

    def number(self, offset, value, width):
        for n in range(width):
            self.b[offset + n] = (value >> (8 * n)) & 0xff

    def copy(self, offset, data):
        self.b[offset:offset + len(data)] = data

    def text(self, offset, value):
        self.copy(offset, value.encode('ascii'))

    def checksum(self, field, offset, size):
        self.number(field, crc16(self.b, offset, size), 2)

    def record(self, offset, name, crc, address, size, attributes, last, extra):
        self.b[offset:offset + 32] = bytes(32)
        self.number(offset + 2, crc, 2)
        self.number(offset + 4, address & 0xffffffff, 4)
        self.number(offset + 8, size & 0xffffffff, 4)
        self.number(offset + 12, attributes, 2)
        self.number(offset + 14, last, 2)
        for n in range(16, 32):
            self.b[offset + n] = 255
        self.text(offset + 16, name)
        self.b[offset + 16 + len(name)] = 0
        if extra is not None:
            self.number(offset + 28, extra, 4)
        self.checksum(offset, offset + 2, 30)


def read_resource(directory, name, size):
    data = (Path(directory) / name).read_bytes()
    if len(data) != size:
        fail('Unexpected FM1 resource: ' + name)
    return data


def encode_boot(plain):
    result = bytearray(plain)
    banks = struct.unpack_from('<H', plain, 0)[0]
    if banks < 1 or banks > len(plain) // 16:
        fail('Invalid boot directory.')
    previous_end = banks * 16
    for i in range(banks):
        header = i * 16
        length = struct.unpack_from('<H', plain, header + 2)[0]
        start = struct.unpack_from('<i', plain, header + 8)[0]
        if (length == 0 or start < previous_end or start > len(plain) - length or
                crc16(plain, header, 14) != struct.unpack_from('<H', plain, header + 14)[0] or
                crc16(plain, start, length) != struct.unpack_from('<H', plain, header + 12)[0]):
            fail('Invalid boot bank.')
        cipher(result, header, 16, 0xffff)
        cipher(result, start, length, 0xffff)
        previous_end = start + length
    return bytes(result)


def build_flash(application, directory, end, vm_end):
    boot = read_resource(directory, 'uboot.boot', 14488)
    config = read_resource(directory, 'boot-config.bin', 721)
    tool = read_resource(directory, 'cfg_tool.bin', 383)
    image, settings = Image(end, 255), Image(34 + len(config), 0)
    for bit in range(16):
        settings.b[bit] = 16
        settings.b[16 + bit] = 16 ^ (255 if ((PUBLIC_CODE >> bit) & 1) == 0 else 0)
    settings.checksum(32, 0, 32)
    settings.copy(34, config)
    image.copy(0, bytes([0, 0, 0x20, 2, 0x2f, 0x10, 0x4c, 0xc9, 0, 0xf0, 0x0f, 0, 0x10, 0x10, 0xa4, 0xff,
                         0x4e, 0x5d, 0x0b, 0x41, 0xe0, 0xcd, 0x78, 0x1d, 0xc8, 0x56, 0x41, 0x7f, 0xb3, 0x67, 0xcf, 0x9f]))
    image.checksum(0, 2, 30)
    config_at = 160 + len(boot)
    image.record(32, 'uboot.boot', crc16(boot), 160, len(boot), 0, 0, None)
    image.record(64, 'isd_config.ini', crc16(settings.b), config_at, len(settings.b), 0x8002, 0, None)
    image.record(96, 'app_dir_head', 65535, AREA, 0xffffffff, 0xff81, 0, None)
    image.record(128, 'key_mac', 65535, FLASH_LIMIT, 4096, 0x0112, 65535, None)
    for p in range(0, 160, 32):
        cipher(image.b, p, 32, 0xffff)
    image.copy(160, encode_boot(boot))
    image.copy(config_at, settings.b)
    payload_size = APP_DATA + len(application) + len(tool)
    app = AREA + APP_DATA
    image.copy(app, application)
    image.copy(app + len(application), tool)
    image.record(AREA + 32, 'app.bin', crc16(application), APP_DATA, len(application), 0xff82, 0, None)
    image.record(AREA + 64, 'cfg_tool.bin', crc16(tool), APP_DATA + len(application), len(tool), 0xff82, 0, None)
    image.record(AREA + 96, 'VM', 65535, end, vm_end - end, 0x8112, 0, MINIMUM_VM)
    image.record(AREA + 128, 'PRCT', 65535, 0, end, 0x8292, 0, 0x45444f43)
    image.record(AREA + 160, 'BTIF', 65535, vm_end, BT_SIZE, 0x8192, 1, 0x4f545541)
    image.record(AREA, 'app_area_head', crc16(image.b, AREA + 32, payload_size - 32), ENTRY_POINT, payload_size, 0xff83, 0, None)
    reserved = AREA + payload_size
    image.record(reserved + 32, 'USER', 65535, FLASH_LIMIT - USER_BYTES, USER_BYTES, 0x8192, 1, 0x4f545541)
    image.record(reserved, 'EXT_RESERVED', crc16(image.b, reserved + 32, 32), 32, 64, 0xff93, 1, None)
    for p in range(0, payload_size + 64, 32):
        cipher(image.b, AREA + p, min(32, payload_size + 64 - p), PUBLIC_CODE ^ (p >> 2))
    return image


def wrap(flash, loader, product):
    if len(loader) < 1 or len(loader) >= 32768:
        fail('Invalid FM1 update loader.')
    loader_at = 1024 + round_up(len(flash.b), 32)
    total = loader_at + round_up(len(loader), 32)
    ufw = Image(total, 255)
    ufw.b[0:224] = bytes(224)
    files, names = [bytes(flash.b), loader], ['flash.bin', 'ota.bin']
    for i, data in enumerate(files):
        record = 64 + i * 80
        ufw.number(record, 0 if i == 0 else 100, 2)
        ufw.number(record + 2, i, 2)
        ufw.number(record + 4, crc16(data), 2)
        ufw.number(record + 8, 1024 if i == 0 else loader_at, 4)
        ufw.number(record + 12, len(data), 4)
        ufw.number(record + 16, round_up(len(data), 32), 4)
        ufw.text(record + 64, names[i])
        cipher(ufw.b, record, 80, 0xffff)
    ufw.number(4, total, 4)
    ufw.number(8, 2, 2)
    ufw.number(10, 4, 2)
    ufw.number(12, 512, 4)
    ufw.text(16, 'AC791N')
    ufw.checksum(2, 64, 160)
    ufw.checksum(0, 2, 62)
    cipher(ufw.b, 0, 64, 0xffff)
    ufw.copy(1024, flash.b)
    ufw.copy(loader_at, loader)
    out = bytearray()
    for source in range(total):
        out.append(ufw.b[source])
        if source < 940 and (source + 1) % 47 == 0:
            index = source // 47
            out.append((ord(product[index]) + index + 1) & 0xff if index < len(product) else 125)
    return bytes(out)


def package_create(app, resources, output, product):
    if not re.fullmatch(r'FM-1_8[0-9]{6}', product or ''):
        fail('Invalid FM1 update identity.')
    if not app or len(app) > FLASH_LIMIT - USER_BYTES - BT_SIZE - MINIMUM_VM - AREA - APP_DATA - 383 - 64:
        fail('Application does not fit the FM1 layout.')
    image_size = round_up(AREA + APP_DATA + len(app) + 383 + 64, 4096)
    user_at = FLASH_LIMIT - USER_BYTES
    vm_end = user_at - BT_SIZE
    if image_size > vm_end - MINIMUM_VM:
        fail('Insufficient FM1 settings space.')
    image = build_flash(app, resources, image_size, vm_end)
    data = wrap(image, (Path(resources) / 'ota.bin').read_bytes(), product)
    Path(output).write_bytes(data)
    return {'flash_bytes': 1048576, 'board_access': False, 'fits': True, 'packager': 'source-python',
            'FLASH_BIN_SIZE': image_size, 'FLASH_NEED_SIZE': FLASH_LIMIT, 'VM_START_ADDR': image_size,
            'VM_END_ADDR': vm_end, 'VM_REAL_SIZE': vm_end - image_size, 'BTIF_RESERVED_START': vm_end,
            'BTIF_RESERVED_END': user_at, 'USER_RESERVED_START': user_at, 'USER_RESERVED_END': FLASH_LIMIT,
            'vm_minimum_bytes': MINIMUM_VM, 'vm_extra_gap_bytes': vm_end - image_size - MINIMUM_VM,
            'btif_bytes': BT_SIZE, 'user_bytes': USER_BYTES}


# ---------------------------------------------------------------- checks
def check_inputs(toolchain):
    manifest = json.loads((SDK / 'manifest.json').read_text(encoding='utf-8-sig'))
    if manifest.get('abi') != 131072:
        fail('This application requires the matching 128-frame SDK.')
    ext = manifest.get('external_toolchain')
    if ext:
        files = ext.get('files') or {}
        if ext.get('schema') != 1 or not files:
            fail('Invalid external toolchain manifest.')
        for required in ['bin/clang.exe', 'bin/pi32v2-ld.exe', 'bin/LLVMgold.dll', 'bin/llvm-nm.exe', 'bin/llvm-objcopy.exe']:
            if required not in files:
                fail('Missing toolchain contract: ' + required)
        for name, digest in files.items():
            if not re.fullmatch(r'(bin|lib)/[A-Za-z0-9_./-]+', name) or '..' in name.split('/') or not re.fullmatch(r'[a-f0-9]{64}', digest):
                fail('Invalid external toolchain entry.')
            path = Path(toolchain) / name
            if not path.exists() or sha256_file(path) != digest:
                fail(f'External toolchain does not match the SDK: {name}. JieLi Windows toolchain 2.5.3 is required.')
    synth = json.loads((SDK / 'synth.json').read_text(encoding='utf-8-sig'))
    if (synth['schema'] != 4 or synth['api'] != 8 or synth['kernel_abi'] != 3 or synth['block_frames'] != 128 or
            synth['sample_rate'] != 44100 or synth['sdk_block_sha256'] != sha256_file(SDK / 'include/fm1_block.h') or
            synth['native_sha256'] != sha256_file(SDK / 'lib/libfm1_engine.a')):
        fail('Native DSP kernels and SDK audio contracts do not match.')
    if not re.search(r'(?m)^#define FM1_DSP_ABI 3U\s*$', (ROOT / 'app/synth/engine.h').read_text()):
        fail('Unsupported native DSP ABI.')
    for name, digest in (synth.get('portable_contracts') or {}).items() if isinstance(synth.get('portable_contracts'), dict) else []:
        if not re.fullmatch(r'[A-Za-z0-9_]+\.inc', name) or digest != sha256_file(ROOT / 'app/synth' / name):
            fail('Fixed DSP coefficients differ from the native kernels.')
    if not synth['sram_required'] or not synth['sram_optional']:
        fail('Native DSP SRAM contract is missing.')
    for name, digest in manifest['files'].items():
        full = (ROOT / name).resolve()
        if SDK.resolve() not in full.parents or sha256_file(full) != digest:
            fail('SDK payload changed: ' + name)
    expected = set(manifest['files'])
    for path in SDK.rglob('*'):
        if path.is_file() and path.name != '.DS_Store':
            rel = path.relative_to(ROOT).as_posix()
            if rel != 'sdk/manifest.json' and rel not in expected:
                fail('Unexpected SDK file: ' + rel)
    return manifest, synth


def test_firmware(wine, synth, extra):
    elf = BUILD / 'sdk.elf'
    report = BUILD / 'firmware-report.json'
    report.write_text('{"passed":false}')
    info, heap, usb_midi = {}, {}, False
    pattern = re.compile(r'^([0-9a-fA-F]+)[ \t]+(([0-9a-fA-F]+)[ \t]+)?([A-Za-z])[ \t]+([^\r]+)')
    for line in wine.run('llvm-nm.exe', ['--print-size', elf], BUILD, capture=True).splitlines():
        m = pattern.match(line)
        if not m:
            continue
        name, kind = m.group(5), m.group(4)
        info[name] = (int(m.group(1), 16), int(m.group(3), 16) if m.group(3) else None, kind)
        if re.match(r'^(__fat_|fat_sdfile_fat_ops$|jl_fat_vfs_ops$|nor_sdfile_vfs_ops$|sdfile_ext_vfs_ops$)', name):
            fail('Unused filesystem entered the fixed Flash runtime: ' + name)
        if 'HEAP' in name or ('heap' in name and kind == 'A'):
            heap[name] = info[name][0]
        if name.startswith('lv_'):
            fail('Unexpected LVGL runtime: ' + name)
        if re.search(r'usb.*midi|midi.*usb', name):
            usb_midi = True
    addr = lambda n: info[n][0]
    registrations = [('initcall', 4, 4), ('early_initcall', 16, 4), ('late_initcall', 4, 4),
                     ('platform_initcall', 4, 4), ('module_initcall', 4, 4), ('sys_event_handler', 12, 12),
                     ('syscfg_ops', 84, 28), ('lp_target', 72, 8), ('sys_cpu_timer', 16, 16),
                     ('clock_critical_handler', 16, 8), ('vfs_ops', 120, 120), ('audio_decoder', 56, 56),
                     ('audio_hwaccel', 32, 32)]
    for name, minimum, step in registrations:
        begin, end = f'_{name}_begin', f'_{name}_end'
        if begin not in info or end not in info:
            begin, end = f'{name}_begin', f'{name}_end'
        if begin not in info or end not in info:
            fail('Runtime registration range missing: ' + name)
        size = addr(end) - addr(begin)
        if size < minimum or size % step:
            fail('Runtime registration table invalid: ' + name)
        if name == 'vfs_ops' and size != 120:
            fail('Expected only the internal Flash filesystem registration.')
    required = ['fm1_r_7919135c9963', 'fm1_r_1f449c2c7b8f', 'vSoftwareInterruptISR', 'fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3',
                'fm1_r_05431712fb7b', 'fm1_update_init', 'fm1_update_message', 'fm1_update_busy', 'fm1_build_id', 'fm1_flash_init',
                'sdfile_vfs_ops', 'sdfile_init', 'syscfg_vm_init', 'syscfg_btif_init', 'wifi_get_xosc', 'wifi_get_mcs_dgain',
                'wifi_get_pa_trim_data', 'connectivity_wl82_bluetooth_start', 'ble_profile_init', 'att_server_init',
                'fm1_ble_set_role', 'fm1_ble_scan', 'fm1_ble_connect', 'fm1_ble_disconnect', 'fm1_ble_get_status', 'fm1_ble_get_device',
                'audio_app_usb_microphone_enable', 'audio_app_usb_speaker_enable', 'connectivity_audio_usb_microphone_enable',
                'connectivity_audio_usb_speaker_enable', 'audio_app_bluetooth_open', 'sbc_decoder', 'sbc_hwaccel',
                'avdtp_packet_handler', 'bt_get_local_name', 'bt_get_mac_addr', 'fm1_bluetooth_configure',
                'fm1_bluetooth_get_status', 'groove_tuning_capture', 'groove_tuning_apply', 'groove_project_apply_edit']
    if re.search(r'(?m)^#define FM1_GLITCH 1\b', (ROOT / 'app/glitch.h').read_text()):
        required.remove('groove_project_apply_edit')  # GLITCH loads no FM1 projects
    for name in required:
        if name not in info:
            fail('Required runtime symbol absent: ' + name)
    for name in ['sdp_send_cmd_iotl', 'sdp_master_packet_handler', 'gatt_client_discover_primary_services_by_uuid128']:
        if name not in info or info[name][2] not in 'TtNn' or not info[name][1] or info[name][1] <= 16:
            fail('Bluetooth discovery implementation missing or stubbed: ' + name)
    for name in ['fm1_r_dee1ea411566', 'fm1_r_d16405f329ed', 'fm1_r_f03cc16d330d', 'fm1_r_1993f9b103e3',
                 'fm1_io_trs_midi_enabled', 'fm1_r_05431712fb7b', 'fm1_r_0aa010fe1de1', 'fm1_r_33d5e5b63b6b', 'fm1_app_sysex']:
        if name not in info or info[name][2] not in 'TNtn' or not info[name][1]:
            fail('Application callback was lost or replaced by a weak default: ' + name)
    if info['fm1_build_id'][1] != 32:
        fail('Browser update build identity must be exactly 32 bytes.')
    if info['fm1_update_identity'][1] != 27:
        fail('FM1 update identity must be exactly 27 bytes.')
    if not usb_midi:
        fail('USB MIDI runtime is missing.')
    for name in ['CurrentTCB', 'highCurrentTCB', 'xIdleTaskHandle']:
        if name not in info or info[name][1] != 4:
            fail('Platform RTOS ABI is invalid: ' + name)
    for name in ['__rt_local_irq_disable', '__rt_local_irq_enable', 'bt_lock_cnt', 'fm1_r_83b2de623a96', 'fm1_r_c327ea18da09',
                 'cpuSuspend', 'audio_app_worker', 'edr_name', 'ble_name', 'a2dp_source_event_handler', 'a2dp_source_sbc_codec',
                 'hfp_profile_init', 'hfp_ag_profile_init', 'spp_user_send_data', 'avrcp_player_event', 'hid_diy_regiest_callback',
                 'obex_connection_req', 'pan_cmd_io_ctrl', 'gatt_client_discover_primary_services', 'ancs_client_init',
                 'ams_client_init', 'sdio_host_init', 'wl30_sdio_claim_irq', 'wl30_sdio_release_irq', 'wifi_on', 'wifi_off',
                 'wifi_rx_task', 'wifi_tx_task', 'ui_yuv420_to_rgb565_pairs', 'ui_tiny_ttf_pack_scanline']:
        if name in info:
            fail('Unsupported platform runtime entered FM1: ' + name)
    linkmap = (BUILD / 'sdk.map').read_text(errors='replace')
    if FROZEN + '(' not in linkmap:
        fail('Fixed native SDK archive missing.')
    if 'libfm1_runtime.a(' in linkmap:
        fail('Only the fixed music/BLE SDK may be linked.')
    for item in ['/system.a(', 'audio_server.a(', 'media_app.a(', 'btstack.a(', 'lib_ui.a(', 'libjpeg_decode_only.a(']:
        if item in linkmap:
            fail('Unsupported FM1 link input: ' + item)
    for member in ['a2dp_source', 'ams_client', 'ancs_client', 'avctp', 'avrcp', 'bip', 'bnep', 'bt_audio', 'client_user',
                   'hfp_ag_profile', 'hfp_profile', 'hid', 'iap_profile', 'obex', 'opp', 'pan', 'pbg_profile', 'rfcomm',
                   'spp_profile', 'spp_update_profile', 'spp_user', 'vCard']:
        if f'.a({member}.c.o)' in linkmap:
            fail('Unsupported FM1 link member: ' + member)
    acl = {}
    for direction in ['tx', 'rx']:
        begin = f'acl_{direction}_pool'
        end = begin + '_end'
        if begin not in info or end not in info:
            fail('Required controller pool missing: ' + begin)
        acl[direction] = addr(end) - addr(begin)
    if acl['tx'] < 1024 or acl['rx'] < 4096:
        fail('Classic ACL pools too small; need 1024/4096 bytes.')
    m = re.search(r'0x([0-9a-fA-F]+)[^\r\n]*_MALLOC_SIZE[ ]*=', linkmap)
    if not m:
        fail('Cannot find runtime heap in link map.')
    heap_bytes = int(m.group(1), 16)
    if heap_bytes < 49152:
        fail(f'FM1 runtime heap is {heap_bytes} bytes; need at least 49152.')
    callbacks = ['fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3', 'fm1_r_05431712fb7b', 'fm1_r_ffe3f82683f6', 'fm1_audio_irq',
                 'fm1_flash_worker_prepare', 'fm1_r_7e12ed19990d', 'fm1_r_74310ca1fed9', 'fm1_engine_render',
                 'fm1_engine_output_level', 'fm1_audio_copy', 'fm1_output_mix', 'fm1_usb_audio_read',
                 'fm1_bluetooth_audio_read', 'fm1_usb_audio_write', 'connectivity_audio_get_speaker_volume',
                 'connectivity_audio_get_mic_volume'] + list(extra)
    for name in list(synth['sram_optional']) + list(synth.get('native_sram_optional', [])):
        if name in info:
            callbacks.append(name)
    callbacks += ['cpu_enter_sfc_critical', 'cpu_exit_sfc_critical', 'enter_spi_code', 'exit_spi_code',
                  'fm1_r_da3931a5b7a8', 'sfc_drop_cache']
    for name in ['audio_app_usb_mix', 'audio_app_usb_record', 'audio_app_usb_replace_input', 'render_stereo',
                 'effects_render', 'effects_sample', 'sequence_event', 'clock_publish', 'groove_arp_event',
                 'groove_arp_running', 'engine_note'] + APP_AUDIO_SYMBOLS:
        if name in info:
            callbacks.append(name)
    engine_ready = 'fm1_r_0aa010fe1de1' in info and addr('fm1_r_0aa010fe1de1') > 0
    if engine_ready:
        callbacks.append('fm1_r_0aa010fe1de1')
        if 'fm1_r_33d5e5b63b6b' in info and addr('fm1_r_33d5e5b63b6b') > 0:
            callbacks.append('fm1_r_33d5e5b63b6b')
    for name in callbacks:
        if name not in info:
            fail('Audio callback absent: ' + name)
        if not 0x01c00000 <= addr(name) < 0x01c80000:
            fail('Audio callback is outside internal SRAM: ' + name)
    for name in ['fm1_input_tick', 'fm1_panel_irq', 'fm1_scan_decode', 'fm1_key_pin', 'fm1_encoder_pin', 'fm1_quadrature']:
        if name not in info or not 0x01c00000 <= addr(name) < 0x01c80000:
            fail('Panel scan code or wiring table is outside internal SRAM: ' + name)
    data = elf.read_bytes()
    if data[:6] != b'\x7fELF\x01\x01':
        fail('Expected ELF32 little endian.')
    entry = struct.unpack_from('<I', data, 24)[0]
    if entry != 0x02000120 or '_start' not in info or addr('_start') != entry or \
            '_text_rodata_begin' not in info or addr('_text_rodata_begin') != entry:
        fail('Firmware startup must match the fixed ROM entry and the first packaged instruction.')
    table = struct.unpack_from('<I', data, 32)[0]
    stride, count, strings_index = struct.unpack_from('<HHH', data, 46)
    if stride < 40 or not count or strings_index >= count:
        fail('Invalid ELF section table.')
    sh = table + strings_index * stride
    str_off, str_size = struct.unpack_from('<II', data, sh + 16)
    sections, payload, app_image = [], 0, 0
    for i in range(count):
        h = table + i * stride
        name_off, kind, flags, address, offset, size = struct.unpack_from('<IIIIII', data, h)
        if not (flags & 2) or not size:
            continue
        end = data.index(b'\0', str_off + name_off)
        name = data[str_off + name_off:end].decode('ascii')
        if not re.fullmatch(r'\.(text|data|bss|ram0_data|ram0_bss|boot_info|cache_ram_data|cache_ram_bss)', name):
            fail('Unsupported allocated section: ' + name)
        nobits = kind == 8
        if not nobits:
            payload += size
            if re.fullmatch(r'\.(text|data|ram0_data|cache_ram_data)', name):
                app_image += size
            elif name != '.boot_info':
                fail('Allocated section is not covered by FM1 packaging: ' + name)
        sections.append({'name': name, 'address': address, 'bytes': size, 'nobits': nobits})
    check_sram_calls(wine, elf)
    result = {'passed': True, 'elf_sha256': sha256_file(elf), 'allocated_image_bytes': payload,
              'app_image_bytes': app_image, 'startup_entry': entry, 'instrument_ready': engine_ready,
              'sections': sections, 'heap_symbols': heap, 'runtime_heap_bytes': heap_bytes,
              'classic_acl_tx_bytes': acl['tx'], 'classic_acl_rx_bytes': acl['rx'],
              'audio_callbacks_in_sram': True, 'board_validation': 'not performed', 'builder': 'tools/build_mac.py'}
    report.write_text(json.dumps(result, indent=2))
    print(f'FM1: {app_image} bytes app image; SRAM/BLE/USB checks passed')


def check_sram_calls(wine, elf):
    """Flash is unreadable while it is being written: SRAM audio code must
    never call into Flash (for example a libc memset the compiler emitted)."""
    sizes = {}
    for line in wine.run('llvm-nm.exe', ['--print-size', elf], BUILD, capture=True).splitlines():
        p = line.split()
        if len(p) == 4:
            sizes.setdefault(p[3], []).append((int(p[0], 16), int(p[1], 16)))
    listing = wine.run('llvm-objdump.exe', ['-d', '-j', '.ram0_data', elf], BUILD, capture=True)
    code = []
    for line in listing.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s', line)
        if m:
            code.append((int(m.group(1), 16), line))
    checked = 0
    for name in SRAM_CALL_CHECK:
        for start, size in sizes.get(name, []):
            if not 0x01c00000 <= start < 0x01c80000:
                continue
            checked += 1
            for address, line in code:
                if start <= address < start + size:
                    m = re.search(r'\b(?:call|goto)\b[^<]*<(\S+) : ([0-9a-f]+) ?>', line)
                    # Only targets inside the Flash code are a hazard. Other "targets"
                    # come from FPU instructions this disassembler does not decode.
                    if m and 0x02000000 <= int(m.group(2), 16) < 0x02800000:
                        fail(f'SRAM audio function {name} calls Flash code {m.group(1)}')
    if not checked:
        fail('SRAM call check found no audio functions.')
    print(f'SRAM call check: {checked} audio functions call only SRAM code')


def package_firmware(wine, product):
    generated = BUILD / 'generated'
    elf = BUILD / 'sdk.elf'
    before = sha256_file(elf)
    output = generated / 'FM1_OFFICIAL_OPEN.fwsc'
    if struct.unpack_from('<I', elf.read_bytes(), 24)[0] != 0x02000120:
        fail('Package entry must match the verified FM1 firmware entry.')
    image = bytearray()
    try:
        for section in ['text', 'data', 'ram0_data', 'cache_ram_data']:
            wine.run('llvm-objcopy.exe', ['-O', 'binary', '-j', '.' + section, elf, section + '.bin'], generated)
            image += (generated / (section + '.bin')).read_bytes()
        layout = package_create(bytes(image), SDK / 'tools/pack', output, product)
    finally:
        for section in ['text', 'data', 'ram0_data', 'cache_ram_data']:
            (generated / (section + '.bin')).unlink(missing_ok=True)
    if not output.exists() or sha256_file(elf) != before:
        fail('Firmware packaging did not produce a matching image.')
    (BUILD / 'package-report.json').write_text(json.dumps(layout, indent=2))
    print(f"Offline package fits 1 MiB: image {layout['FLASH_BIN_SIZE']}, VM {layout['VM_REAL_SIZE']}, user {layout['user_bytes']} bytes")
    return output


def main():
    toolchain = os.environ.get('FM1_PI32_TOOLCHAIN') or str(ROOT.parent / 'toolchains/jl/C$/JL/pi32')
    toolchain = Path(toolchain).resolve()
    if (toolchain / 'clang.exe').exists():
        toolchain = toolchain.parent
    if not (toolchain / 'bin/clang.exe').exists():
        fail('Set FM1_PI32_TOOLCHAIN to the pi32 directory of JieLi toolchain 2.5.3.')
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'firmware-report.json').write_text('{"passed":false}')
    (BUILD / 'package-report.json').write_text('{"board_access":false,"fits":false}')
    for name in ['sdk.elf', 'sdk.map', 'build-identity.json', 'generated/FM1_OFFICIAL_OPEN.fwsc']:
        (BUILD / name).unlink(missing_ok=True)
    version_h = (ROOT / 'app/version.h').read_text()
    m = re.search(r'(?m)^#define FM1_RELEASE_VERSION "(V(?:0[1-9]|[1-9][0-9]{1,2}))"\s*$', version_h)
    if not m:
        fail('Set FM1_RELEASE_VERSION in app/version.h.')
    release = m.group(1)
    m = re.search(r'(?m)^#define FM1_UPDATE_PRODUCT "(FM-1_8[0-9]{6})"\s*$', version_h)
    if not m:
        fail('Set a compatible FM1_UPDATE_PRODUCT in app/version.h.')
    product = m.group(1)
    print(f'Build FM1 Open {release} on macOS')
    manifest, synth = check_inputs(toolchain)
    wine = Wine(toolchain)

    sources = sorted((p.relative_to(ROOT).as_posix() for p in (ROOT / 'app').rglob('*')
                      if p.is_file() and p.suffix in ('.c', '.S') and (ROOT / 'app/synth') not in p.parents),
                     key=str.lower)
    extra = ['groove_sequence_render', 'groove_sequence_clock', 'groove_sequence_position', 'groove_sequence_transport',
             'fm1_sea_decode_groups', 'fm1_codec_sea_mono_s16_run'] + list(synth['sram_required']) + [
        'synth_dx7_render', 'synth_dx7_midi', 'synth_dx7_key', 'fm1_dx7_panic', 'fm_exp2_fixed', 'bass303_bank_render',
        'bass303_bank_key', 'bass303_bank_set', 'drum808_kit_render', 'drum808_kit_trigger', 'psg_render', 'psg_key',
        'psg_midi', 'psg_panic', 'psg_set', 'psg_get', 'pluck_render', 'pluck_key', 'pluck_midi', 'pluck_panic',
        'pluck_set', 'pluck_get', 'fm1_mix128', 'fm1_mix_mono_stereo128', 'fm1_bus_output128', 'fm1_master_mix128',
        'fm1_master_gain', 'fm1_declick128', 'fm1_render_begin', 'fm1_render_end', 'synth_dx7_active',
        'bass303_bank_active', 'drum808_kit_active', 'psg_active', 'pluck_active', 'sampledrum_active',
        'synth_dx7_limit', 'psg_limit', 'pluck_limit', 'sampledrum_limit', 'bass303_advance_idle', 'fm1_send128',
        'fm1_fx_render', 'fm1_fx_clear', 'fm1_fx_active', 'sampledrum_render', 'sampledrum_trigger',
        'sampledrum_panic', 'psg_active_chips']
    obj = BUILD / 'obj'
    gen = BUILD / 'generated'
    obj.mkdir(exist_ok=True)
    gen.mkdir(exist_ok=True)
    for f in obj.iterdir():
        if f.is_file():
            f.unlink()
    flags = ['-target', 'pi32v2', '-integrated-as', '-mcpu=r3', '-mfprev1', '-Wuninitialized',
             '-Wno-invalid-noreturn', '-fno-common', '-flto', '-fallow-pointer-null', '-fprefer-gnu-section',
             '-femulated-tls', '-Wno-shift-negative-value', '-Wframe-larger-than=2560', '-fms-extensions',
             '-fno-unwind-tables', '-ffunction-sections', '-fdata-sections', '-fmessage-length=0',
             '-cl-single-precision-constant', '-mllvm', '-pi32v2-large-program=true', '-D__GCC_PI32V2__', '-O2']
    for d in ['sdk/include', 'sdk/include/libc', 'app/synth', 'sdk/config', 'app', 'app/board']:
        flags.append('-I' + str(ROOT / d))
    objects, commands = [], []
    for index, source in enumerate(sources):
        output = f'obj/{index}_' + re.sub(r'[^a-zA-Z0-9._-]', '_', source) + '.obj'
        options = list(flags)
        if source in ('app/screen.c', 'app/transfer.c', 'app/midi_router.c'):
            options.append('-fno-lto')
        if source == 'app/board/panel.c':
            options += ['-fno-lto', '-fno-pic']
        options += ['-fwrapv', '-fno-fast-math', '-ffp-contract=off']
        if source in ('app/project.c', 'app/transfer.c', 'app/samples.c', 'app/screen.c', 'app/presets.c',
                      'app/midi_router.c', 'app/fm_factory.c', 'app/fm_library.c', 'app/custom_engine.c'):
            options.append('-Oz')
        if source == 'app/screen.c':
            options.append('-fno-inline-functions')
        args = options + ['-MMD', '-MF', output + '.d', '-c', str(ROOT / source), '-o', output]
        print('Compile ' + source, flush=True)
        wine.run('clang.exe', args, BUILD)
        objects.append(output)
        commands.append({'directory': str(BUILD), 'file': str(ROOT / source), 'output': str(BUILD / output),
                         'arguments': ['clang.exe'] + args})
    identity = [PROFILE, sha256_file(SDK / 'manifest.json')]
    for p in sorted((p for p in (ROOT / 'app').rglob('*') if p.is_file()), key=lambda p: str(p).lower()):
        identity.append(p.relative_to(ROOT).as_posix() + ':' + sha256_file(p))
    build_id = sha256_text('\n'.join(identity))
    id_bytes = ','.join('0x' + build_id[i:i + 2] for i in range(0, 64, 2))
    (gen / 'build_identity.c').write_text(
        '#include "layout.h"\n'
        f'const struct fm1_firmware_layout fm1_layout = {{{USER_BYTES}U,{UPDATE_FAMILY}U}};\n'
        f'const unsigned char fm1_build_id[32] = {{{id_bytes}}};\n'
        f'const unsigned char fm1_update_identity[27] = "{product}";\n')
    wine.run('clang.exe', flags + ['-c', 'generated/build_identity.c', '-o', 'obj/build_identity.obj'], BUILD)
    objects.append('obj/build_identity.obj')
    (BUILD / 'build-identity.json').write_text(json.dumps(
        {'profile': PROFILE, 'family': 'release', 'release_version': release, 'build_id': build_id}, indent=2))
    linker = (SDK / 'config/linker.ld.in').read_text().replace('@SBC_DECODER@', 'sbc_decoder\n') \
        .replace('@ACL_TX_EXTRA@', ' + 1024').replace('@ACL_RX_EXTRA@', ' + 4096')
    (gen / 'linker').write_text(linker)
    link = ['--plugin', str(toolchain / 'bin/LLVMgold.dll'), '-o', 'sdk.elf'] + objects + [
        '-flto', '--plugin-opt=-pi32v2-always-use-itblock=false', '--plugin-opt=-enable-ipra=true',
        '--plugin-opt=-pi32v2-merge-max-offset=4096', '--plugin-opt=-pi32v2-enable-simd=true', '--plugin-opt=mcpu=r3',
        '--plugin-opt=-global-merge-on-const', '--plugin-opt=-inline-threshold=5', '--plugin-opt=-inline-max-allocated-size=32',
        '--plugin-opt=-inline-normal-into-special-section=true',
        '--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar',
        '--plugin-opt=-pi32v2-enable-rep-memop', '--plugin-opt=-emulated-tls', '--sort-common',
        '--plugin-opt=-pi32v2-large-program=true', '--plugin-opt=-pi32v2-csync-before-rti=true', '--start-group',
        '--whole-archive', '../../' + FROZEN, '--no-whole-archive',
        '../../sdk/lib/libc.a', '../../sdk/lib/libcompiler_rt.a',
        '--end-group', '--gc-sections', '-Tgenerated/linker', '-M=sdk.map', '--undefined=connectivity_wl82_bluetooth_start',
        '--undefined=audio_app_bluetooth_open', '--undefined=bt_get_local_name', '--undefined=bt_get_mac_addr']
    (BUILD / 'compile_commands.json').write_text(json.dumps(commands, indent=2))
    (BUILD / 'link-arguments.json').write_text(json.dumps(link, indent=2))
    print(f'Link {PROFILE} using prebuilt FM1 SDK', flush=True)
    wine.run('pi32v2-ld.exe', link, BUILD)
    test_firmware(wine, synth, extra)
    output = package_firmware(wine, product)
    print(f'Firmware: {output}')
    print(f'SHA-256:  {sha256_file(output)}')
    write_update_page(output, product, release, build_id)


def write_update_page(firmware, product, release, build_id):
    """Embed the package and the original V15 recovery image into the
    workbench page, like Build.ps1 -Package (local, unsigned distribution)."""
    import base64
    stock_file = SDK / 'firmware/FM-1_015.fwsc'
    stock_hash = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a'
    if sha256_file(stock_file) != stock_hash:
        fail('Matching SDK original V15 recovery firmware is missing or invalid.')
    stock = stock_file.read_bytes()
    data = Path(firmware).read_bytes()
    package = {'schema': 1, 'product': product, 'profile': PROFILE, 'family': UPDATE_FAMILY, 'release_version': release,
               'build_id': build_id, 'sha256': sha256_file(firmware), 'bytes': len(data),
               'loader_sha256': sha256_file(SDK / 'tools/pack/ota.bin'), 'hardware_validated': False,
               'data': base64.b64encode(data).decode(),
               'stock': {'schema': 1, 'kind': 'stock', 'product': 'FM-1_015', 'profile': 'M-VAVE Original V15',
                         'family': 7, 'release_version': 'V15', 'build_id': stock_hash, 'sha256': stock_hash,
                         'bytes': len(stock), 'loader_sha256': 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541',
                         'data': base64.b64encode(stock).decode()}}
    path = BUILD / 'web-package.json'
    path.write_text(json.dumps(package, separators=(',', ':')))
    if subprocess.run([sys.executable, '-I', str(ROOT / 'tools/build_web_mac.py'), '--package', str(path)]).returncode:
        fail('update page build failed')


if __name__ == '__main__':
    main()
