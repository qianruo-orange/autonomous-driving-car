#!/usr/bin/env python3
"""把一个 84×A 的 ONNX 转成当前部署形态的 RKNN：双输出 + FP16 框。

产物有两路输出：
  /model.23/Sigmoid_output_0  [1,A,80]  INT8（类别分数）
  /model.23/Mul_2_output_0    [1,4,A]   FP16（cx,cy,w,h，模型像素）

框走 FP16 是因为 INT8 的框量化步长约 2.73 px（±1.4 px），实测让 mAP50-95 掉
2.45 个点；FP16 每帧只多 33 KB，输出转换耗时在 0–1 ms。

    python3 build_rknn.py --onnx yolo26n-512.onnx --dataset calibration.txt \
                          --imgsz 512 --out yolo26n-512-box16.rknn

要求 rknn-toolkit2 2.3.2（2.5.0 在 MatMul 融合阶段崩溃，见 docs/YOLO26N_QUANTIZATION.md）。
"""
import argparse, os, time
from pathlib import Path

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import yaml
from rknn.api import RKNN

SCORES = "/model.23/Sigmoid_output_0"
BOXES = "/model.23/Mul_2_output_0"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--dataset", required=True, help="校准图片列表（每行一个 PNG/JPG 路径）")
    ap.add_argument("--imgsz", type=int, default=512)
    ap.add_argument("--platform", default="rk3576")
    ap.add_argument("--out", required=True, help="输出 .rknn")
    ap.add_argument("--keep-intermediate", action="store_true",
                    help="保留 step1 产生的 .model/.data/.quantization.cfg")
    args = ap.parse_args()

    onnx_path = Path(args.onnx)
    stem = onnx_path.with_suffix("")
    out = Path(args.out)

    print(f"step1：截断为双输出并统计量化信息（{args.imgsz}）……", flush=True)
    started = time.monotonic()
    rknn = RKNN(verbose=False)
    assert rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
                       target_platform=args.platform) == 0
    assert rknn.load_onnx(model=str(onnx_path), outputs=[SCORES, BOXES]) == 0
    # proposal=False：该模型不支持 expand batch，proposal 会报
    # "This model does not support expand batch"
    assert rknn.hybrid_quantization_step1(dataset=args.dataset, proposal=False) == 0
    rknn.release()
    print(f"step1 完成 {time.monotonic() - started:.1f}s", flush=True)

    cfg = Path(str(stem) + ".quantization.cfg")
    doc = yaml.safe_load(cfg.read_text())
    custom = doc.get("custom_quantize_layers") or {}
    custom[BOXES] = "float16"
    custom[BOXES + "-rs"] = "float16"
    doc["custom_quantize_layers"] = custom
    cfg.write_text(yaml.safe_dump(doc, sort_keys=False))

    print("step2：按配置生成 RKNN……", flush=True)
    rknn = RKNN(verbose=False)
    # 注意：step2 里绝不能再次调用 config()，否则会报 "config of 'step2' is
    # inconsistent with 'step1'"
    assert rknn.hybrid_quantization_step2(model_input=str(stem) + ".model",
                                          data_input=str(stem) + ".data",
                                          model_quantization_cfg=str(cfg)) == 0
    out.parent.mkdir(parents=True, exist_ok=True)
    assert rknn.export_rknn(str(out)) == 0
    rknn.release()
    print(f"BUILD_OK {out} ({out.stat().st_size} 字节)")

    if not args.keep_intermediate:
        for suffix in (".model", ".data", ".quantization.cfg"):
            Path(str(stem) + suffix).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
