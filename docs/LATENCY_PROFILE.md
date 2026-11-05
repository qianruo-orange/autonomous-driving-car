# 推理流水线延迟剖析

本文记录 2026-11-05 的延迟优化过程、每一项的实测结论，以及当前基线。所有数字来自板端日志与 `/api/video/stats`，不是估算。

## 1. 当前基线

```
端到端 processing_age : 46 ms（stdev 0.5，min 46 / max 47）
采集                   : 1280×720 MJPEG 30 fps，丢帧 0
模型                   : YOLO26n 512×512，INT8 类别 + FP16 框
NPU                    : 1188 MHz，Core0/Core1 各一个 worker
CPU                    : performance 调速器，2016 / 2208 MHz
```

46 ms 的构成：

| 部分 | 耗时 | 说明 |
|---|---:|---|
| 相机侧 | 33 ms | 驱动时间戳 → DQBUF 返回，UVC 固有的一帧延迟 |
| 我们的流水线 | 13 ms | decode 1 + resize 2 + run 8 + post 1 + 调度 1 |

## 2. 测量方法

`yolo timing ms:` 每 10 秒一行：

| 字段 | 含义 |
|---|---|
| `capture_to_decode` | 帧时间戳 → 开始解码（含相机侧延迟） |
| `deliver_to_decode` | DQBUF 返回 → 开始解码（应用自身排队） |
| `decode` | JPEG 硬解 + NV12→RGB |
| `resize` | letterbox 缩放到模型输入 |
| `inputs` / `run` / `outputs` | NPU 三段 |
| `post` | 候选筛选 + NMS |

另有 `yolo handoff ms:` 记录"后处理完成 → 显示提交"，实测恒为 0。

`deliver_to_decode = 0` 是关键证据：帧一到就开始处理，`capture_to_decode` 的 33 ms 全部发生在驱动/相机侧。

## 3. 有效的优化

| 改动 | 收益 | 说明 |
|---|---:|---|
| **CPU 调速器 ondemand → performance** | **−14 ms** | 最大单项。帧间隔内降频使所有 CPU 环节变慢且抖动，E2E 60±7 → 46±0.5 |
| NPU 800 → 1188 MHz | −13 ms（run 34.8→12） | 见内核仓 `0005` |
| 模型 640 → 512 输入 | −7 ms（run 12→8） | 精度同时提升，见量化报告 17 |
| 采集 2560×1440 → 1280×720 | −5 ms | 此前一直误用 1440p |
| 运行时 2.3.0 → 2.5.0 | 同模型 −25% | 官方 notes 的多输入输出 CPU 负载优化 |
| 类别输出 NCHW → NHWC | −2 ms（post 4→1） | 逐锚点读 80 个分数由跨 5376 字节变连续 |
| 框输出 FP16 | 精度 +2.45 mAP50-95 | 延迟无变化，见量化报告 16 |

## 4. 验证后否定的方案

| 方案 | 结论 |
|---|---|
| 4 个推理线程 | E2E 62→63、`run` 12→13：NPU 只有 2 核，多开在核内串行 |
| 解码器超频 702→1000 MHz | 解码无变化（7.18→7.71 ms），且有 30–50 ms 偶发长解码 |
| 编码器超频 | 应用不编码，无影响 |
| NV12→letterbox 融合（标量） | 8.17 ms，慢于现有"转换 1.11 + resize 1.11" |
| NMS 候选池 128→64 | post 无变化：耗时在逐锚点类别扫描而非候选池 |
| RGA 硬件缩放 | 硬件在、时钟在跑，但无驱动；移植厂商 rga3 需数天换约 3 ms |

## 5. 剩余空间

| 方向 | 预估 | 代价 |
|---|---:|---|
| 采集再降到 640×480 | −2 ms（decode 5→1） | 录像 720p→480p |
| NEON 融合 NV12→letterbox | −2~3 ms | 需写 NEON 内核 |
| RGA 移植 | −2 ms | 驱动移植，数天 |
| 相机换 CSI 直连 | 可能削掉 33 ms 的一部分 | 硬件改动 |

应用侧已接近该架构下限（13 ms），继续压缩只能动相机链路或硬件加速器。

## 6. 复现

```bash
# 调速器（仓库内 config/cpu_freq.conf，应用启动时恢复）
cat /sys/devices/system/cpu/cpufreq/policy4/scaling_governor   # performance
# 阶段耗时
grep "yolo timing" /opt/autonomous-driving-car/logs/*/*.log | tail
# 端到端
curl -s http://127.0.0.1/api/video/stats | grep -o '"processing_age_ms":[0-9]*'
```
