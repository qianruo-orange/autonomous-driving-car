import argparse, json, math
from pathlib import Path
import numpy as np

def iou(box, boxes):
    if not len(boxes): return np.zeros(0)
    area = np.maximum(0, boxes[:, 2:] - boxes[:, :2]).prod(1)
    inter = np.maximum(0, np.minimum(box[2:], boxes[:, 2:]) - np.maximum(box[:2], boxes[:, :2])).prod(1)
    return inter / np.maximum(1e-9, area + np.maximum(0, box[2:]-box[:2]).prod() - inter)

def decode(raw, w, h, sx, sy, px, py, layout='NCHW'):
    values = np.fromfile(raw, dtype='<f4')
    assert layout in ('NCHW', 'NHWC'), layout
    anchors = values.size // 84
    a = values.reshape(84, anchors).T if layout == 'NCHW' else values.reshape(anchors, 84)
    assert np.isfinite(a).all(), str(raw)
    scores = a[:, 4:]
    assert scores.min() >= -.001 and scores.max() <= 1.001, str(raw)
    cls = scores.argmax(1)
    conf = scores[np.arange(len(a)), cls]
    keep = conf >= .001
    a, conf, cls = a[keep], conf[keep], cls[keep]
    boxes = np.concatenate((a[:, :2] - a[:, 2:4]/2, a[:, :2] + a[:, 2:4]/2), 1)
    valid = ((boxes[:, 2:] - boxes[:, :2]) > 0).all(1)
    boxes, conf, cls = boxes[valid], conf[valid], cls[valid]
    order = np.argsort(-conf, kind='stable')
    selected = []
    while len(order) and len(selected) < 100:
        index = order[0]
        selected.append(index)
        rest = order[1:]
        order = rest[(cls[rest] != cls[index]) | (iou(boxes[index], boxes[rest]) <= .7)]
    boxes = boxes[selected]
    boxes[:, [0, 2]] = np.clip((boxes[:, [0, 2]] - px) / sx, 0, w)
    boxes[:, [1, 3]] = np.clip((boxes[:, [1, 3]] - py) / sy, 0, h)
    return np.column_stack((boxes, conf[selected], cls[selected]))

def ground_truth(label, w, h):
    if not label.exists() or not label.read_text().strip(): return np.zeros((0, 5))
    a = np.loadtxt(label, dtype=np.float32, ndmin=2)
    assert a.shape[1] == 5, str(label)
    xy = a[:, 1:3] * [w, h]
    wh = a[:, 3:5] * [w, h]
    return np.column_stack((xy-wh/2, xy+wh/2, a[:, 0]))

def load_set(tsv, folder):
    output = {}
    results_path = Path(folder).parent / (Path(folder).name.removesuffix('-raw') + '-results.jsonl')
    records = {v['id']:v for line in results_path.read_text().splitlines()
               if (v := json.loads(line)).get('record') == 'image'}
    for line in Path(tsv).read_text().splitlines():
        if not line.strip() or line.startswith('#'): continue
        ident, image, w, h, label = line.split('\t')[:5]
        w, h = int(w), int(h)
        raw = Path(folder) / (ident + '.bin')
        if not raw.exists(): raw = Path(folder) / 'raw' / (ident + '.bin')
        if not raw.exists(): raw = Path(folder) / (ident + '.f32')
        if not raw.exists(): raw = Path(folder) / 'raw' / (ident + '.f32')
        record = records[ident]
        assert record['width'] == w and record['height'] == h
        # letterbox 几何由 prepare_dataset.py 记录，和量化时的输入完全一致
        sx, sy, px, py = [record[k] for k in ['scale_x', 'scale_y', 'pad_x', 'pad_y']]
        output[ident] = dict(pred=decode(raw, w, h, sx, sy, px, py, record['raw_layout']), gt=ground_truth(Path(label), w, h))
    return output

def evaluate(images):
    classes = sorted({int(g[4]) for item in images.values() for g in item['gt']})
    levels = np.linspace(.5, .95, 10)
    confidence, predicted_class, correct = [], [], []
    counts = {c: 0 for c in classes}
    for image in images.values():
        gt, pred = image['gt'], image['pred']
        for g in gt: counts[int(g[4])] += 1
        matches = np.zeros((len(pred), len(levels)), dtype=bool)
        for ti, threshold in enumerate(levels):
            used = set()
            for di, det in enumerate(pred):
                candidates = [i for i, g in enumerate(gt) if int(g[4]) == int(det[5]) and i not in used]
                if not candidates: continue
                values = iou(det[:4], gt[candidates, :4])
                best = int(values.argmax())
                if values[best] >= threshold:
                    used.add(candidates[best]); matches[di, ti] = True
        confidence.extend(pred[:, 4]); predicted_class.extend(pred[:, 5]); correct.extend(matches)
    confidence, predicted_class = np.array(confidence), np.array(predicted_class)
    correct = np.array(correct, dtype=bool).reshape(-1, 10)
    per_class = {}
    for c in classes:
        selected = np.flatnonzero(predicted_class == c)
        selected = selected[np.argsort(-confidence[selected], kind='stable')]
        tp = correct[selected].cumsum(0)
        fp = (~correct[selected]).cumsum(0)
        recall = tp / counts[c]
        precision = tp / np.maximum(tp+fp, 1)
        aps = []
        for ti in range(10):
            aps.append(float(np.mean([precision[recall[:, ti] >= r, ti].max(initial=0) for r in np.linspace(0, 1, 101)])))
        per_class[str(c)] = dict(labels=counts[c], ap50=aps[0], ap50_95=float(np.mean(aps)))
    return dict(images=len(images), labels=sum(counts.values()), classes=len(classes),
                map50=float(np.mean([a['ap50'] for a in per_class.values()])),
                map50_95=float(np.mean([a['ap50_95'] for a in per_class.values()])), per_class=per_class)

def consistency(base, candidate):
    total, retained, new_count = 0, 0, 0
    overlaps, deltas = [], []
    for ident, image in base.items():
        first, second = image['pred'], candidate[ident]['pred']
        first, second = first[first[:, 4] >= .5], second[second[:, 4] >= .5]
        total += len(first); new_count += len(second)
        used = set()
        for det in first:
            ids = [i for i, x in enumerate(second) if int(x[5]) == int(det[5]) and i not in used]
            if not ids: continue
            values = iou(det[:4], second[ids, :4]); best = int(values.argmax())
            if values[best] >= .5:
                j = ids[best]; used.add(j); retained += 1
                overlaps.append(float(values[best])); deltas.append(abs(float(det[4]-second[j, 4])))
    return dict(fp16_high_conf_detections=total, candidate_high_conf_detections=new_count,
                matched_iou50=retained, retained_fraction=retained/total if total else None,
                matched_mean_iou=float(np.mean(overlaps)) if overlaps else None,
                matched_score_abs_mean=float(np.mean(deltas)) if deltas else None,
                matched_score_abs_p95=float(np.percentile(deltas, 95)) if deltas else None)

if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('manifest'); p.add_argument('baseline'); p.add_argument('candidate'); p.add_argument('output')
    a = p.parse_args(); first, second = load_set(a.manifest, a.baseline), load_set(a.manifest, a.candidate)
    result = dict(protocol='64-image held-out subset, confidence>=0.001, class-wise NMS IoU0.7, max100, 101-point AP',
                  baseline=evaluate(first), candidate=evaluate(second), consistency=consistency(first, second))
    result['delta_map50'] = result['candidate']['map50']-result['baseline']['map50']
    result['delta_map50_95'] = result['candidate']['map50_95']-result['baseline']['map50_95']
    Path(a.output).write_text(json.dumps(result, indent=2))
    print(json.dumps({k:v for k,v in result.items() if k not in ('baseline','candidate')}, indent=2))
    print('baseline', {k:v for k,v in result['baseline'].items() if k!='per_class'})
    print('candidate', {k:v for k,v in result['candidate'].items() if k!='per_class'})
