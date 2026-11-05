#!/usr/bin/env python3
"""准备量化校准集与精度验证集（letterbox 到模型输入尺寸）。

两个子命令：

calibration —— 把图片等比缩放并居中补 114 灰边成 imgsz×imgsz 的 PNG，输出一个
               每行一个路径的列表，直接喂给 build_rknn.py --dataset。
validation  —— 生成验证输入 .bin、geometry manifest 和 results.jsonl，
               后者记录每张图的缩放/补边参数，evaluate.py 用它把框逆映射回原图。

    python3 prepare_dataset.py calibration --images calib.txt --imgsz 512 --out work/letterbox512
    python3 prepare_dataset.py validation  --manifest validation.tsv --imgsz 512 \
            --out work/v512-in --raw work/v512-raw

校准与验证必须使用**同一套** letterbox 参数，否则量化时的激活分布与推理时不一致。
"""
import argparse, json
from pathlib import Path

import cv2
import numpy as np

FILL = 114


def letterbox(path, imgsz):
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None:
        raise SystemExit(f"读不到图片: {path}")
    height, width = image.shape[:2]
    scale = min(imgsz / width, imgsz / height)
    rw, rh = max(1, int(width * scale + 0.5)), max(1, int(height * scale + 0.5))
    px, py = (imgsz - rw) // 2, (imgsz - rh) // 2
    canvas = np.full((imgsz, imgsz, 3), FILL, dtype=np.uint8)
    canvas[py:py + rh, px:px + rw] = cv2.resize(image, (rw, rh), interpolation=cv2.INTER_LINEAR)
    return canvas, width, height, rw / width, rh / height, px, py


def cmd_calibration(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    entries = []
    for line in Path(args.images).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        src = Path(line.split("\t")[1] if "\t" in line else line)
        canvas, *_ = letterbox(src, args.imgsz)
        dst = out / (src.stem + ".png")
        cv2.imwrite(str(dst), canvas)
        entries.append(str(dst))
    listing = Path(args.images).with_name(Path(args.images).stem + f"-{args.imgsz}.txt")
    listing.write_text("\n".join(entries) + "\n")
    print(f"CALIBRATION_OK {len(entries)} 张，列表 {listing}")


def cmd_validation(args):
    out = Path(args.out)
    raw = Path(args.raw)
    out.mkdir(parents=True, exist_ok=True)
    raw.mkdir(parents=True, exist_ok=True)
    results_path = out.with_name(out.name.replace("-in", "") + "-results.jsonl")
    rows = []
    for line in Path(args.manifest).read_text().splitlines():
        if not line.strip():
            continue
        ident, image, width, height, label = line.split("\t")[:5]
        canvas, w, h, sx, sy, px, py = letterbox(image, args.imgsz)
        (out / f"{ident}.bin").write_bytes(canvas.tobytes())
        rows.append({
            "record": "image", "id": ident, "original": image, "label": label,
            "width": int(width), "height": int(height),
            "scale_x": sx, "scale_y": sy, "pad_x": px, "pad_y": py,
            "raw_output": str(raw / f"{ident}.f32"), "raw_layout": "NCHW",
        })
    results_path.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    print(f"VALIDATION_OK {len(rows)} 张，输入 {out}，结果 {results_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    cal = sub.add_parser("calibration")
    cal.add_argument("--images", required=True, help="图片列表（每行一个路径，或 id\\t路径）")
    cal.add_argument("--imgsz", type=int, default=512)
    cal.add_argument("--out", required=True, help="letterbox 输出目录")
    cal.set_defaults(func=cmd_calibration)

    val = sub.add_parser("validation")
    val.add_argument("--manifest", required=True,
                     help="TSV：id\\t图片路径\\t宽\\t高\\t标签路径")
    val.add_argument("--imgsz", type=int, default=512)
    val.add_argument("--out", required=True, help=".bin 输入目录")
    val.add_argument("--raw", required=True, help="dump_outputs 输出目录")
    val.set_defaults(func=cmd_validation)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
