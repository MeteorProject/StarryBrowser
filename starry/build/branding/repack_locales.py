#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors

"""Chrome locale repacker with Starry product names in every translation."""

from pathlib import Path
import os
import re
import sys

# This file is reached through src/starry, including in a fresh checkout.
SRC = Path(__file__).absolute().parents[3]
if not (SRC / 'tools/grit').is_dir():
    SRC = Path(os.environ.get('STARRY_CHROMIUM_ROOT', SRC / 'chromium')) / 'src'
sys.path.insert(0, str(SRC / 'tools/grit'))
BRANDING = dict(line.split('=', 1) for line in
                (SRC / 'starry/app/theme/BRANDING').read_text().splitlines() if '=' in line)
PRODUCT_NAME = BRANDING['PRODUCT_SHORTNAME']

import pak_util
from grit import constants
from grit.format import data_pack

# Keep external product names, URLs, API identifiers and legal attribution.
BRAND = re.compile(
    r'(?<![A-Za-z0-9_./:@-])(?:Google Chrome|Chromium|Chrome)'
    r'(?![A-Za-z0-9_/:@-]|\.[A-Za-z0-9_]| Web Store| OS| 应用商店| 線上應用程式商店| ウェブストア| 웹 스토어)')
PROTECTED = re.compile(r'COPYRIGHT|LICENSE|CREDITS|TERMS|LEGAL')


def rebrand(text):
    result = BRAND.sub(PRODUCT_NAME, text)
    # Chromium/Chrome end in a consonant in Korean; Starry/Browser do not.
    particles = {'으로': '로', '이': '가', '은': '는', '을': '를', '과': '와'}
    return re.sub(re.escape(PRODUCT_NAME) + r'(으로|이|은|을|과)(?=[\s,.!?…])',
                  lambda match: PRODUCT_NAME + particles[match[1]], result)


def repack(output_file, input_files, *args, **kwargs):
    original_repack(output_file, input_files, *args, **kwargs)
    pak = data_pack.ReadDataPack(output_file)
    if pak.encoding not in (data_pack.UTF8, data_pack.UTF16):
        raise ValueError('Starry branding expects a locale pack')
    encoding = 'utf-8' if pak.encoding == data_pack.UTF8 else 'utf-16-le'
    info = {}
    for path in input_files:
        info.update(data_pack.ReadGrdInfo(path))
    resources = {}
    for resource_id, payload in pak.resources.items():
        name = info[resource_id].textual_id
        if (not name.startswith('IDS_') or PROTECTED.search(name) or
                payload.startswith((b'\x1f\x8b', constants.BROTLI_CONST))):
            resources[resource_id] = payload
        else:
            resources[resource_id] = rebrand(payload.decode(encoding)).encode(encoding)
    data_pack.WriteDataPack(resources, output_file, pak.encoding)


original_repack = data_pack.RePack

if __name__ == '__main__':
    data_pack.RePack = repack
    pak_util.main()
