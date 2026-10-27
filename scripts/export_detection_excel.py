#!/usr/bin/env python3
"""Export an append-only daily detection journal as a standard Excel workbook.

Only the input size captured at open time is read. Worksheet XML is streamed
directly into ZIP64 members; memory use does not grow with the report. The old
workbook remains intact if parsing, ZIP generation or publication fails.
"""
import argparse
import decimal
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from xml.sax.saxutils import escape
import zipfile

HEADER = ('capture_time', 'width', 'height', 'target', 'confidence', 'x1', 'y1',
          'x2', 'y2', 'frame_seq', 'focus_position_raw', 'focal_length_mm', 'autofocus')
MAX_SHEET_ROWS = 1048576
MAX_LINE_BYTES = 131072
MAIN_NS = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main'
REL_NS = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
PACKAGE_REL_NS = 'http://schemas.openxmlformats.org/package/2006/relationships'
INTEGER_COLUMNS = {1, 2, 9, 10}
TEXT_COLUMNS = {0, 3}
OOXML_ESCAPE = re.compile(r'_x[0-9A-Fa-f]{4}_')


def safe_path(value, must_exist=False):
    path = Path(value).absolute()
    # Reject symlink components before any resolve() can hide them.
    for component in (path, *path.parents):
        try:
            mode = component.lstat().st_mode
        except FileNotFoundError:
            if component == path and not must_exist:
                continue
            raise
        if stat.S_ISLNK(mode):
            raise ValueError('Symlink paths are not accepted: ' + str(component))
    if must_exist and not path.is_file():
        raise ValueError('Input must be a regular file')
    if path.exists() and not path.is_file():
        raise ValueError('Output must be a regular file')
    if not path.parent.is_dir():
        raise ValueError('Output directory does not exist')
    return path


def xml_text(value):
    # Preserve literal OOXML escape-looking text rather than letting Excel
    # reinterpret a target such as "_x000A_" as a newline.
    value = OOXML_ESCAPE.sub(lambda match: '_x005F_' + match.group()[1:], value)
    value = ''.join(character if (character in '\t\n\r' or
        0x20 <= ord(character) <= 0xD7FF or 0xE000 <= ord(character) <= 0xFFFD or
        0x10000 <= ord(character) <= 0x10FFFF) else '\uFFFD' for character in value)
    return escape(value)


def text_cell(reference, value, header=False):
    style = ' s="1"' if header else ''
    return f'<c r="{reference}" t="inlineStr"{style}><is><t xml:space="preserve">{xml_text(value)}</t></is></c>'


def row_xml(number, fields, header=False):
    if len(fields) != len(HEADER):
        raise ValueError('Detection row must contain exactly 13 columns')
    cells = []
    for index, value in enumerate(fields):
        reference = chr(ord('A') + index) + str(number)
        if header or index in TEXT_COLUMNS:
            cells.append(text_cell(reference, value, header))
        elif value == 'null' or value == '':
            cells.append(f'<c r="{reference}"/>')
        elif index == 12:
            if value not in ('true', 'false'):
                raise ValueError('Invalid autofocus value')
            cells.append(f'<c r="{reference}" t="b"><v>{int(value == "true")}</v></c>')
        else:
            if index in INTEGER_COLUMNS:
                if not re.fullmatch(r'-?[0-9]+', value):
                    raise ValueError('Invalid integer in ' + HEADER[index])
                number_text = str(int(value))
            else:
                try:
                    numeric = decimal.Decimal(value)
                except decimal.InvalidOperation as error:
                    raise ValueError('Invalid number in ' + HEADER[index]) from error
                if not numeric.is_finite():
                    raise ValueError('Non-finite number in ' + HEADER[index])
                number_text = str(numeric)
            cells.append(f'<c r="{reference}"><v>{number_text}</v></c>')
    return ('<row r="' + str(number) + '">' + ''.join(cells) + '</row>').encode('utf-8')


def frozen_lines(stream, size):
    remaining = size
    while remaining:
        line = stream.readline(min(remaining, MAX_LINE_BYTES + 1))
        if not line:
            raise ValueError('Detection journal was truncated during export')
        remaining -= len(line)
        if len(line) > MAX_LINE_BYTES:
            raise ValueError('Detection journal line exceeds the supported size')
        if not line.endswith(b'\n'):
            # A concurrent writer may have been in the middle of its last row at
            # the captured byte boundary. Never export that incomplete record.
            if remaining:
                raise ValueError('Detection journal line exceeds the supported size')
            break
        yield line.rstrip(b'\r\n').decode('utf-8')


def worksheet_begin(archive, sheet_number):
    stream = archive.open(f'xl/worksheets/sheet{sheet_number}.xml', 'w', force_zip64=True)
    stream.write((f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        f'<worksheet xmlns="{MAIN_NS}"><sheetViews><sheetView workbookViewId="0">'
        '<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>'
        '</sheetView></sheetViews><cols><col min="1" max="1" width="30" customWidth="1"/>'
        '<col min="2" max="13" width="18" customWidth="1"/></cols><sheetData>').encode())
    stream.write(row_xml(1, HEADER, header=True))
    return stream


def worksheet_end(stream, row_count):
    stream.write(f'</sheetData><autoFilter ref="A1:M{row_count}"/></worksheet>'.encode())
    stream.close()


def write_package(archive, sheet_count):
    archive.writestr('[Content_Types].xml', '<?xml version="1.0" encoding="UTF-8"?>'
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
        '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>' +
        ''.join(f'<Override PartName="/xl/worksheets/sheet{i}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
                for i in range(1, sheet_count + 1)) + '</Types>')
    archive.writestr('_rels/.rels', f'<?xml version="1.0" encoding="UTF-8"?>'
        f'<Relationships xmlns="{PACKAGE_REL_NS}"><Relationship Id="rId1" Type="{REL_NS}/officeDocument" Target="xl/workbook.xml"/></Relationships>')
    archive.writestr('xl/workbook.xml', f'<?xml version="1.0" encoding="UTF-8"?>'
        f'<workbook xmlns="{MAIN_NS}" xmlns:r="{REL_NS}"><bookViews><workbookView/></bookViews><sheets>' +
        ''.join(f'<sheet name="Detections {i}" sheetId="{i}" r:id="rId{i}"/>' for i in range(1, sheet_count + 1)) +
        '</sheets></workbook>')
    archive.writestr('xl/_rels/workbook.xml.rels', f'<?xml version="1.0" encoding="UTF-8"?>'
        f'<Relationships xmlns="{PACKAGE_REL_NS}">' +
        ''.join(f'<Relationship Id="rId{i}" Type="{REL_NS}/worksheet" Target="worksheets/sheet{i}.xml"/>'
                for i in range(1, sheet_count + 1)) +
        f'<Relationship Id="rId{sheet_count + 1}" Type="{REL_NS}/styles" Target="styles.xml"/></Relationships>')
    archive.writestr('xl/styles.xml', f'<?xml version="1.0" encoding="UTF-8"?>'
        f'<styleSheet xmlns="{MAIN_NS}"><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font>'
        '<font><b/><sz val="11"/><name val="Calibri"/></font></fonts>'
        '<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>'
        '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
        '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
        '<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
        '<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>'
        '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>')


def export_workbook(input_path, output_path, sheet_row_limit=MAX_SHEET_ROWS, input_size=None):
    if not 2 <= sheet_row_limit <= MAX_SHEET_ROWS:
        raise ValueError('Sheet row limit must be between 2 and 1048576')
    source = safe_path(input_path, must_exist=True)
    destination = safe_path(output_path)
    if source == destination or (destination.exists() and os.path.samefile(source, destination)):
        raise ValueError('Input and output must be different files')
    descriptor = os.open(source, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    temporary = None
    try:
        source_stat = os.fstat(descriptor)
        if not stat.S_ISREG(source_stat.st_mode):
            raise ValueError('Input must be a regular file')
        size = source_stat.st_size if input_size is None else input_size
        if not isinstance(size, int) or not 0 <= size <= source_stat.st_size:
            raise ValueError('Invalid frozen input size')
        temp_descriptor, temporary = tempfile.mkstemp(prefix='.' + destination.name + '.', suffix='.tmp', dir=destination.parent)
        os.close(temp_descriptor)
        total_rows, sheets, sheet_rows = 0, 1, 1
        with os.fdopen(descriptor, 'rb') as source_stream:
            descriptor = -1
            lines = iter(frozen_lines(source_stream, size))
            first = next(lines, None)
            if first is not None and tuple(first.split('\t')) != HEADER:
                raise ValueError('Unexpected detection journal header')
            with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
                sheet = worksheet_begin(archive, sheets)
                try:
                    for line in lines:
                        fields = line.split('\t')
                        if sheet_rows == sheet_row_limit:
                            worksheet_end(sheet, sheet_rows)
                            sheets += 1
                            sheet = worksheet_begin(archive, sheets)
                            sheet_rows = 1
                        sheet_rows += 1
                        sheet.write(row_xml(sheet_rows, fields))
                        total_rows += 1
                    worksheet_end(sheet, sheet_rows)
                finally:
                    if not sheet.closed:
                        sheet.close()
                write_package(archive, sheets)
        os.chmod(temporary, 0o644)
        with open(temporary, 'r+b') as completed:
            os.fsync(completed.fileno())
        os.replace(temporary, destination)
        temporary = None
        # fsync the containing directory where supported (Linux board).
        if hasattr(os, 'O_DIRECTORY'):
            directory = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY | getattr(os, 'O_NOFOLLOW', 0))
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        return {'rows':total_rows, 'sheets':sheets, 'input_bytes':size, 'bytes':destination.stat().st_size}
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary is not None:
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--input-size', type=int)
    parser.add_argument('--sheet-row-limit', type=int, default=MAX_SHEET_ROWS)
    arguments = parser.parse_args()
    try:
        result = export_workbook(arguments.input, arguments.output, arguments.sheet_row_limit, arguments.input_size)
    except (OSError, ValueError, UnicodeError, zipfile.BadZipFile) as error:
        parser.exit(1, 'Excel export failed: ' + str(error) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
