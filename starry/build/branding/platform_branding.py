#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors

"""Generate persistent platform identifiers and Linux package metadata."""

import argparse
import json
from pathlib import Path
import re
import shlex
import uuid


def read_branding(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        if line and not line.startswith('#'):
            key, value = line.split('=', 1)
            values[key] = value
    if not re.fullmatch(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*', values['PRODUCT_ID']):
        raise ValueError('PRODUCT_ID must be a lowercase package and URI identifier')
    if not re.fullmatch(r'[a-z][a-z0-9.-]+', values['APPLICATION_ID']):
        raise ValueError('APPLICATION_ID must be a reverse-DNS identifier')
    if any(c in values['PRODUCT_FULLNAME'] for c in '/\\\r\n\0'):
        raise ValueError('PRODUCT_FULLNAME must be a single directory name')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]{0,7}', values['WINDOWS_PROG_ID_PREFIX']):
        raise ValueError('WINDOWS_PROG_ID_PREFIX must be at most eight alphanumeric characters')
    guids = [uuid.UUID(value) for key, value in values.items() if is_guid(key)]
    if len(guids) != len(set(guids)):
        raise ValueError('Windows registration GUIDs must be distinct')
    return values


def is_guid(key):
    return key.startswith('WINDOWS_') and key.endswith(('_GUID', '_CLSID', '_IID'))


def guid_initializer(value):
    fields = uuid.UUID(value).fields
    tail = uuid.UUID(value).bytes[8:]
    return ('{0x%08x, 0x%04x, 0x%04x, {%s}}' %
            (fields[0], fields[1], fields[2],
             ', '.join('0x%02x' % byte for byte in tail)))


def header(values):
    product_id = values['PRODUCT_ID']
    app_id = ''.join(part.capitalize() for part in product_id.split('-'))
    strings = {
        'PRODUCT_ID': product_id,
        'APPLICATION_ID': values['APPLICATION_ID'],
        'LINUX_POLICY_PATH': f'/etc/{product_id}/policies',
        'LINUX_NATIVE_MESSAGING_PATH': f'/etc/{product_id}/native-messaging-hosts',
        'LINUX_EXTERNAL_EXTENSIONS_PATH': f'/usr/share/{product_id}/extensions',
        'LINUX_DESKTOP_FILE': f'{product_id}.desktop',
    }
    wide_strings = {
        'PRODUCT_FULLNAME_W': values['PRODUCT_FULLNAME'],
        'WINDOWS_APP_ID': app_id,
        'WINDOWS_EXE': product_id + '.exe',
        'WINDOWS_DLL': product_id + '.dll',
        'WINDOWS_VISUAL_ELEMENTS_MANIFEST': product_id + '.VisualElementsManifest.xml',
        'WINDOWS_NATIVE_MESSAGING_KEY': (
            'SOFTWARE\\' + values['PRODUCT_FULLNAME'] + '\\NativeMessagingHosts'),
        'WINDOWS_INSTALLER_LOG': product_id + '-installer.log',
        'WINDOWS_HTML_PROG_ID': values['WINDOWS_PROG_ID_PREFIX'] + 'HTM',
        'WINDOWS_PDF_PROG_ID': values['WINDOWS_PROG_ID_PREFIX'] + 'PDF',
        'WINDOWS_HTML_DESCRIPTION': values['PRODUCT_FULLNAME'] + ' HTML Document',
        'WINDOWS_PDF_DESCRIPTION': values['PRODUCT_FULLNAME'] + ' PDF Document',
        'WINDOWS_ACTIVE_SETUP_GUID': '{' + values['WINDOWS_ACTIVE_SETUP_GUID'] + '}',
    }
    if len(wide_strings['WINDOWS_HTML_PROG_ID']) > 11:
        raise ValueError('Windows ProgID prefix leaves insufficient room for per-user suffix')
    lines = ['// Generated from BRANDING. Do not edit.',
             '#ifndef STARRY_COMMON_PLATFORM_BRANDING_H_',
             '#define STARRY_COMMON_PLATFORM_BRANDING_H_', '']
    for key, value in strings.items():
        lines.append(f'#define STARRY_{key} {json.dumps(value)}')
    for key, value in wide_strings.items():
        lines.append(f'#define STARRY_{key} L{json.dumps(value)}')
    for key, value in values.items():
        if is_guid(key) and key != 'WINDOWS_ACTIVE_SETUP_GUID':
            lines.append(f'#define STARRY_{key} {guid_initializer(value)}')
    lines += ['', '#endif  // STARRY_COMMON_PLATFORM_BRANDING_H_', '']
    return '\n'.join(lines)


NO_REPOSITORY = '''# Starry does not have an update repository yet.
# Do not install upstream repository configuration or signing keys.
install_key() { :; }
uninstall_key() { :; }
install_deb822_sources() { :; }
remove_legacy_list() { :; }
remove_legacy_key() { :; }
clean_sources_lists() { :; }
install_rpm_key() { :; }
determine_rpm_package_manager() { PACKAGEMANAGERS=""; }
install_yum() { :; }
install_zypp() { :; }
'''


def linux_files(values):
    product_id = values['PRODUCT_ID']
    info = {
        'PACKAGE': product_id,
        # This is the internal Chromium build target; the public launcher uses PACKAGE.
        'PROGNAME': 'chrome',
        'INSTALLDIR': f'/opt/{product_id}',
        'ENROLLMENTDIR': f'/etc/{product_id}/policies/enrollment',
        'MENUNAME': values['PRODUCT_FULLNAME'],
        'SHORTDESC': values['PRODUCT_FULLNAME'],
        'FULLDESC': values['PRODUCT_FULLNAME'],
        'MAINTNAME': values['COMPANY_FULLNAME'],
        'MAINTMAIL': values['MAINTAINER_EMAIL'],
        'PRODUCTURL': values['PRODUCT_URL'],
        'PROJECT_LICENSE': ('BSD-3-Clause and LGPL-2.1+ and Apache-2.0 and IJG and '
                            'MIT and GPL-2.0+ and ISC and OpenSSL and '
                            '(MPL-1.1 or GPL-2.0 or LGPL-2.0)'),
        'DEVELOPER_NAME': values['COMPANY_FULLNAME'],
        'BUGTRACKERURL': values['BUGTRACKER_URL'],
        'HELPURL': values['HELP_URL'],
        'RDN': values['APPLICATION_ID'],
    }
    # Keep the input filename expected by the upstream packaging API.
    files = {'chromium-browser.info': '\n'.join(
        f'{key}={shlex.quote(value)}' for key, value in info.items()) + '\n'}
    for filename in ('apt.include', 'rpm.include'):
        files[filename] = NO_REPOSITORY
    files['key.include'] = '# Starry has no repository signing key configured.\n'
    for filename in ('repo.cron', 'rpmrepo.cron'):
        files[filename] = '#!/bin/sh\n# No Starry update repository configured.\nexit 0\n'
    files['appdata.xml.template'] = '''<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop">
  <id>@@PACKAGE.desktop</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>@@PROJECT_LICENSE</project_license>
  <name>@@MENUNAME</name>
  <summary>@@SHORTDESC</summary>
  <description><p>@@FULLDESC</p></description>
  <url type="homepage">@@PRODUCTURL</url>
  <developer_name>@@DEVELOPER_NAME</developer_name>
  <url type="bugtracker">@@BUGTRACKERURL</url>
  <url type="help">@@HELPURL</url>
</component>
'''
    return files


def write_if_changed(path, contents):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != contents:
        path.write_text(contents)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--branding', required=True)
    parser.add_argument('--format', choices=('header', 'linux'), required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    values = read_branding(args.branding)
    if args.format == 'header':
        write_if_changed(args.output, header(values))
    else:
        for name, contents in linux_files(values).items():
            write_if_changed(Path(args.output) / name, contents)


if __name__ == '__main__':
    main()
