/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Isolated Excel export, archive, clear barriers and path protection. */
#include "yolo/yolo_events.c"
#include "http/http_api_detections.c"
#include <assert.h>

static char response[65536], response_mime[128], response_headers[256];
static size_t response_len;
static int response_code;

void _log_error(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }
void _log_info(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }
void http_ok_json(int fd, const char *json, size_t length)
{
    (void)fd; assert(length < sizeof(response));
    memcpy(response, json, length); response[length] = '\0'; response_len = length; response_code = 200;
}
void http_err(int fd, int code, const char *status, const char *message)
{ (void)fd; (void)status; (void)message; response_code = code; response_len = 0; response[0] = '\0'; }
void http_handle_404(int fd, const char *uri)
{ (void)uri; http_err(fd, 404, "Not Found", NULL); }
void http_serve_stream(int fd, const char *mime, const char *headers, FILE *stream,
                       size_t size, const char *unlink_after)
{
    (void)fd; assert(!unlink_after && size < sizeof(response));
    snprintf(response_mime, sizeof(response_mime), "%s", mime);
    snprintf(response_headers, sizeof(response_headers), "%s", headers);
    response_len = fread(response, 1, size, stream); assert(response_len == size);
    response[response_len] = '\0'; response_code = 200; fclose(stream);
}
int http_json_append(char *json, size_t size, int offset, const char *format, ...)
{
    if (offset < 0 || (size_t)offset >= size) return -1;
    va_list arguments; va_start(arguments, format);
    int length = vsnprintf(json + offset, size - (size_t)offset, format, arguments);
    va_end(arguments);
    return length < 0 || (size_t)length >= size - (size_t)offset ? -1 : offset + length;
}
const char *http_body_start(const char *request)
{
    if (!request) return "";
    const char *body = strstr(request, "\r\n\r\n");
    return body ? body + 4 : request;
}

static const char row[] = "2026-10-27T12:34:56.789+0800\t1280\t720\tperson\t0.9\t1\t2\t3\t4\t42\t645\tnull\ttrue\n";

static void put(const char *relative, const char *bytes)
{
    char directory[9]; memcpy(directory, relative, 8); directory[8] = '\0';
    assert(!mkdirat(events.root_fd, directory, 0700) || errno == EEXIST);
    int fd = openat(events.root_fd, relative, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0 && write_all(fd, bytes, strlen(bytes)) == 0); close(fd);
}

static void put_rows(const char *day)
{
    char relative[80]; snprintf(relative, sizeof(relative), "%s/.detections_%s.rows", day, day);
    char bytes[2048]; snprintf(bytes, sizeof(bytes), "%s%s", report_header, row); put(relative, bytes);
}

static void save_response(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0 && write_all(fd, response, response_len) == 0); close(fd);
}

static void python_check(const char *program, char *path)
{
    char *arguments[] = {"python3", "-c", (char *)program, path, NULL};
    assert(run_report_command(arguments, -1) == 0);
}

static void remove_test_reports(void)
{
    int root = report_root_open(); assert(root >= 0);
    DIR *directory = fdopendir(root); assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        int day = openat(root, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (day >= 0) {
            DIR *files = fdopendir(day); assert(files); struct dirent *file;
            while ((file = readdir(files)))
                if (strcmp(file->d_name, ".") && strcmp(file->d_name, "..")) assert(unlinkat(day, file->d_name, 0) == 0);
            closedir(files); assert(unlinkat(root, entry->d_name, AT_REMOVEDIR) == 0);
        } else assert(unlinkat(root, entry->d_name, 0) == 0); /* Symlink itself only. */
    }
    closedir(directory);
}

int main(void)
{
    char source[PATH_MAX], exporter[PATH_MAX];
    assert(realpath(__FILE__, source)); *strrchr(source, '/') = '\0';
    assert(snprintf(exporter, sizeof(exporter), "%s/../scripts/export_detection_excel.py", source) < (int)sizeof(exporter));
    assert(setenv("GATEWAY_EXCEL_EXPORTER", exporter, 1) == 0);
    char scratch[] = "/tmp/yolo-bulk-test-XXXXXX"; assert(mkdtemp(scratch));
    int original = open(".", O_RDONLY | O_DIRECTORY); assert(original >= 0 && chdir(scratch) == 0);
    assert(mkdir("recordings", 0700) == 0 && mkdir(YOLO_EVENTS_DIR, 0700) == 0);
    events.root_fd = open(YOLO_EVENTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(events.root_fd >= 0);
    assert(yolo_report_rel_valid("20010101/detections_20010101.xlsx"));
    assert(!yolo_report_rel_valid("20010101/detections_20010101.txt"));
    assert(!yolo_report_rel_valid("20010101/detections_20010102.xlsx"));
    assert(!yolo_report_rel_valid("20010101/../secret.xlsx"));

    http_detection_reports_pack(NULL, 0, "GET", "/api/detections/reports/pack", NULL);
    assert(response_code == 404);
    put_rows("20010101"); put_rows("20010102");
    put("20010101/legacy.json", "{\"preserve\":true}");
    put("20010101/legacy.jpg", "old photo"); put("20010101/detections_20010101.txt", "historical migration input");
    assert(mkdirat(events.root_fd, "20010103", 0700) == 0);
    int malicious = openat(events.root_fd, "20010103", O_RDONLY | O_DIRECTORY); assert(malicious >= 0);
    assert(symlinkat("/etc/passwd", malicious, "detections_20010103.xlsx") == 0); close(malicious);
    assert(symlinkat("/tmp", events.root_fd, "20010104") == 0);

    http_detection_reports_list(NULL, 0, "GET", "/api/detections/reports", NULL);
    assert(response_code == 200 && strstr(response, "20010101.xlsx") && strstr(response, "20010102.xlsx"));
    assert(!strstr(response, "20010103") && !strstr(response, "20010104") && !strstr(response, ".rows"));
    assert(strstr(response, "20010102") < strstr(response, "20010101"));
    http_detection_file(NULL, 0, "GET", "/detectionfile/20010101/detections_20010101.xlsx", NULL);
    assert(response_code == 200 && !strcmp(response_mime, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"));
    assert(strstr(response_headers, "detections_20010101.xlsx") && !memcmp(response, "PK", 2));
    assert(faccessat(events.root_fd, "20010101/detections_20010101.xlsx", F_OK, 0) == 0);
    save_response("download.xlsx");
    python_check("import sys,zipfile,xml.etree.ElementTree as E; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; n={'m':'http://schemas.openxmlformats.org/spreadsheetml/2006/main'}; rows=E.fromstring(z.read('xl/worksheets/sheet1.xml')).findall('m:sheetData/m:row',n); assert len(rows)==3; text=lambda c:c.find('m:is/m:t',n).text; assert text(rows[0][0])=='Targets' and rows[0][1].find('m:v',n).text=='1' and text(rows[0][2])=='Resolution' and text(rows[0][4])=='Camera' and rows[0][9].find('m:v',n).text=='42'; assert text(rows[1][0])=='Target' and text(rows[2][0])=='person'", "download.xlsx");
    http_detection_file(NULL, 0, "GET", "/detectionfile/20010103/detections_20010103.xlsx", NULL); assert(response_code == 404);
    http_detection_file(NULL, 0, "GET", "/detectionfile/20010101/../secret.xlsx", NULL); assert(response_code == 404);

    http_detection_reports_pack(NULL, 0, "GET", "/api/detections/reports/pack", NULL);
    assert(response_code == 200 && !strcmp(response_mime, "application/gzip") && strstr(response_headers, "detections_pack.tar.gz"));
    save_response("reports.tar.gz");
    python_check("import sys,tarfile,zipfile,io; t=tarfile.open(sys.argv[1]); files=[m for m in t.getmembers() if m.isfile()]; assert {m.name.lstrip('./') for m in files}=={'20010101/detections_20010101.xlsx','20010102/detections_20010102.xlsx'}; assert all(zipfile.ZipFile(io.BytesIO(t.extractfile(m).read())).testzip() is None for m in files)", "reports.tar.gz");

    unsigned long long before_clear = atomic_load(&report_epoch);
    http_detection_reports_clear(NULL, 0, "POST", "/api/detections/reports/clear", NULL);
    assert(response_code == 200 && !strcmp(response, "{\"deleted\":2}"));
    assert(flush_report_epoch("20010101", row, sizeof(row) - 1, before_clear) == 1);
    assert(faccessat(events.root_fd, "20010101/.detections_20010101.rows", F_OK, 0) < 0);
    assert(faccessat(events.root_fd, "20010101/detections_20010101.xlsx", F_OK, 0) < 0);
    assert(faccessat(events.root_fd, "20010101/legacy.json", F_OK, 0) == 0);
    assert(faccessat(events.root_fd, "20010101/legacy.jpg", F_OK, 0) == 0);
    assert(faccessat(events.root_fd, "20010101/detections_20010101.txt", F_OK, 0) == 0);
    unsigned long long fresh = atomic_load(&report_epoch);
    assert(flush_report_epoch("20010101", row, sizeof(row) - 1, fresh) == 0);
    assert(yolo_events_delete_report("20010101/detections_20010101.xlsx") == 0);
    assert(flush_report_epoch("20010101", row, sizeof(row) - 1, fresh) == 1);
    assert(flush_report_epoch("20010102", row, sizeof(row) - 1, fresh) == 0); /* Other days are not discarded. */
    assert(flush_report_epoch("20010101", row, sizeof(row) - 1, atomic_load(&report_epoch)) == 0);
    errno = 0; assert(yolo_events_delete_report("20010103/detections_20010103.xlsx") < 0 && errno == EINVAL);

    /* A completed snapshot cannot resurrect reports after a clear request. */
    char snapshot[] = "/tmp/yolo-bulk-snapshot-XXXXXX"; assert(mkdtemp(snapshot));
    unsigned count = 0; unsigned long long snapshot_epoch;
    pthread_mutex_lock(&report_io_lock);
    assert(snapshot_reports(snapshot, &count, NULL, &snapshot_epoch) == 0 && count == 2);
    pthread_mutex_unlock(&report_io_lock);
    assert(yolo_events_clear_reports(&count) == 0 && count == 2);
    assert(!snapshot_epoch_unchanged(snapshot_epoch) && errno == EAGAIN); remove_report_snapshot(snapshot);

    /* Reject oversized input before copying its sparse bytes or invoking Python. */
    put_rows("20010101");
    int huge = openat(events.root_fd, "20010101/.detections_20010101.rows", O_WRONLY | O_CLOEXEC);
    assert(huge >= 0 && ftruncate(huge, 2ll * 1024 * 1024 * 1024 + 1) == 0); close(huge);
    http_detection_reports_pack(NULL, 0, "GET", "/api/detections/reports/pack", NULL); assert(response_code == 413);
    assert(yolo_events_clear_reports(&count) == 0 && count == 1);
    assert(yolo_events_clear_reports(&count) == 0 && count == 0);
    http_detection_reports_pack(NULL, 0, "GET", "/api/detections/reports/pack", NULL); assert(response_code == 404);

    remove_test_reports(); close(events.root_fd); events.root_fd = -1;
    assert(rmdir(YOLO_EVENTS_DIR) == 0 && rmdir("recordings") == 0);
    assert(unlink("download.xlsx") == 0 && unlink("reports.tar.gz") == 0);
    assert(fchdir(original) == 0); close(original); assert(rmdir(scratch) == 0);
    puts("PASS: standard Excel export/download, daily-only archive, clear/delete generation barriers, size and symlink protection");
    return 0;
}
