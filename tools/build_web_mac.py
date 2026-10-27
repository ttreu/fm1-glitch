#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""macOS port of tools/Build-Web.ps1 -LocalPreview.

Builds the browser simulator (WebAssembly) and writes build/preview/index.html.
Set FM1_WASI_SDK to an extracted WASI SDK 27 (macOS build).
"""
import base64
import hashlib
import html
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def fail(message):
    raise SystemExit('error: ' + message)


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    package_json = None
    if '--package' in sys.argv:
        package_json = Path(sys.argv[sys.argv.index('--package') + 1]).read_text()
    wasi = os.environ.get('FM1_WASI_SDK') or str(ROOT.parent / 'toolchains/wasi/wasi-sdk-27.0-arm64-macos')
    wasi = Path(wasi)
    compiler = wasi / 'bin/clang'
    if not compiler.exists() or not (wasi / 'share/wasi-sysroot').exists():
        fail('Set FM1_WASI_SDK to an extracted WASI SDK 27 directory.')
    out = ROOT / 'build/web'
    out.mkdir(parents=True, exist_ok=True)
    names = []
    for base in ['app', 'web', 'third_party/synth']:
        for p in (ROOT / base).rglob('*'):
            rel = p.relative_to(ROOT).as_posix()
            if p.is_file() and not rel.startswith('app/board/') and p.suffix in ('.c', '.h', '.inc'):
                names.append(rel)
    names.sort()
    inputs = {n: sha256_file(ROOT / n) for n in names}
    digest = hashlib.sha256('\n'.join(f'{n}:{inputs[n]}' for n in names).encode()).hexdigest()
    wasm = out / 'simulator.wasm'
    options = ['--sysroot=' + str(wasi / 'share/wasi-sysroot'), '-std=c11', '-O2', '-fwrapv', '-fno-fast-math',
               '-ffp-contract=off', '-DFM1_HOST_TEST', '-DFM1_OPEN_WEB', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-array-parameter', '-mexec-model=reactor']
    for d in ['web/engine', 'web/synth', 'third_party/synth', 'sdk/include', 'app/synth', 'app']:
        options.append('-I' + str(ROOT / d))
    options += [str(ROOT / n) for n in names if n.endswith('.c')]
    options += ['-Wl,--strip-all', '-Wl,-z,stack-size=131072', '-Wl,--initial-memory=4194304',
                '-Wl,--max-memory=33554432', '-o', str(wasm)]
    print('Compile the browser application', flush=True)
    wasm.unlink(missing_ok=True)
    if subprocess.run([str(compiler)] + options).returncode != 0:
        fail('compile failed')
    version = re.search(r'(?m)^#define FM1_RELEASE_VERSION "(V(?:0[1-9]|[1-9][0-9]{1,2}))"\s*$',
                        (ROOT / 'app/version.h').read_text())
    if not version:
        fail('Set FM1_RELEASE_VERSION in app/version.h.')
    data = wasm.read_bytes()
    simulation = json.dumps({'schema': 1, 'release_version': version.group(1), 'sha256': hashlib.sha256(data).hexdigest(),
                             'source_sha256': digest, 'sample_bank': 'synthesized-demo',
                             'data': base64.b64encode(data).decode()}, separators=(',', ':'))
    web = (ROOT / 'sdk/Update.html.in').read_text(encoding='utf-8')
    web = re.sub(r'<a\b[^>]*href="/\*FM1_SOURCE_URL\*/"[^>]*>(.*?)</a>', r'<span>\1 (local build)</span>', web)
    notices = ['APP: GPLv3 with the FM1 SDK linking exception; no warranty. SDK and firmware resources have separate '
               'redistribution terms in THIRD_PARTY.md. Source: (local build, GLITCH fork)']
    if package_json:
        notices.append('FM-1 V15 recovery firmware: https://www.m-vave.com/download (PC Firmware / FM-1 V15). '
                       'Distributed by M-VAVE under Apache-2.0. See THIRD_PARTY.md.')
    for f in ['THIRD_PARTY.md', 'LICENSE.md']:
        notices.append(f'\n===== {f} =====\n' + (ROOT / f).read_text(encoding='utf-8'))
    replacements = {
        '/*FM1_SOURCE_URL*/': '',
        '/*FM1_PAYLOAD*/': 'const FM1_PACKAGE = ' + (package_json or 'null') + ';',
        '/*FM1_SIMULATOR*/': 'const FM1_SIMULATOR = ' + simulation + ';',
        '/*FM1_I18N*/': (ROOT / 'sdk/I18n.js').read_text(encoding='utf-8'),
        '/*FM1_SCRIPT*/': (ROOT / 'sdk/WebUpdate.js').read_text(encoding='utf-8'),
        '/*FM1_SIM_SCRIPT*/': (ROOT / 'sdk/Simulator.js').read_text(encoding='utf-8'),
        '/*FM1_SAMPLE_SCRIPT*/': (ROOT / 'sdk/Samples.js').read_text(encoding='utf-8'),
        '/*FM1_NOTICES*/': html.escape('\n'.join(notices), quote=True),
    }
    for key, value in replacements.items():
        web = web.replace(key, value)
    web = web.replace('<title>FM1 工作台</title>', '<title>FM1 GLITCH</title>')
    if re.search(r'/\*FM1_[A-Z_]+\*/', web):
        fail('Unexpanded HTML placeholder.')
    output = ROOT / ('build/official-web/FM1-GLITCH.html' if package_json else 'build/preview/index.html')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(web, encoding='utf-8')
    print(f'Preview: {output}')


if __name__ == '__main__':
    main()
