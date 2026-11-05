#!/usr/bin/env python3
"""把 YOLO26n checkpoint 导出成部署用的 512×512 ONNX。

导出的图只有 84×A 的单张量（cx,cy,w,h + 80 类分数），供 build_rknn.py 截断成
双输出。必须在 512 输入下重新导出：8400→5376 个锚点是图里的常量，不能事后改。

    python3 export_onnx.py --weights yolo26n.pt --imgsz 512 --out yolo26n-512.onnx

依赖：ultralytics / torch / onnx / onnxslim（见 requirements.txt）。
"""
import argparse, hashlib, json, os
from pathlib import Path

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("YOLO_AUTOINSTALL", "False")

import torch
from ultralytics import YOLO
from ultralytics.nn.modules import Detect
import onnx
from onnxslim import slim

torch.set_num_threads(1)
torch.set_num_interop_threads(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", required=True, help="yolo26n.pt")
    ap.add_argument("--imgsz", type=int, default=512, help="边长（默认 512）")
    ap.add_argument("--out", required=True, help="输出 .onnx 路径")
    args = ap.parse_args()

    imgsz = args.imgsz
    anchors = (imgsz // 8) ** 2 + (imgsz // 16) ** 2 + (imgsz // 32) ** 2

    model = YOLO(args.weights).model.eval()
    for module in model.modules():
        if isinstance(module, Detect):
            module.end2end = False
            module.export = True
            module.format = "onnx"
            module.dynamic = False
    model.fuse(verbose=False)

    dummy = torch.zeros(1, 3, imgsz, imgsz)
    with torch.no_grad():
        probe = model(dummy)
    print(f"torch 输出 {tuple(probe.shape)} 坐标上限 {float(probe[0, :4].max()):.2f}")
    expected = (1, 84, anchors)
    if tuple(probe.shape) != expected:
        raise SystemExit(f"输出形状 {tuple(probe.shape)} 与预期 {expected} 不符")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(model, dummy, out, opset_version=12, do_constant_folding=True,
                      input_names=["images"], output_names=["output0"], dynamic_axes=None)

    proto = onnx.load(str(out))
    onnx.checker.check_model(proto)
    proto = slim(proto)
    onnx.save(proto, str(out))
    onnx.checker.check_model(proto)

    meta = {
        "checkpoint_sha256": hashlib.sha256(Path(args.weights).read_bytes()).hexdigest(),
        "onnx_sha256": hashlib.sha256(out.read_bytes()).hexdigest(),
        "shape": list(probe.shape), "imgsz": imgsz, "anchors": anchors,
        "coordinate_units": "pixels-cxcywh", "opset": 12,
        "nodes": len(proto.graph.node),
    }
    meta_path = out.with_suffix(".json")
    meta_path.write_text(json.dumps(meta, indent=2))
    print("EXPORT_OK", json.dumps(meta))
    print("写出", out, "与", meta_path)


if __name__ == "__main__":
    main()
