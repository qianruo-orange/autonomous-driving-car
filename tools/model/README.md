# 模型转换与验证

本目录是 YOLO26n 从 checkpoint 到板端 `.rknn` 的完整离线链路。运行时代码只用 C
（`source/yolo/`），这里的脚本都是**离线工具**，不参与板上运行。

## 当前交付形态

| 项 | 值 |
|---|---|
| 输入 | 512×512 RGB（letterbox，114 灰边） |
| 输出 0 | `/model.23/Sigmoid_output_0` `[1,5376,80]`，INT8，NHWC |
| 输出 1 | `/model.23/Mul_2_output_0` `[1,4,5376]`，FP16，cx,cy,w,h（模型像素） |
| 模型体积 | 7,888,539 字节 |
| 精度（64 图留出集，conf≥0.001，IoU 0.7，101-pt AP） | mAP50 **0.5909**，mAP50–95 **0.4336** |
| 板端耗时 | `run` 8 ms，端到端 44 ms |
| 工具链 | rknn-toolkit2 **2.3.2**，librknnrt 2.5.0 |

## 链路

```
yolo26n.pt
   │ export_onnx.py            导出 512 ONNX（84×5376 单张量）
   ▼
yolo26n-512.onnx
   │ build_rknn.py             截断双输出 + 框走 FP16
   ▼
yolo26n-512-box16.rknn  ──►  config/yolo26.rknn  ──►  部署
```

### 1. 准备数据集

校准集（量化激活统计）与验证集（精度评测）必须用**同一套** letterbox 参数：

```sh
# 校准：每行一个图片路径（或 id<TAB>路径），输出 letterbox PNG + 路径列表
python3 prepare_dataset.py calibration --images calib.txt --imgsz 512 --out work/letterbox512

# 验证：TSV 为 id<TAB>图片<TAB>宽<TAB>高<TAB>标签路径
python3 prepare_dataset.py validation --manifest validation.tsv --imgsz 512 \
        --out work/v512-in --raw work/v512-raw
```

`validation` 会写 `work/v512-results.jsonl`，其中记录每张图的 `scale_x/scale_y/pad_x/pad_y`
和原始输出路径，`evaluate.py` 用它把框逆映射回原图坐标。

### 2. 导出 ONNX

```sh
python3 export_onnx.py --weights yolo26n.pt --imgsz 512 --out work/yolo26n-512.onnx
```

输入尺寸必须在导出时确定：8400→5376 个锚点是图里的常量，事后改输入形状会破坏
`Reshape`。

### 3. 量化成 RKNN

```sh
python3 build_rknn.py --onnx work/yolo26n-512.onnx --dataset work/letterbox512.txt \
                      --imgsz 512 --out work/yolo26n-512-box16.rknn
```

### 4. 板端 dump 原始输出

`dump_outputs.c` 在板子上跑（要用板载 `librknnrt`）：

```sh
gcc -O2 -o dump_outputs dump_outputs.c -I../../include/yolo -l:librknnrt.so
./dump_outputs yolo26n-512-box16.rknn work/v512-in work/v512-raw
```

它把两路输出拼回训练侧的 84×A 布局（行 0–3 框、行 4–83 类别分），
所以评测脚本不需要区分单输出/双输出。

### 5. 评测

```sh
python3 evaluate.py validation.tsv work/baseline-raw work/v512-raw report.json
```

`baseline` 传现有模型的 raw 目录，`candidate` 传新模型的，脚本会打印两者 mAP 与
一致性差异。

## 关键约束（都踩过）

| 约束 | 说明 |
|---|---|
| toolkit 锁 2.3.2 | 2.5.0 在 `_p_fuse_exmatmul_add_mul_exsoftmax13_exmatmul_to_sdpa` 触发 `IndexError: list index out of range`，640/512、hybrid/official 全部复现 |
| `proposal=False` | 该模型不支持 expand batch，proposal 会直接报错 |
| step2 不能调 `config()` | 否则报 `config of 'step2' is inconsistent with 'step1'` |
| 框必须 FP16 | 全 INT8 时框量化步长约 2.73 px（±1.4 px），mAP50-95 掉 2.45 个点；FP16 每帧仅多 33 KB，输出转换 0–1 ms |
| 类别用 NHWC | NCHW 下逐锚点读 80 个分数要跨 5376 字节跳读，后处理 4 ms；改 NHWC 后 1 ms |
| one2one 仍需 NMS | 关掉 NMS 在 conf≥0.60 时仍有 85.7% 重复框，mAP50 从 0.5238 掉到 0.3491 |

改输出的布局（如类别转置成 NHWC）是在 ONNX 里加 `Transpose` 节点实现的，见
`build_rknn.py` 的 `SCORES`/`BOXES` 常量；板端 `yolo_postprocess_two()` 会按张量
维度自动识别 NCHW/NHWC。

## 目录内容

| 文件 | 作用 |
|---|---|
| `export_onnx.py` | checkpoint → ONNX（尺寸可配） |
| `build_rknn.py` | ONNX → RKNN（双输出 + FP16 框） |
| `prepare_dataset.py` | 校准集 / 验证集 letterbox 与清单 |
| `dump_outputs.c` | 板端 dump 原始输出（float32，84×A） |
| `evaluate.py` | mAP 评测与一致性对比 |
| `requirements.txt` | 依赖版本（含 toolkit 版本锁定） |

## 部署

模型不纳入 Git（约 7.9 MB）。板端路径是 `config/yolo26.rknn`，由
`scripts/deploy.sh` 经 `scripts/model_sync.sh` 按哈希同步：

```sh
# 板子上：替换模型后重启服务
cp yolo26n-512-box16.rknn /opt/autonomous-driving-car/config/yolo26.rknn
systemctl restart autonomous-driving-car

# 或者用仓库流程（会做哈希比对，避免覆盖前端上传的模型）
bash scripts/deploy.sh -i
```

模型的量化细节、精度演进与全部实验记录见
[`docs/YOLO26N_QUANTIZATION.md`](../../docs/YOLO26N_QUANTIZATION.md)。
