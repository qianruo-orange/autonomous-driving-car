#!/usr/bin/env python3
"""Historical repair must preserve every frame and never overwrite bad input."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/repair_detection_coordinates.py'
SPEC = importlib.util.spec_from_file_location('coordinates_repair', SCRIPT)
repair = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repair)


class CoordinatesRepairTests(unittest.TestCase):
    def test_per_frame_dimensions_coordinate_order_and_preserved_counts(self):
        raw = ('2026-10-27T15:01:02.003+0800：num[2] size[1280 720] FocalLength[raw:645]\r\n'
               '\tperson with spaces 0.750000 position[-4.5 1300 -0.000 720]\r\n'
               '\tcar 0.650000 position[10.000 20.000 30.000 40.000]\r\n'
               '2026-10-27T15:01:02.036+0800：num[1] size[641 479] FocalLength[unknown]\n'
               '\tcar 0.900000 position[-8 -1 20 30]\n'
               '2026-10-27T15:01:02.069+0800：num[0] size[641 479] FocalLength[unknown]\n').encode()
        output = list(repair.transform(raw.splitlines(keepends=True)))
        actual = b''.join(line for line, changed in output).decode()
        self.assertEqual(sum(changed for line, changed in output), 2)
        self.assertIn('position[0.000 1279.000 0.000 719.000]\r\n', actual)
        self.assertIn('\tcar 0.650000 position[10.000 20.000 30.000 40.000]\r\n', actual)
        self.assertIn('position[0.000 0.000 20.000 30.000]\n', actual)
        self.assertEqual(actual.count('：num['), 3)
        self.assertEqual(actual.count('\t'), 3)
        self.assertEqual(actual.splitlines()[0], raw.decode().splitlines()[0])

    def test_backup_and_idempotence(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / 'detections'
            day = root / '20261027'
            day.mkdir(parents=True)
            source = day / 'result_20261027.txt'
            original = ('time：num[1] size[1280 720] FocalLength[raw:645]\n'
                        '\tperson 0.750000 position[-5 1290 12 760]\n').encode()
            source.write_bytes(original)
            backup = Path(scratch) / 'backups'
            self.assertEqual(repair.repair(root, backup), 1)
            self.assertEqual(list(backup.iterdir())[0].read_bytes(), original)
            expected = source.read_bytes()
            before = source.stat()
            self.assertEqual(repair.repair(root, backup), 0)
            self.assertEqual(source.read_bytes(), expected)
            self.assertEqual(source.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(len(list(backup.iterdir())), 1)

    def test_bad_input_does_not_replace_or_backup(self):
        for body in ('\tperson .9 position[-5 10 2 4]\n',
                     'time：num[1] size[1280 720] FocalLength[raw:1]\n',
                     'time：num[1] size[1280 720] FocalLength[raw:1]\n\tperson .9 position[nan 10 2 4]\n'):
            with self.subTest(body=body), tempfile.TemporaryDirectory() as scratch:
                root = Path(scratch) / 'detections'
                day = root / '20261027'
                day.mkdir(parents=True)
                source = day / 'result_20261027.txt'
                source.write_bytes(body.encode())
                backup = Path(scratch) / 'backups'
                with self.assertRaises(ValueError):
                    repair.repair(root, backup)
                self.assertEqual(source.read_bytes(), body.encode())
                self.assertFalse(backup.exists())
                self.assertFalse(list(day.glob('*.tmp')))

    def test_backup_cannot_pollute_download_tree(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            with self.assertRaises(ValueError):
                repair.repair(root, root / 'backups')


if __name__ == '__main__':
    unittest.main()
