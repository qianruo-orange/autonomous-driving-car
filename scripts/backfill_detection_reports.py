"""Backfill legacy detection JSON into daily TSV reports; never removes input files.

Run while the vehicle service is stopped. The caller supplies the exact detection
directory. All writes use a temporary sibling followed by atomic replacement.
"""
import argparse
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

HEADER = "capture_time\twidth\theight\ttarget\tconfidence\tx1\ty1\tx2\ty2\tframe_seq\tfocus_position_raw\tfocal_length_mm\tautofocus\n"
DAY = re.compile(r"\d{8}\Z")
LEGACY = re.compile(r"det_\d{6}_\d{3}_\d+\.json\Z")


def escape_target(value):
    result = []
    for character in str(value):
        if character == "\\": result.append("\\\\")
        elif character == "\t": result.append("\\t")
        elif character == "\r": result.append("\\r")
        elif character == "\n": result.append("\\n")
        elif ord(character) < 32 or ord(character) == 127: result.append(f"\\x{ord(character):02x}")
        else: result.append(character)
    return "".join(result)


def metadata_rows(metadata):
    sequence = metadata["seq"]
    width, height, milliseconds = metadata["width"], metadata["height"], metadata["capture_unix_ms"]
    if not all(isinstance(v, int) and not isinstance(v, bool) for v in (sequence, width, height, milliseconds)):
        raise ValueError("invalid integer metadata")
    if min(width, height) < 1 or sequence < 0 or milliseconds < 0: raise ValueError("invalid dimensions/time/sequence")
    stamp = datetime.fromtimestamp(milliseconds // 1000).astimezone()
    time_text = stamp.strftime("%Y-%m-%dT%H:%M:%S") + f".{milliseconds % 1000:03d}" + stamp.strftime("%z")
    focus = metadata.get("focus_position_raw")
    focus = str(focus) if isinstance(focus, int) and not isinstance(focus, bool) else "null"
    autofocus = metadata.get("autofocus")
    autofocus = "true" if autofocus is True else "false" if autofocus is False else "null"
    objects = metadata["objects"]
    if not isinstance(objects, list) or not 1 <= len(objects) <= 32: raise ValueError("invalid detection list")
    result = []
    for item in objects:
        box, confidence = item["bbox"], item["conf"]
        if not isinstance(box, list) or len(box) != 4 or not all(isinstance(v, (float, int)) and math.isfinite(v) for v in [confidence] + box):
            raise ValueError("invalid detection coordinates/confidence")
        columns = [time_text, str(width), str(height), escape_target(item.get("name", "obj")),
                   f"{confidence:.6f}", *(f"{value:.3f}" for value in box), str(sequence), focus, "null", autofocus]
        result.append("\t".join(columns) + "\n")
    return stamp.strftime("%Y%m%d"), result


def migrate(root, dry_run=False):
    root = Path(root).resolve(strict=True)
    if not root.is_dir(): raise ValueError("detection root must be a directory")
    grouped = {}
    errors = []
    inputs = 0
    for directory in sorted(root.iterdir()):
        if directory.is_symlink() or not directory.is_dir() or not DAY.fullmatch(directory.name): continue
        for source in sorted(directory.iterdir()):
            if source.is_symlink() or not source.is_file() or not LEGACY.fullmatch(source.name): continue
            try:
                if source.stat().st_size > 32768: raise ValueError("metadata file is too large")
                day, rows = metadata_rows(json.loads(source.read_text(encoding="utf-8")))
                grouped.setdefault(day, []).extend(rows)
                inputs += 1
            except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
                errors.append({"file": str(source.relative_to(root)), "error": str(error)})
    added = 0
    for day, rows in grouped.items():
        directory = root / day
        if directory.exists() and (directory.is_symlink() or not directory.is_dir()):
            raise ValueError("unsafe report directory: " + day)
        target = directory / ("detections_" + day + ".txt")
        existing = []
        mode = 0o644
        if target.exists() or target.is_symlink():
            if target.is_symlink() or not target.is_file(): raise ValueError("unsafe report file: " + day)
            mode = stat.S_IMODE(target.stat().st_mode)
            with target.open("r", encoding="utf-8", newline="") as stream:
                if stream.readline() != HEADER: raise ValueError("unexpected report header: " + day)
                existing = stream.readlines()
            if any(not row.endswith("\n") or len(row.rstrip("\n").split("\t")) != 13 for row in existing):
                raise ValueError("incomplete/malformed existing report: " + day)
        seen = set(existing)
        new_rows = []
        for row in rows:
            if row not in seen: new_rows.append(row); seen.add(row)
        if not new_rows: continue
        added += len(new_rows)
        if dry_run: continue
        directory.mkdir(mode=0o755, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(prefix=".detections-backfill-", dir=directory)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
                stream.write(HEADER)
                stream.writelines(existing)
                stream.writelines(new_rows)
                stream.flush(); os.fsync(stream.fileno())
            os.chmod(temporary, mode)
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
    return {"legacy_files_read": inputs, "days": len(grouped), "rows_added": added, "errors": errors, "dry_run": dry_run}


def self_test():
    with tempfile.TemporaryDirectory(prefix="car-report-backfill-") as scratch:
        root = Path(scratch)
        directory = root / "20261027"; directory.mkdir()
        metadata = {"seq": 7, "width": 1280, "height": 720, "capture_unix_ms": 1793030400123,
                    "focus_position_raw": None, "autofocus": True,
                    "objects": [{"name": "target\\\t\n", "conf": .9, "bbox": [1, 2, 3, 4]},
                                {"name": "person", "conf": .8, "bbox": [5, 6, 7, 8]}]}
        source = directory / "det_010203_004_7.json"
        source.write_text(json.dumps(metadata), encoding="utf-8")
        first = migrate(root)
        assert first["rows_added"] == 2 and not first["errors"]
        assert source.exists()
        reports = list(root.glob("*/detections_*.txt")); assert len(reports) == 1
        body = reports[0].read_text(encoding="utf-8")
        assert body.count(HEADER) == 1 and len(body.splitlines()) == 3
        assert "1280\t720\ttarget\\\\\\t\\n\t0.900000" in body
        assert "\t7\tnull\tnull\ttrue\n" in body
        assert migrate(root)["rows_added"] == 0 and reports[0].read_text() == body
        metadata["seq"] = 8; metadata["capture_unix_ms"] += 1000
        (directory / "det_010204_004_8.json").write_text(json.dumps(metadata), encoding="utf-8")
        assert migrate(root, True)["rows_added"] == 2 and reports[0].read_text() == body
        assert migrate(root)["rows_added"] == 2
        assert source.exists() and not list(root.glob("*/*.jpg"))
        (directory / "det_010205_004_9.json").write_text("broken JSON", encoding="utf-8")
        failed = migrate(root)
        assert len(failed["errors"]) == 1 and failed["rows_added"] == 0
    print("PASS: legacy JSON backfill preserves inputs, escapes TSV, supports multiple targets and is idempotent")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test: self_test()
    else:
        if options.root is None: parser.error("--root is required")
        outcome = migrate(options.root, options.dry_run)
        print(json.dumps(outcome))
        sys.exit(bool(outcome["errors"]))
