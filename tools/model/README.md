# 模型转换与验证（离线说明）

本目录记录 YOLO26n 从 checkpoint 到板端 `.rknn` 的离线链路。仓库里只保留**板端验证工具**
（`dump_outputs.c`）；转换本身在固定容器内用官方工具链完成，不在本仓库里携带任何 Python
源码——这是本工程「不含解释器依赖」的一部分，理由见
[`docs/AGENT_HARDWARE.md`](../../docs/AGENT_HARDWARE.md) §6。

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
   │ 1. 导出 512 ONNX（84×5376 单张量）
   ▼
yolo26n-512.onnx
   │ 2. 截断双输出 + 框走 FP16 + 类别转 NHWC
   ▼
yolo26n-512-box16.rknn  ──►  config/yolo26.rknn  ──►  部署
```

### 1. 为什么转换在容器里

`rknn-toolkit2` 只提供 Python 包，且 2.3.2 与 2.5.0 的行为差异直接决定本模型能否转换成功
（见下方约束表）。把它引入仓库等于引入一个必须与系统 Python 隔离的环境；固定镜像把这个问题
关在容器里，仓库只需要记录**镜像标签、脚本版本与产物哈希**三件事：

```sh
# 固定工具链镜像（标签与 python/convert.py 的 commit 一并记录在发布说明里）
docker run --rm -it -v "$PWD:/work" -w /work \
    rockchip/rknn-toolkit2:2.3.2-py38 bash
```

容器内只做三件事：`load_onnx(outputs=['/model.23/Sigmoid_output_0','/model.23/Mul_2_output_0'])`
截断成两路、`build(do_quantization=True, dataset=校准集)` 量化、`export_rknn()` 导出。
完整参数、失败记录与逐步实验见 [`docs/YOLO26N_QUANTIZATION.md`](../../docs/YOLO26N_QUANTIZATION.md)。

### 2. 校准集与验证集

两者必须使用**同一套** letterbox 参数（512、114 灰边、等比缩放），否则量化统计与精度评测不在
同一个坐标系里，mAP 会虚高。清单是纯文本，一行一张图：

```text
# calibration.txt / validation.tsv
datasets/train/0001.jpg
datasets/train/0002.jpg
```

验证集清单额外带 `id<TAB>宽<TAB>高<TAB>标签路径`，用于把框逆映射回原图坐标。

### 3. 板端 dump 原始输出

`dump_outputs.c` 在板子上编译（要链接板载 `librknnrt`）：

```sh
gcc -O2 -o dump_outputs dump_outputs.c -I../../include/yolo -l:librknnrt.so
./dump_outputs yolo26n-512-box16.rknn work/v512-in work/v512-raw
```

它把两路输出拼回训练侧的 84×A 布局（行 0–3 框、行 4–83 类别分），所以评测脚本不需要区分
单输出/双输出。dump 出来的 float32 张量就是精度比对的唯一依据，评测在仓库外进行。

## 关键约束（都踩过）

| 约束 | 说明 |
|---|---|
| toolkit 锁 2.3.2 | 2.5.0 在 `_p_fuse_exmatmul_add_mul_exsoftmax13_exmatmul_to_sdpa` 触发 `IndexError: list index out of range`，640/512、hybrid/official 全部复现 |
| `proposal=False` | 该模型不支持 expand batch，proposal 会直接报错 |
| step2 不能重设 config | 否则报 `config of 'step2' is inconsistent with 'step1'` |
| 框必须 FP16 | 全 INT8 时框量化步长约 2.73 px（±1.4 px），mAP50-95 掉 2.45 个点；FP16 每帧仅多 33 KB，输出转换 0–1 ms |
| 类别用 NHWC | NCHW 下逐锚点读 80 个分数要跨 5376 字节跳读，后处理 4 ms；改 NHWC 后 1 ms |
| one2one 仍需 NMS | 关掉 NMS 在 conf≥0.60 时仍有 85.7% 重复框，mAP50 从 0.5238 掉到 0.3491 |

改输出布局（如类别转置成 NHWC）是在 ONNX 里加 `Transpose` 节点实现的；板端
`yolo_postprocess_two()` 会按张量维度自动识别 NCHW/NHWC，两种布局都能跑，只有延迟不同。

## 目录内容

| 文件 | 作用 |
|---|---|
| `dump_outputs.c` | 板端 dump 原始输出（float32，84×A），精度比对的输入 |
| `README.md` | 本文：链路、约束、部署路径 |

## 部署

模型不纳入 Git（约 7.9 MB）。板端路径是 `config/yolo26.rknn`，由 `scripts/deploy.sh` 经
`scripts/model_sync.sh` 按哈希同步：

```sh
# 板子上：替换模型后重启服务
cp yolo26n-512-box16.rknn /opt/intelligent-robot/config/yolo26.rknn
systemctl restart intelligent-robot

# 或者用仓库流程（会做哈希比对，避免覆盖前端上传的模型）
bash scripts/deploy.sh -i
```
