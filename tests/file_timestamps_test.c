/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production file creation with a fixed wall clock: protect downloaded dates,
   existing recordings/logs, rapid restarts, deletion and rotation. */
#define _GNU_SOURCE
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>

static struct timeval test_wallclock;
static time_t test_time(time_t *result)
{
    if (result) *result = test_wallclock.tv_sec;
    return test_wallclock.tv_sec;
}
static int test_gettimeofday(struct timeval *tv, void *zone)
{
    (void)zone;
    *tv = test_wallclock;
    return 0;
}
#define time test_time
#define gettimeofday test_gettimeofday
#define LOG_MAX_SIZE 1024
#include "core/log.c"
#include "video/rec_mp4.c"
#undef time
#undef gettimeofday
#include "http/http_logs.c"
#include "http/http_api_rec.c"

static void put_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fputs(text, fp) >= 0);
    assert(!fclose(fp));
}

static void assert_file(const char *path, const char *expected)
{
    FILE *fp = fopen(path, "rb");
    assert(fp);
    char text[4096] = {0};
    assert(fread(text, 1, sizeof(text) - 1, fp) > 0);
    fclose(fp);
    assert(strstr(text, expected));
}

static void clean_directory(const char *directory)
{
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat st;
        assert(!stat(path, &st));
        if (S_ISDIR(st.st_mode)) clean_directory(path);
        else assert(!unlink(path));
    }
    closedir(dir);
    assert(!rmdir(directory));
}

int main(void)
{
    assert(!setenv("TZ", "UTC", 1));
    tzset();
    struct tm tm = {.tm_year = 126, .tm_mon = 9, .tm_mday = 27,
                    .tm_hour = 12, .tm_min = 34, .tm_sec = 56};
    test_wallclock.tv_sec = mktime(&tm);
    test_wallclock.tv_usec = 123000;
    char root[] = "/tmp/gateway-file-timestamps-XXXXXX";
    assert(mkdtemp(root));
    char day[PATH_MAX], legacy_log[PATH_MAX], legacy_rec[PATH_MAX];
    snprintf(day, sizeof(day), "%s/20261027", root);
    assert(!mkdir(day, 0700));
    snprintf(legacy_log, sizeof(legacy_log), "%s/%s_info.log", day, APP_NAME);
    snprintf(legacy_rec, sizeof(legacy_rec), "%s/rec_123456.mp4", day);
    put_file(legacy_log, "legacy log retained");
    put_file(legacy_rec, "legacy recording retained");

    log_init(root);
    LOG_INFO("first timestamped log");
    LOG_ERROR("timestamped error log");
    assert(strstr(g_log.info_path, "_info_20261027_123456_123.log"));
    assert(strstr(g_log.error_path, "_error_20261027_123456_123.log"));
    char first_path[PATH_MAX];
    safe_strncpy(first_path, sizeof(first_path), g_log.info_path);
    assert_file(first_path, "first timestamped log");
    log_close();
    log_init(root); /* Same millisecond must preserve both old channels. */
    assert(strstr(g_log.info_path, "_info_20261027_123456_123_1.log"));
    LOG_INFO("same millisecond restart");
    assert_file(first_path, "first timestamped log");

    char large[1600];
    memset(large, 'X', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    LOG_INFO("%s", large);
    char rotated[PATH_MAX];
    safe_strncpy(rotated, sizeof(rotated), g_log.info_path);
    LOG_INFO("after size rotation");
    assert(strcmp(rotated, g_log.info_path));
    assert_file(rotated, "XXXXXXXX");
    assert_file(g_log.info_path, "after size rotation");

    assert(!unlink(g_log.info_path));
    test_wallclock.tv_usec = 124000;
    LOG_INFO("after web deletion");
    assert(strstr(g_log.info_path, "_123456_124.log"));
    assert_file(g_log.info_path, "after web deletion");
    test_wallclock.tv_sec += 86400;
    LOG_INFO("after midnight");
    assert(strstr(g_log.info_path, "/20261028/"));
    assert(strstr(g_log.info_path, "_20261028_123456_124.log"));
    log_close();

    test_wallclock.tv_sec -= 86400;
    test_wallclock.tv_usec = 123000;
    char first_name[128], second_name[128];
    rec_mp4_t *first = rec_mp4_create(day, "rec", 1280, 720, first_name, sizeof(first_name));
    rec_mp4_t *second = rec_mp4_create(day, "rec", 1280, 720, second_name, sizeof(second_name));
    assert(first && second);
    assert(!strcmp(first_name, "rec_20261027_123456_123.mp4"));
    assert(!strcmp(second_name, "rec_20261027_123456_123_1.mp4"));
    assert(strcmp(first->path, second->path));
    assert(rec_mp4_finalize(first) == -1); /* Empty-session cleanup is retained. */
    assert(rec_mp4_finalize(second) == -1);
    assert_file(legacy_log, "legacy log retained");
    assert_file(legacy_rec, "legacy recording retained");

    /* Existing APIs accept arbitrary log names and numeric recording suffixes.
       Lists/packs still enumerate both timestamped and legacy regular files. */
    char directory[64], name[256];
    assert(!logs_resolve_rel("20261027/rockchip-smart-gateway_info.log",
                            directory, sizeof(directory), name, sizeof(name)));
    assert(!logs_resolve_rel("20261027/rockchip-smart-gateway_info_20261027_123456_123.log",
                            directory, sizeof(directory), name, sizeof(name)));
    assert(logs_resolve_rel("20261027/../private", directory, sizeof(directory), name, sizeof(name)));
    assert(logs_total_size(root) > 0);
    assert(rec_file_valid("rec_123456.mp4"));
    assert(rec_file_valid("rec_20261027_123456_123.mp4"));
    assert(rec_file_valid("rec_20261027_123456_123_1.mp4"));
    assert(!rec_file_valid("rec_../private.mp4"));
    clean_directory(root);
    puts("PASS: timestamped log/recording names preserve old files, collisions, rotation and API compatibility");
    return 0;
}
