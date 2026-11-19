# Copyright 2026 The Starry Browser Authors

"""Prepare reference outputs for Chromium's branded Windows service IDLs."""

from pathlib import Path
import re
import shutil
import sys
import uuid


def order_proxy_tables(proxy, header, substitutions):
    # MIDL's IID lookup compares the in-memory bytes of Windows GUIDs.
    keys = {}
    for guid, name in re.findall(rb'MIDL_INTERFACE\("([\w-]+)"\)\s+(\w+) :',
                                 header):
        guid = guid.decode('ascii').upper()
        keys[name] = uuid.UUID(substitutions.get(guid, guid)).bytes_le

    table = re.compile(rb'_(ProxyVtblList|StubVtblList|InterfaceNamesList|BaseIIDList)'
                       rb'\[\] =\s*\{\r?\n(?P<rows>.*?)    0\r?\n\};', re.S)
    tables = {m[1]: m for m in table.finditer(proxy)}
    if set(tables) != {b'ProxyVtblList', b'StubVtblList', b'InterfaceNamesList',
                       b'BaseIIDList'}:
        raise ValueError('Unrecognized MIDL proxy tables')
    names = re.findall(rb'^    "(\w+)",\r?$', tables[b'InterfaceNamesList']['rows'], re.M)
    order = sorted(range(len(names)), key=lambda i: keys[names[i]])
    for match in reversed(list(tables.values())):
        rows = match['rows'].splitlines(keepends=True)
        if not names or len(rows) != len(names):
            raise ValueError(f'Unexpected number of rows in {match[1]!r}')
        start, end = match.span('rows')
        proxy = proxy[:start] + b''.join(rows[i] for i in order) + proxy[end:]
    return proxy


def prepare_reference(source, output, generated_idl, dynamic_guids):
    name = Path(generated_idl).stem
    substitutions = dict(pair.split('=') for pair in dynamic_guids.upper().split(','))
    output.mkdir(parents=True, exist_ok=True)
    header = (source / (name + '.h')).read_bytes()
    heading = re.compile(rb'^/\* Compiler settings for [^\r\n]*?/' +
                         re.escape((name + '.idl').encode()) + rb':', re.M)
    for suffix in ('.h', '_i.c', '_p.c'):
        filename = name + suffix
        contents = (source / filename).read_bytes()
        if suffix == '_p.c':
            contents = order_proxy_tables(contents, header, substitutions)
        contents, count = heading.subn(
            lambda _: b'/* Compiler settings for ' + generated_idl.encode() + b':',
            contents)
        if count != 1:
            raise ValueError(f'Expected one MIDL compiler settings header in {filename}')
        (output / filename).write_bytes(contents)
    for suffix in ('.tlb', '.dlldata.c'):
        shutil.copyfile(source / (name + suffix), output / (name + suffix))


if __name__ == '__main__':
    prepare_reference(Path(sys.argv[1]), Path(sys.argv[2]), *sys.argv[3:])
