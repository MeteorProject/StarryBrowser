#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors

"""Generate native Windows installer strings with Starry product names."""

import re
import sys

import repack_locales as branding

sys.path.insert(0, str(branding.SRC / 'base/i18n/win/embedded_i18n'))
import create_string_rc

original_write_rc = create_string_rc.StringRcMaker._StringRcMaker__WriteRCFile

# Native installer messages contain inflected product names and localized
# hyphenated phrases. These are prose, unlike protocol and API identifiers.
NATIVE_CHROMIUM = re.compile(
    r'(?<![A-Za-z0-9_./:@])Chromium(?!\.[A-Za-z0-9_])'
    r'(?=(?:a|i|in|ot|u|ile|hoz|is|s|ista)?\b)')


def rebrand_installer(text):
    return NATIVE_CHROMIUM.sub(branding.PRODUCT_NAME, branding.rebrand(text))


def write_rc(self, translations):
    # Rebrand after matching the source translation IDs, before RC escaping.
    for entry in translations:
        if not branding.PROTECTED.search(entry.resource_id_str):
            entry.translation = rebrand_installer(entry.translation)
    original_write_rc(self, translations)


create_string_rc.StringRcMaker._StringRcMaker__WriteRCFile = write_rc

if __name__ == '__main__':
    sys.exit(create_string_rc.main())
