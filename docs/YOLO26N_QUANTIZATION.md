# YOLO26n UINT8 输入与 W8A8 混合量化报告

> **2026-11-05 更新**：交付模型已切换为 YOLO26 one2one 双输出全 INT8 方案，单输出混合量化方案转为回滚路径。迁移依据、实测数据与未完成项见第 14 节。

报告日期：2026 年 11 月 2 日（Asia/Shanghai）。设备：Radxa ROCK 4D / RK3576。应用：autonomous-driving-car。

本文记录本次已完成的模型转换、校准、板端精度验证与问题定位，并说明去掉模型特征输出后的交付方案。数据来自本机固定验证集和保存的转换产物；不同阶段的模型和测试结果分别标明。

## 1. 交付结论

采用 UINT8 RGB 图像接口、INT8/W8A8 主干与 FP16 敏感检测头的混合量化方案。交付模型仅有一个检测输出 `output0`，形状 `[1,84,8400]`。按最新要求去掉 P4 特征输出、每目标 128 维向量、特征相似度关联及结果文件中的特征数据；目标跟踪保留检测框和运动学关联。

| 项目 | 原 FP16 模型 | 单输出混合量化模型 |
| --- | --- | --- |
| 文件大小 | 10,528,118 字节 | 8,446,083 字节，减少约 19.78% |
| SDK 图像输入 | UINT8、NHWC | UINT8、NHWC |
| 查询到的模型输入 | FP16 `[1,640,640,3]` | INT8 `[1,640,640,3]` |
| 输入量化参数 | 不适用 | scale=0.00392156886、zero-point=-128 |
| 检测输出 | FP16 `[1,84,8400]` | FP16 `[1,84,8400]` |
| 模型输出数量 | 1 | 1 |
| 模型特征输出 | 无 | 无，P4 实验输出已移除 |
| 固定验证集 mAP50 | 62.32% | 61.20% |
| 固定验证集 mAP50–95 | 47.18% | 46.38% |

这里的“u8 方案”指应用提交的 UINT8 图像缓冲。RK3576 内部使用 INT8/W8A8 路线，并保留部分 FP16 运算，不宜称为全模型纯 UINT8 量化。混合模型相对基线的 mAP50 与 mAP50–95 分别降低约 1.12 和 0.80 个百分点；数据范围和限制见下文。

## 2. 模型身份与工具环境

源权重来自 [Ultralytics 官方 YOLO26n checkpoint](https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt)，已核验 nano 架构以及原 FP16 模型的权重对应关系，详见同目录的 [FRAME_PIPELINE.md](FRAME_PIPELINE.md)。原 checkpoint 为 5,544,453 字节。

| 构件 | SHA-256 |
| --- | --- |
| 官方 `yolo26n.pt` | `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef` |
| 导出的 `yolo26n-pixel.onnx` | `6de676ce6144fbc6696f8d90e2f089d0718b54c5a09676bb7b3a01aaed6c93c2` |
| 原 FP16 RKNN | `d4465c45a49e3ae995dd2dbb5e9a5984424aba3268ece02f112647db04f6963a` |
| 交付的单输出混合 RKNN | `00c1c7c1a67a3a34dca32738267aa827eccd285a5023f174cdb07ac739491cfd` |

| 环境项目 | 本次使用值 |
| --- | --- |
| 模型准备 | Python 3.12、Ultralytics 8.4.187、Torch 2.2.0、ONNX、ONNX Runtime、onnxslim |
| 模型编译 | RKNN-Toolkit2 2.3.2，目标平台 `rk3576` |
| 板端运行库 | RKNN Runtime 2.3.0，构建标识 `c949ad889d@2024-11-07T11:35:33` |
| 板端 NPU 驱动 | 0.9.8 |
| 相机配置 | 1280×720 MJPEG，配置目标 30 FPS |
| 模型输入 | 640×640 RGB，静态 batch=1 |

模型准备使用隔离 Python 环境，正式 C/C++ 应用不依赖这些 Python 包。曾在独立目录对照官方 Runtime 2.3.2，用于排查量化输出问题；没有因此替换系统的 Runtime 2.3.0。模型能否运行以本次板端实测为准，不能据此推定任意 Toolkit、Runtime、驱动组合兼容。

## 3. 量化策略与实际数据类型

配置为 `w8a8`、按通道权重量化、`normal` 校准算法、优化级别 3。先保留自动混合精度建议，再把检测头的敏感浮点张量与完整输出拼接链设为 FP16。强制头部张量 61 项，叠加自动建议与拼接链后，`custom_quantize_layers` 共 67 项。

```python
configuration = dict(
    target_platform="rk3576",
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    quantized_dtype="w8a8",
    quantized_algorithm="normal",
    quantized_method="channel",
    float_dtype="float16",
    optimization_level=3,
)
```

编译产物的实际参数统计如下。INT8 权重比例按权重元素数计算，不代表算子数、激活数或模型文件字节占比。

| 参数类别 | 张量数 | 元素数 | 数据类型 |
| --- | ---: | ---: | --- |
| 量化卷积权重 | 76 | 2,242,480 | INT8 |
| 量化卷积偏置 | 76 | 7,192 | INT32 |
| 保留浮点的权重 | 26 | 157,680 | FP16 |
| 保留浮点的偏置 | 26 | 1,580 | FP32 |

INT8 权重占比为 `2,242,480 / (2,242,480 + 157,680) ≈ 93.43%`。统计保存在 `build/actual-quantization.json`。

应用向 SDK 传入 UINT8、NHWC 图像，`pass_through=0`；让 SDK 依据模型输入属性完成转换。模型原生输入为 INT8、zero-point=-128、scale≈1/255。应用不能直接把无符号像素缓冲当作有符号 INT8，也不能在 CPU 端先归一化后再重复施加 `std=255`。检测输出设置 `want_float=1`，由 SDK 将 FP16 输出转换为工程后处理使用的 float。

## 4. 校准数据、独立验证与预处理

校准集共 96 张，包含 64 张 COCO128 图像和 32 张实际相机原始帧。相机帧来自未画框的 `/video/mjpeg`，接收样本间隔至少 0.5 秒，并通过 64×64 RGB 缩略图的最小平均绝对差筛选以减少重复。该接收间隔不等同于物理曝光时间间隔。

独立验证包含另外 64 张 COCO128 图像，共 506 个标注、63 个有标注类别；另留 8 张新相机帧，用于输出一致性检查，不参与 mAP。COCO 图像按数值 ID 排序后，零起始偶数位置用于校准，奇数位置用于验证，两集合不重叠。

| 数据构件 | 数量或校验信息 |
| --- | --- |
| 校准图像 | 96＝64 张 COCO＋32 张相机帧 |
| 独立精度验证 | 64 张 COCO，506 个标注，63 类 |
| 相机一致性验证 | 8 张，无人工标注 |
| `calibration.txt` SHA-256 | `e8be446e3388dcdae65d9b22537b970c6e08c86c19cbeac39ab6adca99466790` |
| 官方 COCO128 ZIP SHA-256 | `61e5e3028863d8ffc3b81d6a514603954889f0edd5e4b44c4ce60b2da99aeb8e` |

原始数据来自 [Ultralytics 官方 COCO128 压缩包](https://github.com/ultralytics/assets/releases/download/v0.0.0/coco128.zip)。官方包缺少 ID `000000000250` 和 `000000000508` 的对应标签，另有两个没有对应图片的标签；本次把缺失标签图明确作为空标注背景图，单独保存合成的空标签和处理记录，不修改官方原始文件。

预处理逐步与工程保持一致：JPEG 解码为 RGB；计算 float32 比例 `min(640/w,640/h)`；缩放尺寸按 `int(dimension*scale+0.5f)` 得到；采用 OpenCV `INTER_LINEAR`；整数居中，空白填 RGB 114；保留 UINT8 0–255，模型配置只施加一次 mean=0/std=255。64 张验证 PNG 已与应用原生解码和 letterbox 结果逐像素核对，完全一致。

COCO128 属于 COCO train2017 的小子集。上述 mAP 用于固定样本上的量化回归比较，不代表完整 COCO val2017 成绩或实际驾驶场景的泛化能力。8 张相机帧缺少人工标注，不能用于声称真实场景检测精度。

## 5. 模型导出与输出约定

本次保留工程原有 one-to-many 检测接口。导出时检测头设为 `end2end=False`、`export=True`、`format='onnx'`、`dynamic=False`，融合卷积与 BN，导出 opset12、静态 batch=1，再简化并进行 ONNX 模型检查。黑图上 Torch 与 ONNX 的最大绝对差约为 0.0022583。

单输出 `[1,84,8400]` 包括像素单位的 `cx,cy,w,h` 和 80 类概率，不附加独立 objectness。生产后处理沿用阈值过滤、分类别 NMS、逆 letterbox 和最多 32 个检测。评估程序则读取完整原始输出，单独使用评估阈值，避免生产截断影响精度对比。

官方导出器也提供 INT8 和混合精度支持，但当前 [Ultralytics RKNN 导出实现](https://github.com/ultralytics/ultralytics/blob/main/ultralytics/utils/export/rknn.py) 可在检测头前截断 NPU 图并把浮点解码尾部放到主机。本次交付保留工程已经验证的单个、已解码检测输出，采用本次固定转换脚本和模型哈希复现，不能直接把任意新版默认导出物覆盖到线上。

## 6. 精度验证结果

评估统一采用置信度下限 0.001、分类别 NMS IoU=0.7、最多 100 个检测、IoU 0.50–0.95、101 点 AP。独立 NumPy 评估器读取原始浮点输出，并在模型坐标系先执行 NMS，再逆变换和裁剪。评估器以小型人工构造例验证过匹配和积分行为。

| 指标 | FP16 基线 | 单输出混合量化 | 变化 |
| --- | ---: | ---: | ---: |
| mAP50 | 62.3239% | 61.2043% | -1.1196 个百分点 |
| mAP50–95 | 47.1768% | 46.3817% | -0.7951 个百分点 |
| 高置信度检测数 | 188 | 182 | 同一对照阈值 |
| 同类、IoU≥0.5 匹配数 | 188 个作参照 | 179 | 保留约 95.21% |
| 匹配框平均 IoU | — | 0.98037 | 相对 FP16 输出 |
| 匹配置信度平均绝对差 | — | 0.01873 | 相对 FP16 输出 |
| 匹配置信度绝对差 P95 | — | 0.06855 | 相对 FP16 输出 |

本次重新评估的精度证据为 `car-feature-removal/evaluation.json`；与早期单输出候选的 `car-quant-u8/evaluation-fixed.json` 完全一致，均对应已经修复输出拼接链的单输出混合模型。上述 188/182 是独立评估器在置信度≥0.5 下的一致性统计；它们与生产后处理最多 32 个检测的计数口径不同。72 张生产后处理样本中，FP16 与量化模型分别得到 168 和 160 个检测。

## 7. 板端性能与测量口径

本次移除特征后，重新暂停在线推理，在同样的 72 张图像上串行测试 FP16 与单输出混合模型，固定 CPU4、NPU core0、一个上下文，首图预热两次，每图计时一次。两份逐图原始记录均保存在新的 `car-feature-removal/` 目录，未覆盖此前资料；查询确认两模型都只有一个 FP16 检测输出。

| 阶段 | FP16 均值 | 单输出混合量化均值 |
| --- | ---: | ---: |
| `rknn_inputs_set` | 16.044ms | 0.381ms |
| `rknn_run` | 50.417ms | 46.554ms |
| `rknn_outputs_get` | 2.700ms | 2.703ms |
| 检测后处理 | 0.696ms | 0.720ms |
| 上述四阶段合计 | 69.856ms | 50.358ms |
| JPEG 解码，单独列出 | 2.766ms | 2.752ms |
| letterbox，单独列出 | 1.055ms | 1.063ms |

上述四阶段合计均值缩短约 **27.91%**。合计时间按每图四阶段相加后统计：中位数为 68.910→47.127ms，P95 为 71.632→49.506ms。P95 不等于各阶段 P95 相加；JPEG、缩放、渲染、视频编码和浏览器播放均未计入上述四阶段合计。

全部 72 个样本均纳入统计，没有剔除尖峰。FP16 的 `rknn_run` 最大约 125.32ms；混合模型有约 209.97 和 102.82ms 的两个尖峰。因此混合模型的 `run` 均值高于其 P95，这并非计算错误，也说明均值提升不能解读成每帧都更快或不存在尾延迟。

`rknn_run` 是包含设备等待的 API 墙钟时间，不能当作纯 NPU 计算时间。板卡温度、频率和其他系统活动会影响耗时，本次没有把一次 72 图串行测试当作长期尾延迟保证。单上下文结果也不能直接推算双 NPU 自适应并发吞吐，或摄像头曝光到屏幕显示的端到端延迟。

本节只使用本次单输出模型数据。历史带 P4 的第二轮测试仍保留在 `car-quant-u8/`，其特征图解量化和 ROI 提取成本不混入本节主表。可复核的新记录为 `car-feature-removal/fp16-results.jsonl`、`quant-results.jsonl` 和 `report-timings.json`。

## 8. 特征输出实验与本次移除

此前曾给缓存模型增加 P4 张量 `/model.19/cv2/act/Mul_output_0`，形状 `[1,128,40,40]`、INT8。其 128 来自该网络层的通道数；后处理把每个检测框映射到 P4，在裁去边缘 5% 的 ROI 内进行 4×4 双线性采样，逐通道平均并做 L2 归一化，得到每目标 128 维向量。它不是新增训练得到的独立身份识别层。

历史带 P4 模型为 8,447,185 字节，SHA-256 为 `1525b0d27cb1e8eb07da8f506990a52ebf440d3cc2ff6a1bf11c42bda28ef7a9`。增加已有特征输出没有改变固定验证集的检测精度，但中间特征未经身份/ReID 监督，同类不同目标也可能产生很高余弦相似度，不能当作通用身份保证。

按本次要求，交付重新采用已经过板端验证的单输出混合模型，不需要重新校准主干。应用同步移除特征图读取、ROI 向量、相似度匹配、API 向量和新增结果中的特征字段。跟踪保留目标类别、框位置/尺寸、恒速预测、预测框 IoU、中心位移门控和目标 ID；长时遮挡后没有外观重识别能力。

历史实验的 `feature-similarity.json`、`evaluation-final.json`、`feature-tests/`、旧结果文件仍作为历史资料保存，不代表当前应用仍输出特征。旧日报中的已有特征内容不追溯删除。

## 9. 应用接入与结果文件

运行路径为 `config/yolo26.rknn`，由现有构建和部署脚本按哈希同步。模型、校准图像和相机样本不纳入 Git，代码、中文报告和 PDF 纳入版本管理。

去掉特征后，新结果仍追加到 `recordings/detections/YYYYMMDD/result_YYYYMMDD.txt`。保留时间、数量、图像尺寸、焦距原始值、帧号 `seq`、`capture_ms`、目标索引 `index`、类别编号 `class`、类别名、置信度及坐标；不再生成 `feature_method`、`feature_dim`、`feature_valid` 和 `feature[...]`。

推理结果按值进入有界队列，由已有报告线程批量落盘，相机和推理线程不直接写文件。跟踪使用原始检测框估计速度，显示用的平滑放在跟踪之后。模型重载清空跟踪历史，避免不同运行会话之间错误沿用目标状态。视觉目标选择不会开启车辆跟随或电机。

## 10. 可复现流程

1. 校验官方权重哈希，按第 5 节导出并核对 ONNX 输出形状、像素坐标约定及 Torch/ONNX 数值误差。
2. 按第 4 节固定校准和验证清单；保留来源、图像哈希、缩放填充参数和缺失标签说明。对照应用预处理，不对输入重复归一化。
3. 使用第 3 节参数调用 `config`、`load_onnx`，执行 `hybrid_quantization_step1(dataset=..., proposal=True, proposal_dataset_size=1)`。保留 `.model`、`.data` 和原始 `.quantization.cfg`。
4. 在自动建议基础上，把 `/model.23/` 检测头中可量化的浮点激活及 `output0`、`output0-rs`、`output0_int8` 设为 `float16`。整数形状张量和常量不得盲目列入自定义浮点层。本次共保留 67 项。
5. 用与第一步完全一致的配置执行 `hybrid_quantization_step2`，再 `export_rknn`。配置中的 `custom_string` 也保持一致；实际完整配置在 `build/quantization-config.json`，不可只复制部分参数后混用旧缓存。
6. 查询实际输入、输出数量、形状、类型、量化参数；确保只有一个 FP16 检测输出。检查原始输出全部有限、概率范围和像素坐标约定正确，再进行固定集精度和性能对照。
7. 验证通过后，把模型放到工程 `config/yolo26.rknn`，运行以下项目脚本；部署前保留旧模型、二进制、网页、配置和模型同步基线。

```bash
bash scripts/build.sh -R
sudo bash scripts/deploy.sh -i
```

8. 核对源码与部署模型的 SHA-256、服务运行版本、相机/显示/编码计数递增、网页视频、录像、目标选择/停止、模型重载与结果文件格式。没有重新验证的现场功能不据此声称通过。

完整校准及混合误差分析本次约 23 分钟；固定配置下第二步编译实测约 31.6 秒。时间取决于环境，某个校准子阶段的完成比例不能代表整个任务完成比例。

## 11. 关键故障与处理依据

| 现象或尝试 | 实测结果与处理 |
| --- | --- |
| 只把公开检测头和输出保留 FP16 | 概率固定约 0.000122，64 张验证 mAP 为零，候选拒绝部署 |
| 手工构造原生 INT8 输入 | 未修复异常，排除简单输入类型转换原因 |
| 绕过 SDK，读取原生 FP16 输出 | 异常仍在，排除单纯输出浮点转换原因 |
| 独立加载官方 Runtime 2.3.2 | 异常仍在，系统运行库保持 2.3.0 |
| 检查中间转换和最终拼接 | `output0-rs`、`output0_int8` 仍走 INT8，大坐标与小概率共用刻度，概率分辨率丢失 |
| 将完整拼接链保留 FP16 | 概率恢复，重新通过全部固定集板端精度对照 |
| 盲目把形状/常量转浮点 | `Constant_12_output_0` 等操作数不合法；仅筛选可量化浮点激活 |
| 第二步修改附加说明字符串 | 与缓存配置不一致导致构建中止；恢复第一步完整配置后通过 |

最终方案保留了解码和输出的数值范围，降低了主干及输入转换成本。未采用输出概率已经损坏的“更纯整数”候选。

## 12. 部署核验与回滚要求

本次移除特征后已通过项目脚本 Release 编译并部署。核验记录确认模型哈希为本报告的单输出混合模型；相机配置保持 1280×720，采样间隔内采集、显示与编码计数各增加 40，录像持续写入；跟踪 API 不再返回模型特征或外观对象，全部实际工作线程保持 FIFO，车辆跟随关闭。新一轮 72 图推理与 64 图精度评估也已完成，完整证据保存在 `car-feature-removal/`。该上线采样中没有检测目标，不把它作为真实目标关联或长时跟踪的精度证明。

回滚时一并恢复模型、对应应用二进制、网页、配置与 `config/.last-deployed.sha256` 同步基线；保持源码目录和部署目录的模型一致，再重启服务。原 FP16 模型保留在 `/home/qianruo/kbuild/car-quant-u8/original-fp16.rknn`，首次切换前的完整备份位于 `/home/qianruo/kbuild/car-quant-before-gUqtEH/`，本次移除特征前的完整备份位于 `/home/qianruo/kbuild/car-quant-before-sUFfJS/`。单输出混合模型的源产物位于 `car-quant-u8/build/yolo26n-w8a8-mixed.rknn`。

不启动车辆电机来验证视觉修改。现场 CAN 总线、电机闭环、完整 COCO 验证、长时真实道路与跨摄像头身份识别均不在本次量化验证范围内。

## 13. 证据与官方资料索引

下表本机路径统一以 `/home/qianruo/kbuild/` 为根。这些资料包含设备数据、模型转换依赖和临时工具，未整体提交 Git；报告保存可复核的关键参数、口径与哈希。

| 本机资料 | 内容 |
| --- | --- |
| `car-yolo26n/provenance.md`、`comparison.json` | 原权重身份、nano 架构和 FP16 权重对应核验 |
| `car-quant-u8/data/README.md`、`manifest.json`、`checks.json` | 来源、拆分、标签策略、预处理与校验 |
| `car-quant-u8/data/calibration.json`、`validation.json` | 图像清单、哈希、尺寸和几何变换 |
| `car-quant-u8/build/export.py`、`export.json` | ONNX 导出及数值检查 |
| `car-quant-u8/build/quantize-step1.py`、`quantize-step2.py` | 校准、混合误差建议和第二步编译 |
| `car-quant-u8/build/quantization-config.json` | 两步共享的完整配置 |
| `car-quant-u8/build/forced-fp16-head.json`、`actual-quantization.json` | 浮点保留列表与实际类型统计 |
| `car-quant-u8/build/candidate.json` | 单输出候选哈希、大小和编译时间 |
| `car-quant-u8/validation/infer.cpp`、`evaluate_raw.py` | 原生板端验证与独立精度评估 |
| `car-quant-u8/evaluation-fixed.json`、`hardware-validation.log` | 单输出混合模型的精度及运行证据 |
| `car-feature-removal/fp16-results.jsonl`、`quant-results.jsonl` | 本次移除特征后，单输出模型逐图结果和分阶段耗时 |
| `car-feature-removal/report-timings.json` | 本次 72 图计时的均值与 P95 汇总 |
| `car-feature-removal/evaluation.json` | 本次重新完成的 64 图精度评估，与早期正确候选结果一致 |
| `car-feature-removal/deploy.log`、`deploy-verification.json` | 单输出部署、帧计数、录像、API 与实时线程核验 |
| `car-quant-u8/evaluation-final.json`、`feature-similarity.json` | 历史带特征输出实验，已从交付移除 |

官方参考：

- [Rockchip RKNN-Toolkit2 v2.3.2](https://github.com/airockchip/rknn-toolkit2/tree/v2.3.2)：本次使用的工具版本与平台支持。
- [Rockchip 模型转换示例](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/python/convert.py)：RK3576 使用 i8 路线及 mean/std 配置的示例。它是参考流程，不等同于本项目的完整混合方案。
- [Ultralytics RKNN 导出源码](https://github.com/ultralytics/ultralytics/blob/main/ultralytics/utils/export/rknn.py)：官方 INT8/浮点导出及检测解码尾部的组织方式。检索日期为 2026 年 11 月 2 日；当前主分支可能继续变化，复现本次结果应优先使用固定版本和哈希。

## 14. 2026-11-05：迁移到 one2one 双输出（全 INT8）

### 14.1 单输出方案的边界

`output0` 同时承载框坐标（校准集最大约 695）与类别概率（0–1）。INT8 仅 256 级，一组仿射参数覆盖 695 的跨度后每级约 2.72，0–1 的置信度会被压平，因此检测头必须保留 FP16。交付模型的 INT8 权重占比为 93.4%，检测头 61 层仍是 FP16，并非整网量化。

### 14.2 官方截断方案

Rockchip `rknn_model_zoo` v2.5.0 的 `examples/yolo26` 为 RK3576 给出另一条路径：按张量名截断 one2one 头，输出两支。

| 项目 | 取值 |
|---|---|
| 导出 | `yolo export model=yolo26n.pt format=onnx`（默认 one2one 头） |
| 截断张量 | `/model.23/Sigmoid_output_0`、`/model.23/Mul_2_output_0` |
| 转换 | `rknn.load_onnx(model=..., outputs=[...])` + `rknn.build(do_quantization=True, dataset=...)` |
| 输入 | `(1,640,640,3)` INT8，scale=0.00392156886，zp=-128 |
| 输出 0 | `(1,80,8400)` **INT8**，类别分数（已 sigmoid） |
| 输出 1 | `(1,4,8400)` **INT8**，框 x1,y1,x2,y2（模型输入像素） |
| 交付哈希 | `29cab9860993948e8636494f974ef0c4338cc4cd8030f49e2b3ada3d5875619b` |

两支各自独立量化，类别分支不再与像素尺度共享 scale，检测头得以全 INT8。one2one 头自身完成去重，后处理不再需要 NMS。

### 14.3 工具版本约束

本次仍用 RKNN-Toolkit2 **2.3.2**（aarch64 wheel + Python 3.12）。2.5.0 在 `hybrid_quantization_step1` 的图融合阶段崩溃：

```
File "rknn/api/rules/matmul.py", line 1539, in _p_fuse_exmatmul_add_mul_exsoftmax13_exmatmul_to_sdpa
IndexError: list index out of range
```

640 与 512 输入、`optimization_level` 0/1/3 均可复现，是该模型的注意力结构与 2.5.0 融合规则的冲突，与输入尺寸无关。升级到 2.5.0 前需确认该规则已修复。

### 14.4 板端后端（纯 C）

- `yolo_postprocess_two()`：按两路各自的 zp/scale 反量化，逐锚点取最大类，阈值过滤后按分数取 TopK，无 NMS。
- 推理请求 `want_float=0`，直接消费原生 INT8 输出，省去每帧的 FP16→float 转换。
- 单输出路径完整保留：混血 FP16 模型仍可加载运行，作为回滚路径。

### 14.5 板端实测（640 输入，NPU 1188 MHz）

| 指标 | 单输出混合 | 双输出全 INT8 |
|---|---:|---:|
| `run` | 22.2 ms | 19–21 ms |
| `outputs` | 2 ms | 0 ms（原生 INT8） |
| `post` | 含 NMS | 2 ms，无 NMS |
| 采集帧率 | 30 fps | 30 fps |
| 运行错误 | — | 0 条 |

### 14.6 未完成项

双输出模型尚未在 64 图留出集上与单输出模型做 mAP 对比：现有 C 评测器只认单输出，需扩展为读取两路 INT8 输出后补测。若 mAP50 相对 0.6232 下降超过 2 个点，应回滚至单输出模型。

### 14.7 512 输入实验（未部署）

同流程在 512×512 下实测：`run` 17.42 ms（640 为 24.04 ms）、总耗时 19.90 ms（640 为 28.74 ms），输出 `[1,84,5376]`。模型保存在 `/home/qianruo/yolo26n-512-w8a8.rknn`，未部署：精度未验证，且当时单输出方案仍在服役。

### 14.8 回滚

```bash
sudo cp /home/qianruo/kbuild/fix-20261105/b-migration/yolo26.rknn.live-before-b \
        /opt/autonomous-driving-car/config/yolo26.rknn
sudo systemctl restart autonomous-driving-car
```
