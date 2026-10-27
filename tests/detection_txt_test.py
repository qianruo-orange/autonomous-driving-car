#!/usr/bin/env python3
"""Check TXT sample formatting and one-time journal migration safety."""
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/migrate_detection_txt.py'
SPEC = importlib.util.spec_from_file_location('detection_txt', SCRIPT)
txt = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(txt)


class DetectionTxtTests(unittest.TestCase):
    @unittest.skipUnless(os.open in os.supports_dir_fd, 'POSIX directory descriptor safety')
    def test_legacy_public_txt_is_renamed_without_reexport(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            day = root / '20261027'
            day.mkdir()
            legacy = day / 'detections_20261027.txt'
            original = b'preserve exact native TXT history\n'
            legacy.write_bytes(original)
            self.assertEqual(txt.migrate(root), 1)
            self.assertFalse(legacy.exists())
            self.assertEqual((day / 'result_20261027.txt').read_bytes(), original)
            self.assertEqual(txt.migrate(root), 0)

    def row(self, seq=1, name='person', confidence='0.75', focus='645', mm='null', camera=None):
        fields = ['2026-10-27T15:01:02.003+0800', '1280', '720', name,
                  confidence, '1.25', '2.5', '100', '200', str(seq), focus, mm, 'true']
        if name == 'none':
            fields[4:9] = ['null'] * 5
        if camera is not None:
            fields.append(camera)
        return '\t'.join(fields) + '\n'

    def convert(self, rows):
        incoming = io.BytesIO(('\t'.join(txt.HEADER) + '\n' + ''.join(rows)).encode())
        return ''.join(part for metadata, objects in txt.frames(incoming)
                       for part in txt.frame_text(metadata, objects))

    def test_sample_groups_objects_and_coordinate_order(self):
        result = self.convert([self.row(), self.row(name='car', confidence='0.5'), self.row(seq=2, name='none')])
        self.assertEqual(result, '2026-10-27T15:01:02.003+0800：num[2] size[1280 720] FocalLength[raw:645]\n'
            '\tperson 0.750000 position[1.250 100.000 2.500 200.000]\n'
            '\tcar 0.500000 position[1.250 100.000 2.500 200.000]\n'
            '2026-10-27T15:01:02.003+0800：num[0] size[1280 720] FocalLength[raw:645]\n')

    def test_known_mm_and_unknown_are_honest(self):
        self.assertIn('FocalLength[3.6mm]', self.convert([self.row(mm='3.6')]))
        self.assertIn('FocalLength[unknown]', self.convert([self.row(focus='null')]))

    def test_mixed_camera_columns_and_control_escapes(self):
        result = self.convert([self.row(name=r'person\t\n\\'), self.row(seq=2, camera='UGREEN')])
        self.assertEqual(result.count('：num['), 2)
        self.assertIn(r'person\t\n\\', result)
        self.assertEqual(result.count('\n'), 4)

    def test_legacy_original_coordinates_are_clipped_without_rescaling(self):
        row = self.row().replace('\t1.25\t2.5\t100\t200\t', '\t-25\t-4\t1300\t800\t')
        result = self.convert([row])
        self.assertIn('position[0.000 1279.000 0.000 719.000]', result)

    def test_reject_bad_rows_without_silent_loss(self):
        for row in (self.row(confidence='nan'), self.row(focus='1.2'), self.row(mm='-1'),
                    self.row().rstrip('\n'), self.row().replace('1280', '0', 1)):
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.convert([row])

    @unittest.skipUnless(os.open in os.supports_dir_fd, 'POSIX directory descriptor safety')
    def test_atomic_idempotent_migration_and_clear(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            day = root / '20261027'
            day.mkdir()
            journal = day / '.detections_20261027.rows'
            journal.write_text('\t'.join(txt.HEADER) + '\n' + self.row(), encoding='utf-8')
            self.assertEqual(txt.migrate(root), 1)
            output = day / 'result_20261027.txt'
            expected = self.convert([self.row()])
            self.assertEqual(output.read_text(encoding='utf-8'), expected)
            self.assertTrue(journal.exists())
            output.write_text(expected + 'new native frame\n', encoding='utf-8')
            self.assertEqual(txt.migrate(root), 0)
            self.assertEqual(output.read_text(encoding='utf-8'), expected + 'new native frame\n')
            output.unlink()
            journal.unlink()  # Production clear/delete removes both.
            self.assertEqual(txt.migrate(root), 0)
            self.assertFalse(output.exists())

    @unittest.skipUnless(os.open in os.supports_dir_fd, 'POSIX directory descriptor safety')
    def test_partial_input_and_symlinks_do_not_publish(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            day = root / '20261027'
            day.mkdir()
            journal = day / '.detections_20261027.rows'
            journal.write_text('\t'.join(txt.HEADER) + '\n' + self.row() + 'partial', encoding='utf-8')
            with self.assertRaises(ValueError):
                txt.migrate(root)
            output = day / 'result_20261027.txt'
            self.assertFalse(output.exists())
            self.assertFalse(list(day.glob('*.tmp')))
            journal.unlink()
            journal.symlink_to(SCRIPT)
            with self.assertRaises(ValueError):
                txt.migrate(root)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
