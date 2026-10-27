#!/usr/bin/env python3
"""Convert retained daily TSV journals to frame-grouped TXT while service is stopped.

An existing public TXT is never overwritten or appended again. The journal is
retained as historical source until explicit deletion or clearing removes it.
Neither old photos, JSON snapshots nor backup directories are inspected.
"""
import argparse
import datetime
import math
import os
from pathlib import Path
import re
import stat

HEADER = ('capture_time', 'width', 'height', 'target', 'confidence', 'x1', 'y1',
          'x2', 'y2', 'frame_seq', 'focus_position_raw', 'focal_length_mm', 'autofocus')
MAX_LINE_BYTES = 131072
MAX_FRAME_OBJECTS = 65536


def number(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError('Non-finite journal number')
    return result


def parse(line):
    fields = line.rstrip('\r\n').split('\t')
    if len(fields) not in (13, 14):
        raise ValueError('Expected 13 or 14 journal columns')
    datetime.datetime.fromisoformat(fields[0])
    if int(fields[1]) <= 0 or int(fields[2]) <= 0:
        raise ValueError('Invalid frame dimensions')
    int(fields[9])
    if fields[10] not in ('', 'null'):
        int(fields[10])
    if fields[11] not in ('', 'null'):
        if number(fields[11]) <= 0:
            raise ValueError('Invalid focal length')
    if fields[12] not in ('true', 'false', 'null', ''):
        raise ValueError('Invalid autofocus')
    no_targets = fields[3] == 'none' and all(value in ('null', '') for value in fields[4:9])
    if not no_targets:
        for value in fields[4:9]:
            number(value)
    return fields, no_targets


def frames(stream):
    header = stream.readline(MAX_LINE_BYTES + 1).decode('utf-8').rstrip('\r\n').split('\t')
    if tuple(header) not in (HEADER, HEADER + ('camera',)):
        raise ValueError('Unsupported journal header')
    key, metadata, objects = None, None, []
    for line in iter(lambda: stream.readline(MAX_LINE_BYTES + 1), b''):
        if len(line) > MAX_LINE_BYTES or not line.endswith(b'\n'):
            raise ValueError('Incomplete or oversized journal row')
        fields, no_targets = parse(line.decode('utf-8'))
        current = tuple(fields[index] for index in (0, 9, 1, 2))
        if key is not None and current != key:
            yield metadata, objects
            metadata, objects = None, []
        if metadata is None:
            metadata, key = fields, current
        elif any(fields[index] != metadata[index] for index in (10, 11, 12)):
            raise ValueError('Inconsistent focus metadata within one frame')
        if not no_targets:
            if len(objects) == MAX_FRAME_OBJECTS:
                raise ValueError('Too many objects in one frame')
            objects.append(fields)
    if metadata is not None:
        yield metadata, objects


def frame_text(metadata, objects):
    focus = ('%gmm' % number(metadata[11]) if metadata[11] not in ('null', '') else
             'raw:' + str(int(metadata[10])) if metadata[10] not in ('null', '') else 'unknown')
    yield f'{metadata[0]}：num[{len(objects)}] size[{int(metadata[1])} {int(metadata[2])}] FocalLength[{focus}]\n'
    for fields in objects:
        # The journal already escapes backslashes and control characters in
        # target names. Keep these escapes so one target always occupies one line.
        # Retained journals are already in original-image coordinates. Old
        # postprocessing omitted image-boundary clipping; never scale twice.
        bounds = (int(metadata[1]) - 1,) * 2 + (int(metadata[2]) - 1,) * 2
        values = [number(fields[index]) for index in (5, 7, 6, 8)]
        coordinates = [0.0 if value <= 0 else min(value, bound)
                       for value, bound in zip(values, bounds)]
        yield ('\t%s %.6f position[%.3f %.3f %.3f %.3f]\n' %
               (fields[3], number(fields[4]), *coordinates))


def regular_at(directory, name):
    try:
        info = os.stat(name, dir_fd=directory, follow_symlinks=False)
    except FileNotFoundError:
        return False
    if not stat.S_ISREG(info.st_mode):
        raise ValueError('Detection file is not regular: ' + name)
    return True


def migrate_day(directory, day):
    name, journal = f'result_{day}.txt', f'.detections_{day}.rows'
    legacy = f'detections_{day}.txt'
    if regular_at(directory, name):
        return False
    if regular_at(directory, legacy):
        os.link(legacy, name, src_dir_fd=directory, dst_dir_fd=directory, follow_symlinks=False)
        os.unlink(legacy, dir_fd=directory)
        os.fsync(directory)
        return True
    # This prevents a later deployment from replacing newer native TXT frames
    # with the older retained journal. Clear/delete removes both files together.
    if regular_at(directory, name) or not regular_at(directory, journal):
        return False
    source = os.open(journal, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory)
    temporary = f'.detections_{day}.migrate_{os.getpid()}_{os.urandom(6).hex()}.tmp'
    target = None
    try:
        target = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                         0o644, dir_fd=directory)
        incoming = os.fdopen(source, 'rb')
        source = None
        try:
            outgoing = os.fdopen(target, 'w', encoding='utf-8', newline='')
        except BaseException:
            incoming.close()
            raise
        target = None
        with incoming, outgoing:
            for metadata, objects in frames(incoming):
                outgoing.writelines(frame_text(metadata, objects))
            outgoing.flush()
            os.fsync(outgoing.fileno())
        # link() publishes without replacing a TXT created concurrently. The
        # supported caller stops the service before invoking migration.
        os.link(temporary, name, src_dir_fd=directory, dst_dir_fd=directory, follow_symlinks=False)
        os.fsync(directory)
        return True
    finally:
        if source is not None:
            os.close(source)
        if target is not None:
            os.close(target)
        try:
            os.unlink(temporary, dir_fd=directory)
        except FileNotFoundError:
            pass


def migrate(root):
    root = Path(root).absolute()
    for component in (root, *root.parents):
        if component.is_symlink():
            raise ValueError('Symlink root paths are not accepted')
    if not root.exists():
        return 0
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    converted = 0
    try:
        for day in sorted(os.listdir(descriptor)):
            if not re.fullmatch(r'[0-9]{8}', day):
                continue
            if not stat.S_ISDIR(os.stat(day, dir_fd=descriptor, follow_symlinks=False).st_mode):
                continue
            directory = os.open(day, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
            try:
                converted += migrate_day(directory, day)
            finally:
                os.close(directory)
    finally:
        os.close(descriptor)
    return converted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, help='Detection directory; stop the service first')
    arguments = parser.parse_args()
    print('Migrated %d daily detection TXT files' % migrate(arguments.root))


if __name__ == '__main__':
    main()
