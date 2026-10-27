#!/usr/bin/env python3
"""Clip historical native TXT boxes to each frame's original image dimensions.

Stop the gateway service first. Existing coordinates are already inverse-mapped
from letterbox space: do not scale them again. Preserve every frame and object,
including boxes which become zero-area at an image boundary. Only changed TXT
lines are replaced. Keep an exact backup outside the downloadable report tree.
"""
import argparse
import math
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile

HEADER = re.compile(r'^.*：num\[(\d+)\] size\[(\d+) (\d+)\] FocalLength\[[^\r\n]*\]\r?\n?$')
OBJECT = re.compile(r'^(\t.* position\[)([^\s]+) ([^\s]+) ([^\s]+) ([^\s]+)(\]\r?\n?)$')


def transform(lines):
    """Yield (bytes, changed) while retaining the original frame/object count."""
    width = height = expected = seen = None
    for raw in lines:
        text = raw.decode('utf-8')
        header = HEADER.fullmatch(text)
        if header:
            if expected is not None and seen != expected:
                raise ValueError('Frame target count does not match its object lines')
            expected, width, height = map(int, header.groups())
            if width <= 0 or height <= 0:
                raise ValueError('Invalid original image size')
            seen = 0
            yield raw, False
            continue
        match = OBJECT.fullmatch(text)
        if not match or expected is None:
            raise ValueError('Unrecognized TXT line or object before frame header')
        values = [float(value) for value in match.groups()[1:5]]
        if not all(math.isfinite(value) for value in values):
            raise ValueError('Non-finite historical coordinate')
        bounds = (width - 1, width - 1, height - 1, height - 1)
        clipped = [0.0 if value <= 0 else min(value, bound) for value, bound in zip(values, bounds)]
        changed = clipped != values or any(value == 0 and token.startswith('-')
                                           for value, token in zip(values, match.groups()[1:5]))
        if changed:
            text = match[1] + ' '.join('%.3f' % value for value in clipped) + match[6]
            raw = text.encode('utf-8')
        seen += 1
        yield raw, changed
    if expected is not None and seen != expected:
        raise ValueError('Incomplete final frame')


def safe_directory(path):
    path = Path(path).absolute()
    for component in (path, *path.parents):
        if component.is_symlink():
            raise ValueError('Symlink path is not accepted: ' + str(component))
    return path


def signature(info):
    # Windows reports creation/change time differently for pathname and handle
    # queries after replace(); POSIX ctime is a useful extra mutation guard.
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns,
            info.st_ctime_ns if os.name == 'posix' else 0)


def repair_file(source, backup_root):
    info = source.lstat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError('Detection TXT must be a regular file')
    descriptor, temporary = tempfile.mkstemp(prefix='.coordinate-repair-', suffix='.tmp', dir=source.parent)
    changes = 0
    try:
        with source.open('rb') as incoming, os.fdopen(descriptor, 'wb') as outgoing:
            if signature(os.fstat(incoming.fileno())) != signature(info):
                raise ValueError('Detection TXT changed before repair')
            for raw, changed in transform(incoming):
                outgoing.write(raw)
                changes += changed
            outgoing.flush()
            os.fsync(outgoing.fileno())
        if not changes:
            return 0
        if signature(source.lstat()) != signature(info):
            raise ValueError('Detection TXT changed during repair; stop the service first')
        backup_root.mkdir(parents=True, exist_ok=True)
        # Unique backups are never overwritten on repeat executions.
        backup_descriptor, backup = tempfile.mkstemp(prefix=source.stem + '.before-coordinate-clipping.',
                                                    suffix='.txt', dir=backup_root)
        with source.open('rb') as incoming, os.fdopen(backup_descriptor, 'wb') as outgoing:
            shutil.copyfileobj(incoming, outgoing, length=1024 * 1024)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        if signature(source.lstat()) != signature(info):
            raise ValueError('Detection TXT changed during backup; original remains untouched')
        os.chmod(temporary, stat.S_IMODE(info.st_mode))
        os.utime(temporary, ns=(info.st_atime_ns, info.st_mtime_ns))
        os.replace(temporary, source)
        print(f'Repaired {changes} object lines: {source.name}; backup: {backup}')
        return changes
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def repair(root, backup_root):
    root, backup_root = safe_directory(root), safe_directory(backup_root)
    if root == backup_root or root in backup_root.parents:
        raise ValueError('Backups must be outside the downloadable detection tree')
    changed = 0
    for day in sorted(root.iterdir()):
        if not re.fullmatch(r'[0-9]{8}', day.name) or day.is_symlink() or not day.is_dir():
            continue
        source = day / f'detections_{day.name}.txt'
        if source.exists() or source.is_symlink():
            changed += repair_file(source, backup_root)
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--backup-root', required=True)
    parser.add_argument('--service-stopped', action='store_true', required=True,
                        help='Confirm the gateway service was stopped before running this tool')
    arguments = parser.parse_args()
    print('Corrected %d historical object lines' % repair(arguments.root, arguments.backup_root))


if __name__ == '__main__':
    main()
