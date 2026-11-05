/* 把 RKNN 模型在验证集上的原始输出 dump 成 float32，供 evaluate.py 算 mAP。
 *
 * 输出布局与训练侧的 84×A 单张量一致（行主序）：
 *   row 0..3  = cx, cy, w, h（模型像素）
 *   row 4..83 = 80 类分数
 * 这样 evaluate.py 不需要知道模型是单输出还是双输出。
 *
 *   gcc -O2 -o dump_outputs dump_outputs.c -I../../include/yolo -l:librknnrt.so
 *   ./dump_outputs yolo26n-512-box16.rknn work/v512-in work/v512-raw
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rknn_api.h"

static int compare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "用法: %s <model.rknn> <输入目录(.bin)> <输出目录(.f32)>\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("open model"); return 1; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void *model = malloc((size_t)size);
    if (!model || fread(model, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); return 1; }
    fclose(fp);

    rknn_context ctx = 0;
    if (rknn_init(&ctx, model, (uint32_t)size, 0, NULL) != RKNN_SUCC) {
        fprintf(stderr, "rknn_init 失败\n");
        return 1;
    }

    rknn_input_output_num io;
    memset(&io, 0, sizeof(io));
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != RKNN_SUCC ||
        io.n_input < 1 || io.n_output < 1) {
        fprintf(stderr, "查询输入输出失败\n");
        return 1;
    }

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) return 1;
    int in_w = in_attr.fmt == RKNN_TENSOR_NCHW ? (int)in_attr.dims[3] : (int)in_attr.dims[2];
    int in_h = in_attr.fmt == RKNN_TENSOR_NCHW ? (int)in_attr.dims[2] : (int)in_attr.dims[1];

    rknn_tensor_attr out[2];
    int scores_idx = -1, boxes_idx = -1;
    for (unsigned i = 0; i < io.n_output && i < 2; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].index = i;
        if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out[i], sizeof(out[i])) != RKNN_SUCC) return 1;
        if (out[i].dims[out[i].n_dims - 1] == 4 || out[i].dims[1] == 4) boxes_idx = (int)i;
        else scores_idx = (int)i;
    }
    if (scores_idx < 0 || boxes_idx < 0) {
        fprintf(stderr, "输出不是「类别 + 框」两路\n");
        return 1;
    }

    /* 类别：NHWC [1,A,80] 或 NCHW [1,80,A]；框：始终 [1,4,A] */
    int scores_nchw = out[scores_idx].dims[1] <= out[scores_idx].dims[2];
    int anchors = scores_nchw ? (int)out[scores_idx].dims[2] : (int)out[scores_idx].dims[1];
    int classes = scores_nchw ? (int)out[scores_idx].dims[1] : (int)out[scores_idx].dims[2];
    if (anchors != (int)out[boxes_idx].dims[2] && anchors != (int)out[boxes_idx].dims[1]) {
        fprintf(stderr, "类别与框的锚点数不一致\n");
        return 1;
    }
    printf("输入 %dx%d  锚点 %d  类别 %d  类别布局 %s\n",
           in_w, in_h, anchors, classes, scores_nchw ? "NCHW" : "NHWC");

    size_t in_bytes = (size_t)in_w * in_h * 3;
    unsigned char *input = malloc(in_bytes);
    float *line = malloc(sizeof(float) * (size_t)(4 + classes) * anchors);
    if (!input || !line) return 1;

    /* 输出目录不存在就建一层，省得调用方先 mkdir */
    if (mkdir(argv[3], 0755) && errno != EEXIST) {
        perror("创建输出目录");
        return 1;
    }

    DIR *dir = opendir(argv[2]);
    if (!dir) { perror("open 输入目录"); return 1; }
    const char **names = NULL;
    size_t count = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".bin")) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, cap * sizeof(*names));
        }
        names[count++] = strdup(de->d_name);
    }
    closedir(dir);
    qsort(names, count, sizeof(*names), compare);

    int failures = 0;
    for (size_t n = 0; n < count; n++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", argv[2], names[n]);
        FILE *in = fopen(path, "rb");
        if (!in || fread(input, 1, in_bytes, in) != in_bytes) {
            fprintf(stderr, "读取失败 %s\n", path);
            failures++;
            if (in) fclose(in);
            continue;
        }
        fclose(in);

        rknn_input rk_in;
        memset(&rk_in, 0, sizeof(rk_in));
        rk_in.index = 0;
        rk_in.type = RKNN_TENSOR_UINT8;
        rk_in.fmt = RKNN_TENSOR_NHWC;
        rk_in.size = (uint32_t)in_bytes;
        rk_in.buf = input;
        if (rknn_inputs_set(ctx, 1, &rk_in) != RKNN_SUCC || rknn_run(ctx, NULL) != RKNN_SUCC) {
            fprintf(stderr, "推理失败 %s\n", path);
            failures++;
            continue;
        }

        rknn_output outs[2];
        memset(outs, 0, sizeof(outs));
        for (unsigned i = 0; i < io.n_output && i < 2; i++) {
            outs[i].index = i;
            /* 框让运行时转 float；类别保留 INT8 由这里反量化 */
            outs[i].want_float = (int)i == boxes_idx;
        }
        if (rknn_outputs_get(ctx, io.n_output > 2 ? 2 : io.n_output, outs, NULL) != RKNN_SUCC) {
            fprintf(stderr, "取输出失败 %s\n", path);
            failures++;
            continue;
        }

        const float *boxes = (const float *)outs[boxes_idx].buf;
        const signed char *scores = (const signed char *)outs[scores_idx].buf;
        const float scale = out[scores_idx].scale;
        const int zp = out[scores_idx].zp;
        for (int i = 0; i < anchors; i++) {
            for (int k = 0; k < 4; k++) line[(size_t)k * anchors + i] = boxes[(size_t)k * anchors + i];
            for (int c = 0; c < classes; c++) {
                int raw = scores_nchw ? scores[(size_t)c * anchors + i]
                                      : scores[(size_t)i * classes + c];
                line[(size_t)(4 + c) * anchors + i] = (float)(raw - zp) * scale;
            }
        }
        rknn_outputs_release(ctx, io.n_output > 2 ? 2 : io.n_output, outs);

        char stem[PATH_MAX];
        snprintf(stem, sizeof(stem), "%s", names[n]);
        stem[strlen(stem) - 4] = 0;
        snprintf(path, sizeof(path), "%s/%s.f32", argv[3], stem);
        FILE *of = fopen(path, "wb");
        if (!of || fwrite(line, sizeof(float), (size_t)(4 + classes) * anchors, of) !=
                        (size_t)(4 + classes) * anchors) {
            fprintf(stderr, "写出失败 %s: %s\n", path, strerror(errno));
            failures++;
        }
        if (of) fclose(of);
    }

    printf("DUMPED %zu 张，失败 %d\n", count - (size_t)failures, failures);
    for (size_t n = 0; n < count; n++) free((void *)names[n]);
    free(names);
    free(line);
    free(input);
    rknn_destroy(ctx);
    free(model);
    return failures ? 1 : 0;
}
