/* Exercise production platform mapping and upload destination installation. */
#include "yolo/rknn_yolo.c"
#include "http/http_api_yolo.c"
#include <assert.h>
#include <dirent.h>

static void write_bytes(const char *path, const char *bytes)
{
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes));
    assert(!fclose(file));
}

static void expect_bytes(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb"); assert(file);
    char data[64]; size_t length = fread(data, 1, sizeof(data) - 1, file);
    data[length] = '\0'; assert(!fclose(file)); assert(!strcmp(data, expected));
}

int main(void)
{
    const char rk3576[] = "radxa,rock-4d\0rockchip,rk3576\0";
    const char rk3588s[] = "board,example\0rockchip,rk3588s\0";
    const char rk3576m[] = "rockchip,rk3576m";
    const char invalid[] = "other,rk3576\0rockchip,rk35880\0";
    const npu_platform_info_t *platform = npu_platform_from_compatible(rk3576, sizeof(rk3576));
    assert(!strcmp(platform->name, "rk3576") && platform->cores == 2);
    platform = npu_platform_from_compatible(rk3588s, sizeof(rk3588s));
    assert(!strcmp(platform->name, "rk3588") && platform->cores == 3);
    platform = npu_platform_from_compatible(rk3576m, sizeof(rk3576m) - 1); /* Bounded non-NUL input. */
    assert(!strcmp(platform->name, "rk3576") && platform->cores == 2);
    assert(npu_platform_from_compatible(invalid, sizeof(invalid)) == &npu_unknown_platform);
    assert(npu_platform_from_compatible(NULL, 0) == &npu_unknown_platform);
    char model[128];
    rknn_yolo_default_model_path(model, sizeof(model));
    assert(!strcmp(model, "config/yolo26.rknn"));
    rknn_yolo_default_model_path(NULL, 0);

    char scratch[] = "/tmp/car-model-path-test-XXXXXX";
    assert(mkdtemp(scratch));
    int original = open(".", O_RDONLY | O_DIRECTORY);
    assert(original >= 0 && chdir(scratch) == 0);
    const char *destination = "config/yolo26.rknn";
    assert(yolo_prepare_model_directory(destination) == 0);
    write_bytes(destination, "RKNNold");
    assert(yolo_atomic_install("missing-upload", destination) < 0);
    expect_bytes(destination, "RKNNold");
    write_bytes("upload", "RKNNnew");
    assert(yolo_atomic_install("upload", destination) == 0);
    assert(access("upload", F_OK) < 0);
    expect_bytes(destination, "RKNNnew");
    assert(unlink(destination) == 0 && rmdir("config") == 0);
    assert(mkdir("outside", 0700) == 0);
    assert(symlink("outside", "config") == 0);
    assert(yolo_prepare_model_directory(destination) < 0); /* No symlink parent escape. */
    assert(unlink("config") == 0 && rmdir("outside") == 0);
    assert(fchdir(original) == 0); close(original); assert(rmdir(scratch) == 0);
    puts("PASS: platform aliases, canonical model defaults, atomic upload installation and parent path protection");
    return 0;
}
