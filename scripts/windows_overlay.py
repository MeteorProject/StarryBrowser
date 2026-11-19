#!/usr/bin/env python3
"""Stage the Windows build's //starry inputs as regular files inside Chromium."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile


def is_link(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_hashes(root):
    result = {}
    def on_error(error):
        raise error
    for directory, dirs, files in os.walk(root, onerror=on_error):
        for name in dirs + files:
            path = Path(directory, name)
            if is_link(path):
                raise RuntimeError(f'Unexpected link inside the Starry source tree: {path}')
        for name in files:
            path = Path(directory, name)
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def sync_overlay(source, destination):
    source = source.resolve(strict=True)
    destination = destination.parent.resolve(strict=True) / destination.name
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError('The overlay source and build directory must be separate.')
    branding = Path('app/theme/BRANDING')
    if not (source / branding).is_file():
        raise RuntimeError(f'Missing Starry source file: {source / branding}')

    state = destination.parent.parent / 'starry-overlay-hashes.json'
    source_hashes = file_hashes(source)
    if os.path.lexists(destination) and is_link(destination):
        try:
            matches = destination.samefile(source)
        except FileNotFoundError:
            matches = True  # A broken link has no target contents to preserve.
        if not matches:
            raise RuntimeError(f'Unexpected target of {destination}: {os.readlink(destination)}')
        # Remove only the old directory entry after its replacement is ready.
        # In particular, never pass a directory junction to a recursive removal.
        with tempfile.TemporaryDirectory(dir=destination.parent) as temp:
            ready = Path(temp, 'starry')
            shutil.copytree(source, ready, copy_function=shutil.copy)
            os.utime(ready, None)
            if destination.is_symlink():
                destination.unlink()
            else:
                destination.rmdir()
            ready.rename(destination)
        print('Replaced the Starry directory link with regular build inputs.', flush=True)
    destination.mkdir(exist_ok=True)

    previous = json.loads(state.read_text(encoding='utf-8')) if state.exists() else {}
    current = file_hashes(destination)
    updates, removals = [], []
    for name in sorted(source_hashes.keys() | previous.keys()):
        if '..' in Path(name).parts or Path(name).anchor:
            raise ValueError(f'Invalid overlay state path: {name}')
        wanted, before, actual = source_hashes.get(name), previous.get(name), current.get(name)
        if actual == wanted:
            continue
        if wanted is not None and actual is None:
            updates.append(name)
        elif before == wanted:
            continue  # Preserve a change the developer made only in the build tree.
        elif actual == before:
            (removals if wanted is None else updates).append(name)
        elif before is None or wanted is None:
            continue  # Preserve a pre-existing or locally modified build file.
        else:
            raise RuntimeError(f'Both Starry copies changed: {source / name} and {destination / name}')
    for name in removals:
        removed = destination / name
        removed.unlink()
        for parent in removed.parents:
            if parent == destination:
                break
            try:
                parent.rmdir()
            except OSError:
                break
    for name in updates:
        src, dst = source / name, destination / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=dst.parent, delete=False) as tmp:
            pending = Path(tmp.name)
        try:
            shutil.copy(src, pending)
            pending.replace(dst)
        finally:
            pending.unlink(missing_ok=True)

    if not (destination / branding).is_file() or is_link(destination):
        raise RuntimeError(f'Expected a regular build directory containing {destination / branding}')
    if previous != source_hashes:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=state.parent,
                                         delete=False) as temp:
            json.dump(source_hashes, temp, sort_keys=True)
        Path(temp.name).replace(state)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    sync_overlay(args.source.absolute(), args.destination.absolute())
