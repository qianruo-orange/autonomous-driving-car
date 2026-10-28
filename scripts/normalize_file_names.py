#!/usr/bin/env python3
"""Normalize vehicle log/video names while stopped, preserving every file."""
import argparse
import datetime
import os
from pathlib import Path
import re
import stat

def new_name(name, day, info):
    log = re.fullmatch(r'(?:autonomous-driving-car|rockchip-smart-gateway)_(info|error)(?:_([0-9]{8}_[0-9]{6}_[0-9]{3}(?:_[0-9]+)?))?\.log(?:\.([0-9]+))?', name)
    if log:
        stamp = log[2] or datetime.datetime.fromtimestamp(info.st_mtime).strftime('%Y%m%d_%H%M%S_000')
        return 'log_' + log[1] + '_' + stamp + ('_' + log[3] if log[3] else '') + '.log'
    video = re.fullmatch(r'rec_([0-9]{8}_[0-9]{6}_[0-9]{3}(?:_[0-9]+)?)\.mp4', name)
    if video:
        return 'video_' + video[1] + '.mp4'
    video = re.fullmatch(r'rec_([0-9]{6})(?:_([0-9]+))?\.mp4', name)
    if video:
        return 'video_' + day + '_' + video[1] + '_000' + ('_' + video[2] if video[2] else '') + '.mp4'
    return None

def normalize(root):
    root = Path(root).absolute()
    for component in (root, *root.parents):
        if component.is_symlink():
            raise ValueError('Symlink roots are not accepted')
    if not root.exists():
        return 0
    count = 0
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for day in sorted(os.listdir(descriptor)):
            if not re.fullmatch(r'[0-9]{8}', day):
                continue
            if not stat.S_ISDIR(os.stat(day, dir_fd=descriptor, follow_symlinks=False).st_mode):
                continue
            directory = os.open(day, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
            try:
                for old in sorted(os.listdir(directory)):
                    info = os.stat(old, dir_fd=directory, follow_symlinks=False)
                    if not stat.S_ISREG(info.st_mode):
                        continue
                    name = new_name(old, day, info)
                    if not name:
                        continue
                    stem, extension = os.path.splitext(name)
                    for index in range(10000):
                        candidate = name if not index else f'{stem}_{index}{extension}'
                        try:
                            os.link(old, candidate, src_dir_fd=directory, dst_dir_fd=directory, follow_symlinks=False)
                        except FileExistsError:
                            other = os.stat(candidate, dir_fd=directory, follow_symlinks=False)
                            if (other.st_dev, other.st_ino) != (info.st_dev, info.st_ino):
                                continue
                        os.unlink(old, dir_fd=directory)
                        count += 1
                        break
                    else:
                        raise ValueError('Too many filename collisions')
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        os.close(descriptor)
    return count

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, help='Deployed vehicle directory; stop service first')
    args = parser.parse_args()
    root = Path(args.root).absolute()
    logs = 'logs'
    config = root / 'config/config.txt'
    if config.exists():
        for line in config.read_text(encoding='utf-8').splitlines():
            match = re.match(r'^\s*log_dir\s*=\s*([^#\r\n]+)', line)
            if match:
                logs = match[1].strip()
    path = Path(logs)
    log_root = path if path.is_absolute() else root / path
    print('Normalized %d log/video filenames' % (normalize(log_root) + normalize(root / 'recordings')))

if __name__ == '__main__':
    main()
