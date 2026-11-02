# YOLO26n 混合量化与目标特征输出报告

日期：2026 年 11 月 2 日。硬件：RK3576，双核 NPU；相机配置：1280×720 MJPEG，期望 30 FPS。模型输入为 640×640 RGB。

## 结论

采用 UINT8 图像输入、W8A8 主干和 FP16 敏感检测头的混合方案。最终模型同时输出检测结果与 P4 特征图，后处理为每个检测目标生成 128 维单位向量。跟踪以模型向量和运动预测为主，不再计算 HSV、LBP 或梯度直方图。

| 项目 | 原 FP16 模型 | 混合量化＋特征输出模型 |
| --- | --- | --- |
| 文件大小 | 10,528,118 字节 | 8,447,185 字节，减少约 19.77% |
| 输入接口 | UINT8、NHWC | UINT8、NHWC |
| 实际模型输入 | FP16 | INT8，scale≈1/255，zero-point=-128 |
| 检测输出 | FP16 `[1,84,8400]` | FP16 `[1,84,8400]` |
| 特征输出 | 无 | INT8 `[1,128,40,40]`，SDK 解量化为浮点 |
| 每目标描述子 | CPU 图像直方图 | 模型 P4 ROI 池化，128 维、L2 归一化 |

UINT8 表示提交给 SDK 的图像缓冲类型；RK3576 内部采用 W8A8/INT8 路线。此方案保留部分浮点运算，不能称为全模型纯 UINT8 量化。

## 模型来源与构建参数

源权重来自 [Ultralytics 官方 YOLO26n](https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt)，SHA-256 为 `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`。已核验 nano 架构及原 FP16 模型权重对应关系，见[帧处理与原模型核验](FRAME_PIPELINE.md)。

使用 Toolkit2 2.3.2，目标平台 `rk3576`，`w8a8`、按通道权重量化、`normal` 校准算法、优化级别 3。保留自动混合精度建议，并将检测头及最终拼接链保留 FP16。运行库为 2.3.0，驱动为 0.9.8；已在实际板卡上验证初始化和推理。

按编译产物的实际权重张量统计，2,242,480 个权重元素使用 INT8，157,680 个使用 FP16，INT8 权重约占 93.43%。对应偏置分别采用 INT32 或 FP32。该比例按权重元素计算，不代表全部运算、激活或文件字节都使用 8 位。

原始检测输出保持 one-to-many、像素单位的 `cx,cy,w,h` 与 80 类概率，沿用工程现有阈值过滤和分类别 NMS。SDK 输入使用 `pass_through=0`，输出使用 `want_float=1`；不把 UINT8 像素直接当作有符号 INT8 数值。

参考：[Rockchip 转换示例](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/python/convert.py)、[Toolkit2 2.3.2 API](https://github.com/airockchip/rknn-toolkit2/blob/v2.3.2/doc/03_Rockchip_RKNPU_API_Reference_RKNN_Toolkit2_V2.3.2_EN.pdf)。

## 校准数据与验证范围

校准集固定为 96 张：64 张 COCO128 图像和 32 张未画框的实际相机帧。清单 SHA-256：`e8be446e3388dcdae65d9b22537b970c6e08c86c19cbeac39ab6adca99466790`。

独立验证使用另外 64 张 COCO128 图像，共 506 个标注、63 个有标注类别；另外保留 8 张新相机帧，只比较输出一致性。COCO 图像按数值 ID 排序后交错划分，校准和验证不重叠。官方压缩包中缺少标签的背景图按明确空标注处理，未修改原始压缩包。

预处理为 RGB、等比例线性缩放、整数居中、灰色 114 填充，输出 640×640；模型侧只施加一次 mean=0、std=255。64 张验证 PNG 已与工程原生 JPEG 解码和 letterbox 的结果逐像素核对，完全相同。

COCO128 来自 train2017 小子集。本报告的数值用于固定样本上的量化回归比较，不代表完整 COCO val2017 或真实驾驶场景的泛化精度。8 张相机图没有人工标注，不参与 mAP 计算。

## 精度对比

评估统一采用置信度下限 0.001、分类别 NMS IoU 0.7、最多 100 个检测、IoU 0.50–0.95、101 点 AP。读取原始浮点输出独立计算，避免生产后处理最多 32 个候选对评估造成额外截断。

| 指标 | FP16 | 混合量化＋特征输出 | 变化 |
| --- | ---: | ---: | ---: |
| mAP50 | 62.32% | 61.20% | -1.12 个百分点 |
| mAP50–95 | 47.18% | 46.38% | -0.80 个百分点 |

以 FP16 置信度≥0.5 的检测作一致性对照，188 个中有 179 个被量化模型按同类、IoU≥0.5 匹配，保留率约 95.21%；匹配框平均 IoU 为 0.9804，平均置信度绝对差约 0.0187。

新增 P4 输出没有改变已验证单输出量化模型的检测精度结果。72 张板端图像的 160 个生产检测均获得有效特征，向量平方范数范围约为 `0.999999953–1.000000046`。

## 板端耗时

在暂停在线服务后，对两模型串行运行同样的 72 张图像，固定主机 CPU4 和 NPU core0、一个上下文，首图预热两次。下表为均值，不包含 JPEG 解码、缩放、渲染或视频编码。

| 阶段 | FP16 | 混合量化＋特征输出 |
| --- | ---: | ---: |
| `rknn_inputs_set` | 16.265ms | 0.384ms |
| `rknn_run` | 45.749ms | 38.926ms |
| `rknn_outputs_get` | 2.724ms | 3.174ms |
| 检测后处理 | 0.705ms | 0.709ms |
| 每目标特征提取 | 0 | 0.119ms |
| 上述阶段合计 | 65.443ms | 43.312ms |

该测试中上述阶段合计缩短约 33.8%。额外特征图解量化与 ROI 提取增加约 0.6ms，相比减少的输入转换成本仍有净收益。`rknn_run` 是 API 墙钟耗时，包含设备等待，不能当作纯 NPU 计算时间。

频率、温控和输入画面会影响结果；此单上下文测试也不能直接替代双 NPU 自适应并发的整条视频链路测量。相机交付、软件 H.264 编码与浏览器播放仍有独立成本。

## 目标特征与运动学跟踪

模型增加 `/model.19/cv2/act/Mul_output_0` 输出，即 P4 `[1,128,40,40]`。实际特征输出为 INT8，scale≈0.0236290935、zero-point=-116；SDK 解量化后供 CPU 读取。

推理 worker 在释放 SDK 输出前，使用该帧实际 letterbox 比例和填充，将最终检测框映射到特征图；裁去 ROI 边缘 5%，按 4×4 双线性网格采样并平均，生成 128 维 L2 单位向量。向量按值嵌入检测结果，随帧号和目标索引一起传入队列，不保留 SDK 缓冲指针。

跟踪使用模型余弦相似度、恒速预测中心、预测 IoU、位移门控及尺寸变化。具有有效向量时，关联代价中模型相似度占主导；各 ID 保存固定参考、慢速更新的向量及平滑速度。用于显示的框 EMA 放在跟踪之后，避免干扰速度估计。

近邻特征门槛为 0.70。显式选中目标的远距重识别采用 0.995 门槛、歧义拒绝及连续两帧确认，最长保留 5 秒恢复窗口。无有效向量时仅允许近距离运动关联。模型重载时清空历史参考，避免跨模型比较不同特征基底。

这些是检测网络的中间特征，未经独立身份/ReID 训练。本次短场景同目标余弦最小约 0.9991，而同图不同同类检测的相似度也可能高达 0.9941，因此采用保守远距门槛；这不是通用身份识别精度保证。旧的 HSV/LBP/梯度描述子及其匹配门槛已移除。

`/api/tracking` 返回特征方法、维度、有效目标数及选中目标的完整向量。选择视觉目标不会开启跟随或电机。

## 结果文件格式

结果仍追加到 `recordings/detections/YYYYMMDD/result_YYYYMMDD.txt`，保留原有时间、数量、图像尺寸、焦距原始值、类别名称、置信度及位置字段。新增字段如下：

| 层级 | 新增字段 |
| --- | --- |
| 帧 | `seq`、`capture_ms`、`feature_method[yolo26n_p4_roi]`、`feature_dim[128]` |
| 目标 | `index`、`class`、`feature_valid`、`feature[128 个数值]` |

向量使用 9 位有效数字保存，保留 float32 数值精度。缺失、非有限或零范数向量记录为 `feature_valid[0] feature[]`，保留该帧及其他检测。数据按值进入有界队列，由已有写入线程批量落盘；相机和推理线程不直接写文件。单帧缓冲扩大至 128KiB，覆盖 32 个目标及完整向量的最长记录。旧文件保持原内容，后续记录追加新字段。

## 验证与复核资料

已完成特征模块 ASan/UBSan 检查、运动预测和特征关联检查、原生板端 72 张模型对照、工程 Release 编译，以及在线视觉选择、128 维向量范数、稳定 ID、视觉停止、相机重启、模型重载、视频和录像恢复检查。测试程序、模型转换依赖和相机样本均位于工程外。

本机证据目录：`/home/qianruo/kbuild/car-quant-u8/`。其中 `evaluation-final.json`、`fp16-results.jsonl`、`quant-results.jsonl`、`build/actual-quantization.json`、`build/features-candidate.json`、`live-tracking.json` 可复查对应结果。

| 模型 | SHA-256 |
| --- | --- |
| 原 FP16 | `d4465c45a49e3ae995dd2dbb5e9a5984424aba3268ece02f112647db04f6963a` |
| 最终混合量化＋特征输出 | `1525b0d27cb1e8eb07da8f506990a52ebf440d3cc2ff6a1bf11c42bda28ef7a9` |

构建时发现仅将检测头和公开输出设为 FP16 还不够：内部 `output0-rs` 和 `output0_int8` 拼接仍为 INT8，使大坐标与小概率共用刻度，导致概率丢失。该候选未部署；将完整拼接链保留浮点后重新编译，并重新通过全部板端精度检查。更换运行库和手工输入量化的诊断均未解决该问题，因此线上继续使用已验证的原运行库。

## 部署与回滚

运行路径继续使用 `config/yolo26.rknn`，由既有构建、部署脚本按哈希同步。模型和相机图像不纳入 Git；代码、报告和格式说明纳入版本管理。

原 FP16 模型备份位于 `/home/qianruo/kbuild/car-quant-u8/original-fp16.rknn`；首次切换前的模型、二进制、配置与同步基线保存在 `/home/qianruo/kbuild/car-quant-before-gUqtEH/`。回滚需一并保持源码目录、部署目录中的模型和同步基线一致，并重启服务使模型参考特征重新建立。

## 复现流程与关键配置

模型准备在隔离 Python 环境中完成，与 C/C++ 应用构建分开。本次使用 Python 3.12、Ultralytics 8.4.187、Torch 2.2.0、ONNX、ONNX Runtime、onnxslim 和 Toolkit2 2.3.2；应用不依赖这些 Python 包。

1. 下载并校验上述官方 checkpoint；将检测头设为 `end2end=False`、`export=True`、`format='onnx'`、`dynamic=False`，融合卷积与 BN。导出 opset12、静态批次 1 的 ONNX，再执行简化和模型检查。先核对输出为 `[1,84,8400]`、坐标单位为像素；本次黑图的 Torch/ONNX 最大绝对差为约 0.00226。
2. 准备与线上完全相同的 RGB letterbox PNG。按数值 ID 排序拆分官方 [COCO128 数据](https://github.com/ultralytics/assets/releases/download/v0.0.0/coco128.zip)，偶数位置用于校准，奇数位置用于独立验证；另加入实际相机帧。官方 ZIP SHA-256 为 `61e5e3028863d8ffc3b81d6a514603954889f0edd5e4b44c4ce60b2da99aeb8e`。
3. 使用以下参数调用 `config` 和 `load_onnx`，再执行 `hybrid_quantization_step1(dataset=..., proposal=True, proposal_dataset_size=1)`，保留生成的 `.model`、`.data` 和原始 `.quantization.cfg`。

```python
config = dict(
    target_platform="rk3576",
    mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
    quantized_dtype="w8a8", quantized_algorithm="normal",
    quantized_method="channel", float_dtype="float16",
    optimization_level=3,
)
```

4. 在自动建议基础上，把检测头的浮点激活、`output0`、`output0-rs`、`output0_int8` 设为 `float16`。不要把整数形状和坐标常量盲目加入 `custom_quantize_layers`；本次强制 61 个头部张量，加上自动建议和最终拼接链，共 67 项。
5. 通过 `hybrid_quantization_step2` 和 `export_rknn` 生成检测候选。第二步配置必须与第一步一致，连附加模型说明字符串也不能随意改变；生成模型后重新查询实际输入输出属性。
6. 先验证检测候选，再给缓存的优化模型增加已有 P4 张量作为第二输出，形状为 `[1,128,40,40]`。主干没有新增计算和权重，复用同一套校准参数重新编译即可，不需要再次执行完整校准。
7. 暂停在线推理，在相同图像、CPU 亲和性和 NPU 核上串行对照原 FP16 与候选。检查全部输出有限、坐标单位正确、概率在有效范围，分别计算检测精度和耗时，再测试特征的布局、归一化、帧索引、生命周期及跟踪行为。
8. 将通过验证的模型放到 `config/yolo26.rknn`，执行 `bash scripts/build.sh -R` 和 `sudo bash scripts/deploy.sh -i`。验证视频、录像、目标选择、模型重载及结果文件，再确认源码和部署目录中的模型哈希一致。

本次完整校准和混合误差分析约 23 分钟；后续编译约 30 秒。时间与环境有关，不能把某个校准子步骤的进度当成总任务完成比例。

## 排错记录

| 现象或尝试 | 实测结果与处理 |
| --- | --- |
| 只强制公开检测头与输出 FP16 | 置信度固定为约 0.000122，64 张 mAP 为零，拒绝部署 |
| 手工构造原生 INT8 输入 | 没有修复概率异常，排除简单输入类型转换原因 |
| 读取原生 FP16 输出，绕过 SDK 浮点转换 | 异常仍在，排除单纯输出转换原因 |
| 用独立目录中的官方运行库 2.3.2 对照 | 异常仍在，未因此升级系统运行库 |
| 检查编译图的中间转换与拼接 | 发现概率被转换成 INT8，与大坐标合并后丢失分辨率 |
| 强制完整拼接链 FP16，重新编译 | 置信度恢复，再次通过全部板端精度与特征检查 |
| 32 个目标、长名称和极小特征分量 | 最长测试记录 87,095 字节，128KiB 缓冲完整容纳，向量逐值回读一致 |

## 资料索引

| 本机资料 | 内容 |
| --- | --- |
| `car-yolo26n/provenance.md`、`comparison.json` | 官方 nano 架构与原模型权重身份核验 |
| `car-yolo26n/quantization-research.md` | 8 位方案、工具版本和官方输出兼容性调研 |
| `car-quant-u8/data/README.md`、`manifest.json` | 数据来源、哈希、拆分、缺失标签及预处理说明 |
| `car-quant-u8/data/calibration.json`、`validation.json` | 固定样本清单、几何变换和每张图的哈希 |
| `car-quant-u8/build/export.py`、`export.json` | ONNX 导出程序与数值验证 |
| `car-quant-u8/build/quantize-step1.py`、`quantize-step2.py` | 校准、误差建议与混合编译 |
| `car-quant-u8/build/build-features.py` | 从缓存模型增加 P4 输出并重新编译 |
| `car-quant-u8/build/forced-fp16-head.json`、`actual-quantization.json` | 浮点保留清单与编译产物实际类型统计 |
| `car-quant-u8/validation/infer.cpp`、`evaluate_raw.py` | 复用应用预处理的板端验证与独立精度计算 |
| `car-quant-u8/evaluation-final.json`、`feature-similarity.json` | 最终精度、特征相似度与适用限制 |
| `car-quant-u8/feature-tests/` | 特征、跟踪、TXT 完整性、队列和导出验证 |
| `car-quant-u8/deploy.log`、`live-tracking.json` | 部署核验与在线目标选择、向量、ID 对照 |

上表路径统一位于 `/home/qianruo/kbuild/`。它们包含设备数据和临时分析工具，未整体提交 Git；本文件归纳参数、步骤、结论和哈希，工程源码提供实际运行实现。正式应用的编译与部署脚本仍只消费已准备好的 RKNN 文件。
