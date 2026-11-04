# Frame publication and model verification

Update on 2026-11-04: annotated frames are no longer encoded. Live view and recording both consume the camera's raw MJPEG directly, so the ring now has a single publication topic and no encoder stage. Earlier revisions used a prepared V4L2 JPEG driver, cached DMA-BUF and full-range BT.601 NEON conversion to RGB; that path still feeds inference and is unchanged.

## Frame handoff

The frame ring publishes two topics under its mutex:

| Publication | Subscribers | Payload ownership |
| --- | --- | --- |
| `FRAME_TOPIC_CAPTURED` after V4L2 dequeue | RGB decode producer, raw-frame readers, recorder | mmap buffer stays pinned by claims; delayed QBUF |

`FRAME_TOPIC_DISPLAYED` was removed with the encoder stage. Rendering still advances `display_seq`, which is what lets the maintain pass return the RGB buffer of skipped and superseded slots to the pool.

Decode hands each RGB job to the bounded inference queue and wakes eligible workers. Workers publish results to the result queue, which wakes the renderer. These queues preserve backpressure and the renderer's latest-result policy. No broker thread or additional image copy is needed for notification.

Each subscriber tracks a frame sequence and cancellation generation. Checking availability and waiting use the same mutex as publication, so a frame published before a wait is still observed. Capture wakes capture subscribers; the recording thread waits on the same topic and copies each frame in sequence. Live viewers take the newest frame by copying it under the ring lock, so a slow client never pins a slot.

Stopping, reloading and camera quiescence cancel outstanding subscriptions, including a cancellation that occurs just before entering the wait. Wait deadlines use `CLOCK_MONOTONIC`. Timeouts only allow periodic exit and disconnect checks; they do not impose a frame-processing interval. Notification makes a consumer runnable immediately; CPU availability, processing time and queue backpressure still determine when it executes.

The camera format remains MJPEG at the configured 1280×720 and requested 30 FPS. The attached UGREEN Camera 2K supports no direct RGB/BGR output; its YUYV modes reach 640×480 at 30 FPS or 800×600 at 20 FPS. The raw-format change was cancelled, so this update does not reduce resolution or switch formats. Negotiated FPS is not a measurement of actual frame cadence.

## Verified YOLO26n FP16 baseline

On 2026-11-02, the original FP16 model was verified against the [official YOLO26n checkpoint](https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n.pt). It was already YOLO26n. The subsequent W8A8/FP16 conversion and the single detection output are documented in the [Chinese quantization report](YOLO26N_QUANTIZATION.md). The experimental P4 output and target descriptors have been removed. The canonical runtime filename remains `config/yolo26.rknn`.

| Artifact | SHA-256 |
| --- | --- |
| RK3576 RKNN, 10,528,118 bytes, compiler 2.3.2 | `d4465c45a49e3ae995dd2dbb5e9a5984424aba3268ece02f112647db04f6963a` |
| Official `yolo26n.pt`, 5,544,453 bytes | `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef` |

The checkpoint declares nano scale and 2,572,280 original parameters. All 204 fused one-to-many weight/bias tensors correspond to the compiled model: 196 shapes match directly, and eight depthwise weights use the equivalent RKNN layout. These tensors account for 2,408,932 parameters. The first two convolution weight tensors (5,040 values) match the official checkpoint byte for byte after FP16 conversion and hardware layout packing; five complete fused bias tensors also match. The numeric check samples provenance rather than auditing every compiled weight.

The model takes 640×640 RGB and produces two INT8 tensors: class scores `[1,80,8400]` and boxes `[1,4,8400]` as centre/size in model pixels. The one2one assignment reduces duplicate candidates but does not remove them, so postprocessing is threshold, top-K and class-wise NMS. Model weights stay outside Git; build and deployment continue to reconcile the local and installed artifact by hash.

## Validation

The ring was checked with ASan and UBSan for publication before and during waits, multiple subscribers, topic isolation, cancellation races, monotonic timeouts, RGB pool availability, claim protection, delayed QBUF and quiescence. Temporary verification programs and model-inspection dependencies remain outside the repository.
