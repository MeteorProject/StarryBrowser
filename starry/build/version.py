#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors

"""Parse Starry's SemVer version file and generate build-time version metadata."""

from __future__ import annotations

import argparse
import dataclasses
import json
from pathlib import Path
import plistlib
import re

# Official SemVer 2.0.0 grammar.
SEMVER_RE = re.compile(
    r'^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)'
    r'(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)'
    r'(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?'
    r'(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$'
)


@dataclasses.dataclass(frozen=True)
class StarryVersion:
    full: str
    major: int
    minor: int
    patch: int
    prerelease: str
    build: str

    @property
    def core(self) -> str:
        return f'{self.major}.{self.minor}.{self.patch}'

    @property
    def mac_bundle_version(self) -> str:
        # Apple requires CFBundleVersion to contain 1-3 period-separated
        # non-negative integers (no '-prerelease' or '+build').
        return self.core

    @property
    def windows_version(self) -> str:
        # Windows PE VS_VERSION_INFO and SxS assembly identities require four
        # 16-bit unsigned integers (0..65535).
        for part_name, part in (
            ('MAJOR', self.major),
            ('MINOR', self.minor),
            ('PATCH', self.patch),
        ):
            if part > 65535:
                raise ValueError(
                    f'{part_name} ({part}) exceeds Windows 16-bit version limit'
                )
        return f'{self.major}.{self.minor}.{self.patch}.0'

    @property
    def linux_package_version(self) -> str:
        # Debian and RPM sort '~' before the final release and forbid '-' in
        # RPM's Version field, so map SemVer '-pre' to '~pre' (with inner '-'
        # normalized to '.') and drop '+build' metadata.
        if not self.prerelease:
            return self.core
        return f"{self.core}~{self.prerelease.replace('-', '.')}"


def parse_version(raw: str) -> StarryVersion:
    text = raw.strip()
    match = SEMVER_RE.fullmatch(text)
    if not match:
        raise ValueError(f'Invalid SemVer version in starry/VERSION: {text!r}')
    return StarryVersion(
        full=text,
        major=int(match.group(1)),
        minor=int(match.group(2)),
        patch=int(match.group(3)),
        prerelease=match.group(4) or '',
        build=match.group(5) or '',
    )


def read_version(path: str | Path) -> StarryVersion:
    return parse_version(Path(path).read_text(encoding='utf-8'))


def header(version: StarryVersion) -> str:
    lines = [
        '// Generated from //starry/VERSION. Do not edit.',
        '#ifndef STARRY_COMMON_VERSION_VALUES_H_',
        '#define STARRY_COMMON_VERSION_VALUES_H_',
        '',
        f'#define STARRY_VERSION_STRING {json.dumps(version.full)}',
        f'#define STARRY_VERSION_MAJOR {version.major}',
        f'#define STARRY_VERSION_MINOR {version.minor}',
        f'#define STARRY_VERSION_PATCH {version.patch}',
        f'#define STARRY_VERSION_PRERELEASE {json.dumps(version.prerelease)}',
        f'#define STARRY_VERSION_BUILD {json.dumps(version.build)}',
        f'#define STARRY_WINDOWS_VERSION_STRING {json.dumps(version.windows_version)}',
        '',
        '#endif  // STARRY_COMMON_VERSION_VALUES_H_',
        '',
    ]
    return '\n'.join(lines)


def win_version_file(version: StarryVersion) -> str:
    # Validate 16-bit bounds via windows_version before emitting.
    _ = version.windows_version
    return (
        f'MAJOR={version.major}\n'
        f'MINOR={version.minor}\n'
        f'BUILD={version.patch}\n'
        'PATCH=0\n'
    )


def rewrite_mac_plist(version: StarryVersion, input_path: str | Path) -> bytes:
    raw = Path(input_path).read_bytes()
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    plist = plistlib.loads(raw)
    plist['CFBundleShortVersionString'] = version.full
    plist['CFBundleVersion'] = version.mac_bundle_version
    return plistlib.dumps(plist, fmt=fmt)


def write_if_changed(path: str | Path, contents: str | bytes) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = contents.encode('utf-8') if isinstance(contents, str) else contents
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version-file', required=True)
    parser.add_argument(
        '--format',
        choices=('header', 'win-version-file', 'mac-plist'),
        required=True,
    )
    parser.add_argument('--input')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    version = read_version(args.version_file)
    if args.format == 'header':
        write_if_changed(args.output, header(version))
    elif args.format == 'win-version-file':
        write_if_changed(args.output, win_version_file(version))
    else:
        if not args.input:
            parser.error('--input is required for --format=mac-plist')
        write_if_changed(args.output, rewrite_mac_plist(version, args.input))


if __name__ == '__main__':
    main()
