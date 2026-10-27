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
CAMERA_HEADER = HEADER + ('camera',)
OBJECT_HEADER = ('Target', 'Confidence', 'X1', 'Y1', 'X2', 'Y2')
UNKNOWN_CAMERA = 'Unknown (not recorded)'
MAX_FRAME_OBJECTS = 65536


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


def text_cell(reference, value, header=False, style_number=0):
    style_number = 1 if header else style_number
    style = f' s="{style_number}"' if style_number else ''
    return f'<c r="{reference}" t="inlineStr"{style}><is><t xml:space="preserve">{xml_text(value)}</t></is></c>'


def numeric_text(value, integer=False):
    if integer:
        if not re.fullmatch(r'-?[0-9]+', value):
            raise ValueError('Invalid integer in detection journal')
        return str(int(value))
    try:
        numeric = decimal.Decimal(value)
    except decimal.InvalidOperation as error:
        raise ValueError('Invalid number in detection journal') from error
    if not numeric.is_finite():
        raise ValueError('Non-finite number in detection journal')
    return str(numeric)


def typed_cell(reference, kind, value, style=0):
    if kind == 'text':
        return text_cell(reference, str(value), style_number=style)
    style_attribute = f' s="{style}"' if style else ''
    if value in ('null', '', None):
        return f'<c r="{reference}"{style_attribute}/>'
    if kind == 'bool':
        if value not in ('true', 'false'):
            raise ValueError('Invalid autofocus value')
        return f'<c r="{reference}" t="b"{style_attribute}><v>{int(value == "true")}</v></c>'
    number = numeric_text(str(value), kind == 'int')
    return f'<c r="{reference}"{style_attribute}><v>{number}</v></c>'


def row_xml(number, cells):
    return ('<row r="' + str(number) + '">' + ''.join(
        typed_cell(chr(ord('A') + index) + str(number), *cell)
        for index, cell in enumerate(cells)) + '</row>').encode('utf-8')


def unescape_journal(value):
    def decode(match):
        token = match.group(1)
        if token.startswith('x'):
            return chr(int(token[1:], 16))
        return {'t':'\t', 'r':'\r', 'n':'\n', '\\':'\\'}[token]
    return re.sub(r'\\(x[0-9a-fA-F]{2}|[trn\\])', decode, value)


def parse_record(line):
    fields = line.split('\t')
    if len(fields) not in (13, 14):
        raise ValueError('Detection row must contain 13 legacy or 14 camera columns')
    # Validate every numeric and boolean value, including metadata repeated in
    # object rows, before an atomic workbook can replace its previous snapshot.
    for index in range(1, 13):
        value = fields[index]
        if index in TEXT_COLUMNS or value in ('null', ''):
            continue
        if index == 12:
            if value not in ('true', 'false'):
                raise ValueError('Invalid autofocus value')
        else:
            numeric_text(value, index in INTEGER_COLUMNS)
    if not fields[0] or int(fields[1]) <= 0 or int(fields[2]) <= 0:
        raise ValueError('Missing capture time or invalid image dimensions')
    numeric_text(fields[9], True)
    camera = unescape_journal(fields[13]) if len(fields) == 14 else UNKNOWN_CAMERA
    if not camera:
        camera = 'Unknown'
    return fields[:13] + [camera]


def grouped_frames(lines):
    key, metadata, objects = None, None, []
    for line in lines:
        fields = parse_record(line)
        current = (fields[0], fields[9], fields[1], fields[2])
        if key is not None and current != key:
            yield metadata, objects
            metadata, objects = None, []
        if metadata is None:
            metadata = fields
            key = current
        elif metadata[13] == UNKNOWN_CAMERA and fields[13] != UNKNOWN_CAMERA:
            metadata[13] = fields[13]
        no_targets = fields[3] == 'none' and all(value in ('null', '') for value in fields[4:9])
        if not no_targets:
            if len(objects) == MAX_FRAME_OBJECTS:
                raise ValueError('Detection frame exceeds supported object count')
            objects.append(fields)
    if metadata is not None:
        yield metadata, objects


def summary_cells(metadata, count, continuation=None):
    pairs = (
        ('Targets', 'int', count),
        ('Resolution', 'text', metadata[1] + ' × ' + metadata[2]),
        ('Camera', 'text', metadata[13]),
        ('Capture time', 'text', metadata[0]),
        ('Frame sequence', 'int', metadata[9]),
        ('Lens position raw', 'int', metadata[10]),
        ('Focal length mm', 'number', metadata[11]),
        ('Autofocus', 'bool', metadata[12]),
    )
    cells = []
    for label, kind, value in pairs:
        cells.extend((('text', label, 2), (kind, value, 3)))
    if continuation:
        cells.extend((('text', 'Continuation', 2), ('text', continuation, 3)))
    return cells


def object_cells(fields):
    return [('text', unescape_journal(fields[3]))] + [('number', value) for value in fields[4:9]]


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
        f'<worksheet xmlns="{MAIN_NS}"><sheetViews><sheetView workbookViewId="0"/></sheetViews>'
        '<cols>' + ''.join(f'<col min="{column}" max="{column}" width="{ {1:28,6:42,8:32}.get(column,18) }" customWidth="1"/>'
            for column in range(1, 19)) + '</cols><sheetData>').encode())
    return stream


def worksheet_end(stream, row_count):
    stream.write(b'</sheetData></worksheet>')
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
        '<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>'
        '<fill><patternFill patternType="solid"><fgColor rgb="FFDCEAF7"/><bgColor indexed="64"/></patternFill></fill></fills>'
        '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
        '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
        '<cellXfs count="4"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
        '<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment wrapText="1"/></xf>'
        '<xf numFmtId="0" fontId="0" fillId="2" borderId="0" xfId="0" applyFill="1" applyAlignment="1"><alignment wrapText="1"/></xf></cellXfs>'
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
        total_rows, frames, workbook_rows, sheets, sheet_rows = 0, 0, 0, 1, 0
        with os.fdopen(descriptor, 'rb') as source_stream:
            descriptor = -1
            lines = iter(frozen_lines(source_stream, size))
            first = next(lines, None)
            if first is not None and tuple(first.split('\t')) not in (HEADER, CAMERA_HEADER):
                raise ValueError('Unexpected detection journal header')
            with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
                sheet = worksheet_begin(archive, sheets)
                try:
                    for metadata, objects in grouped_frames(lines):
                        frames += 1
                        count = len(objects)
                        if count and sheet_row_limit < 3:
                            raise ValueError('At least three worksheet rows are required for a detection frame')
                        capacity = sheet_row_limit - 2
                        chunks = [objects[index:index + capacity] for index in range(0, count, capacity)] if count else [[]]
                        for part, chunk in enumerate(chunks, 1):
                            needed = 1 + (1 + len(chunk) if chunk else 0)
                            if sheet_rows and sheet_rows + needed > sheet_row_limit:
                                worksheet_end(sheet, sheet_rows)
                                sheets += 1
                                sheet = worksheet_begin(archive, sheets)
                                sheet_rows = 0
                            sheet_rows += 1
                            marker = f'{part} of {len(chunks)}' if len(chunks) > 1 else None
                            sheet.write(row_xml(sheet_rows, summary_cells(metadata, count, marker)))
                            workbook_rows += 1
                            if chunk:
                                sheet_rows += 1
                                sheet.write(row_xml(sheet_rows, [('text', value, 1) for value in OBJECT_HEADER]))
                                workbook_rows += 1
                                for fields in chunk:
                                    sheet_rows += 1
                                    sheet.write(row_xml(sheet_rows, object_cells(fields)))
                                    total_rows += 1
                                    workbook_rows += 1
                    if not frames:
                        sheet_rows = 1
                        workbook_rows = 1
                        sheet.write(row_xml(1, [('text', 'No detection frames', 1)]))
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
        return {'rows':total_rows, 'frames':frames, 'workbook_rows':workbook_rows,
                'sheets':sheets, 'input_bytes':size, 'bytes':destination.stat().st_size}
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
