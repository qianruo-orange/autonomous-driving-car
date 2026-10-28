#!/usr/bin/env python3
"""Validate the production standard-library detection workbook exporter."""
import importlib.util
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from xml.etree import ElementTree as ET
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/export_detection_excel.py'
SPEC = importlib.util.spec_from_file_location('detection_excel', SCRIPT)
excel = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(excel)
NS = {'m':excel.MAIN_NS, 'r':excel.PACKAGE_REL_NS}


class DetectionExcelTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='car-excel-test-')
        self.root = Path(self.directory.name)
        self.source = self.root / 'daily.rows'
        self.output = self.root / 'detections_20261027.xlsx'

    def tearDown(self):
        self.directory.cleanup()

    def row(self, sequence=1, name='person', camera=None):
        fields = ['2026-10-27T15:01:02.003+0800', '1280', '720', name,
            '0.750000', '1.250', '2.500', '100.000', '200.000', str(sequence), '645', 'null', 'true']
        if camera is not None:
            fields.append(camera)
        return '\t'.join(fields) + '\n'

    def journal(self, rows, camera=False):
        self.source.write_text('\t'.join(excel.CAMERA_HEADER if camera else excel.HEADER) + '\n' + ''.join(rows), encoding='utf-8', newline='')

    def workbook_rows(self):
        with zipfile.ZipFile(self.output) as archive:
            self.assertIsNone(archive.testzip())
            workbook = ET.fromstring(archive.read('xl/workbook.xml'))
            relationships = ET.fromstring(archive.read('xl/_rels/workbook.xml.rels'))
            targets = {item.attrib['Id']:item.attrib['Target'] for item in relationships}
            result = []
            for sheet in workbook.find('m:sheets', NS):
                relationship = sheet.attrib['{' + excel.REL_NS + '}id']
                sheet_xml = ET.fromstring(archive.read('xl/' + targets[relationship]))
                result.append(sheet_xml.findall('m:sheetData/m:row', NS))
            self.assertIn('[Content_Types].xml', archive.namelist())
            self.assertIn('_rels/.rels', archive.namelist())
            self.assertIn('xl/styles.xml', archive.namelist())
            for name in archive.namelist():
                if name.endswith(('.xml', '.rels')):
                    ET.fromstring(archive.read(name))
            return result

    def test_standard_package_types_and_escaped_target(self):
        self.journal([self.row(name='=SUM(A1) & <bird> "quoted" _x000A_\x01')])
        result = excel.export_workbook(self.source, self.output)
        self.assertEqual((result['rows'], result['sheets']), (1, 1))
        rows = self.workbook_rows()[0]
        self.assertEqual((len(rows), result['frames']), (3, 1))
        summary = {cell.attrib['r']:cell for cell in rows[0]}
        cells = {cell.attrib['r']:cell for cell in rows[2]}
        self.assertEqual(cells['A3'].attrib['t'], 'inlineStr')
        self.assertEqual(cells['A3'].find('m:is/m:t', NS).text, '=SUM(A1) & <bird> "quoted" _x005F_x000A_\uFFFD')
        self.assertEqual(cells['B3'].find('m:v', NS).text, '0.750000')
        self.assertEqual(summary['B1'].find('m:v', NS).text, '1')
        self.assertEqual(summary['D1'].find('m:is/m:t', NS).text, '1280 × 720')
        self.assertEqual(summary['F1'].find('m:is/m:t', NS).text, excel.UNKNOWN_CAMERA)
        self.assertIsNone(summary['N1'].find('m:v', NS))
        self.assertEqual(summary['P1'].attrib['t'], 'b')
        self.assertEqual(summary['P1'].find('m:v', NS).text, '1')

    def test_new_sheets_repeat_header_and_preserve_every_row(self):
        self.journal([self.row(i) for i in range(1, 6)])
        result = excel.export_workbook(self.source, self.output, sheet_row_limit=6)
        self.assertEqual((result['rows'], result['sheets']), (5, 3))
        sheets = self.workbook_rows()
        self.assertEqual([len(rows) for rows in sheets], [6, 6, 3])
        sequences = []
        for rows in sheets:
            self.assertEqual(rows[0].find('m:c/m:is/m:t', NS).text, 'Targets')
            for row in rows[::3]:
                cells = list(row)
                sequences.append(cells[9].find('m:v', NS).text)
        self.assertEqual(sequences, ['1', '2', '3', '4', '5'])

    def test_frozen_size_excludes_later_appends_and_incomplete_row(self):
        self.journal([self.row(1)])
        frozen = self.source.stat().st_size
        with self.source.open('ab') as stream:
            stream.write(self.row(2).encode())
        excel.export_workbook(self.source, self.output, input_size=frozen)
        self.assertEqual(len(self.workbook_rows()[0]), 3)
        excel.export_workbook(self.source, self.output)
        self.assertEqual(len(self.workbook_rows()[0]), 6)
        with self.source.open('ab') as stream:
            stream.write(b'incomplete\trow')
        excel.export_workbook(self.source, self.output)
        self.assertEqual(len(self.workbook_rows()[0]), 6)

    def test_size_is_captured_before_reading_the_stream(self):
        self.journal([self.row(1)])
        original = excel.frozen_lines
        def append_then_read(stream, size):
            with self.source.open('ab') as current:
                current.write(self.row(2).encode())
            return original(stream, size)
        with patch.object(excel, 'frozen_lines', append_then_read):
            result = excel.export_workbook(self.source, self.output)
        self.assertEqual(result['rows'], 1)
        self.assertEqual(len(self.workbook_rows()[0]), 3)

    def test_empty_input_and_header_only_create_valid_header(self):
        for contents in ('', '\t'.join(excel.HEADER) + '\n'):
            self.source.write_text(contents, encoding='utf-8')
            result = excel.export_workbook(self.source, self.output)
            self.assertEqual(result['rows'], 0)
            self.assertEqual(len(self.workbook_rows()[0]), 1)

    def test_no_target_row_has_blank_numeric_cells(self):
        self.journal(['2026-10-27T15:01:02.003+0800\t1280\t720\tnone\tnull\tnull\tnull\tnull\tnull\t2\tnull\tnull\tfalse\n'])
        excel.export_workbook(self.source, self.output)
        rows = self.workbook_rows()[0]
        self.assertEqual(len(rows), 1)
        cells = list(rows[0])
        self.assertEqual(cells[1].find('m:v', NS).text, '0')
        self.assertIsNone(cells[11].find('m:v', NS))
        self.assertEqual(cells[15].find('m:v', NS).text, '0')

    def test_multiple_targets_and_camera_share_one_frame_block(self):
        self.journal([self.row(7, camera='UGREEN Camera (/dev/video1)'),
                      self.row(7, name='bird', camera='UGREEN Camera (/dev/video1)')], camera=True)
        result = excel.export_workbook(self.source, self.output)
        rows = self.workbook_rows()[0]
        self.assertEqual((result['frames'], result['rows'], result['workbook_rows']), (1, 2, 4))
        self.assertEqual(list(rows[0])[1].find('m:v', NS).text, '2')
        self.assertEqual(list(rows[0])[5].find('m:is/m:t', NS).text, 'UGREEN Camera (/dev/video1)')
        self.assertEqual([cell.find('m:is/m:t', NS).text for cell in rows[1]], list(excel.OBJECT_HEADER))
        self.assertEqual([list(row)[0].find('m:is/m:t', NS).text for row in rows[2:]], ['person', 'bird'])

    def test_legacy_and_camera_extension_can_mix_in_one_journal(self):
        self.journal([self.row(1), self.row(2, camera='USB\\tCamera\\n/dev/video2'), self.row(2, name='bird')])
        result = excel.export_workbook(self.source, self.output)
        rows = self.workbook_rows()[0]
        self.assertEqual((result['frames'], result['rows']), (2, 3))
        self.assertEqual(list(rows[0])[5].find('m:is/m:t', NS).text, excel.UNKNOWN_CAMERA)
        self.assertEqual(list(rows[3])[5].find('m:is/m:t', NS).text, 'USB\tCamera\n/dev/video2')
        self.journal([self.row(camera='')], camera=True)
        excel.export_workbook(self.source, self.output)
        self.assertEqual(list(self.workbook_rows()[0][0])[5].find('m:is/m:t', NS).text, 'Unknown')

    def test_capture_time_separates_restarted_frame_sequences(self):
        self.journal([self.row(1), self.row(1).replace('15:01:02.003', '15:02:02.003')])
        result = excel.export_workbook(self.source, self.output)
        self.assertEqual((result['frames'], len(self.workbook_rows()[0])), (2, 6))

    def test_frame_blocks_do_not_split_at_normal_sheet_boundary(self):
        self.journal([self.row(1), self.row(2), self.row(2, name='bird')])
        result = excel.export_workbook(self.source, self.output, sheet_row_limit=6)
        self.assertEqual((result['frames'], result['sheets']), (2, 2))
        self.assertEqual([len(rows) for rows in self.workbook_rows()], [3, 4])

    def test_oversized_frame_has_explicit_continuations(self):
        self.journal([self.row(1, name='target '+str(i)) for i in range(5)])
        result = excel.export_workbook(self.source, self.output, sheet_row_limit=4)
        sheets = self.workbook_rows()
        self.assertEqual((result['frames'], result['rows'], result['sheets']), (1, 5, 3))
        self.assertEqual([len(rows) for rows in sheets], [4, 4, 3])
        self.assertEqual([list(rows[0])[17].find('m:is/m:t', NS).text for rows in sheets], ['1 of 3', '2 of 3', '3 of 3'])
        self.assertTrue(all(list(rows[0])[1].find('m:v', NS).text == '5' for rows in sheets))

    def test_real_target_named_none_is_not_a_zero_target_frame(self):
        self.journal([self.row(name='none')])
        result = excel.export_workbook(self.source, self.output)
        self.assertEqual((result['frames'], result['rows']), (1, 1))

    def test_bad_rows_and_publish_failure_preserve_previous_workbook(self):
        self.journal([self.row()])
        excel.export_workbook(self.source, self.output)
        previous = self.output.read_bytes()
        for bad in (self.row().replace('0.750000', 'NaN'), 'wrong\tcolumns\n'):
            self.journal([bad])
            with self.assertRaises(ValueError):
                excel.export_workbook(self.source, self.output)
            self.assertEqual(self.output.read_bytes(), previous)
            self.assertFalse(list(self.root.glob('*.tmp')))
        self.journal([self.row(2)])
        with patch.object(excel.os, 'replace', side_effect=OSError('simulated replacement failure')):
            with self.assertRaises(OSError):
                excel.export_workbook(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), previous)
        self.assertFalse(list(self.root.glob('*.tmp')))

    def test_wrong_header_and_invalid_limits_are_rejected(self):
        self.source.write_text('wrong header\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            excel.export_workbook(self.source, self.output)
        self.journal([])
        for limit in (0, 1, excel.MAX_SHEET_ROWS + 1):
            with self.assertRaises(ValueError):
                excel.export_workbook(self.source, self.output, sheet_row_limit=limit)

    def test_cli_exports_a_workbook(self):
        self.journal([self.row()])
        result = subprocess.run([sys.executable, str(SCRIPT), '--input', str(self.source), '--output', str(self.output),
            '--sheet-row-limit', '3'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"rows": 1', result.stdout)
        self.workbook_rows()

    @unittest.skipUnless(hasattr(os, 'symlink'), 'symlinks unavailable')
    def test_symlink_input_output_and_parent_are_rejected(self):
        self.journal([self.row()])
        link = self.root / 'link.rows'
        try:
            link.symlink_to(self.source)
        except OSError as error:
            self.skipTest(str(error))
        with self.assertRaises(ValueError):
            excel.export_workbook(link, self.output)
        link.unlink()
        link.symlink_to(self.source)
        with self.assertRaises(ValueError):
            excel.export_workbook(self.source, link)
        parent = self.root / 'linked'
        parent.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            excel.export_workbook(parent / self.source.name, self.output)


if __name__ == '__main__':
    unittest.main()
