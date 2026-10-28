/* SPDX-License-Identifier: GPL-3.0-or-later */
/* File operations run only in this test's private working directory. */
#include "http/http_files.c"
#include <assert.h>

static char active_file[128]="20261027/video_20261027_120000_000.mp4";
static unsigned closed_count;
int video_rec_status(video_rec_status_t *out)
{ memset(out,0,sizeof(*out));out->recording=1;safe_strncpy(out->file,sizeof(out->file),active_file);return 0; }
int yolo_events_export_report(const char *name)
{ return file_open_relative(YOLO_EVENTS_DIR,name); }
int yolo_events_clear_reports(unsigned *deleted)
{
    unsigned n=0;uint64_t total=0;int rc=file_walk(YOLO_EVENTS_DIR,NULL,NULL,2,1,&n,&total);
    if(deleted)*deleted=n;return rc;
}
void http_send_response(int fd,int code,const char *status,const char *mime,const void *body,size_t size)
{ (void)fd;(void)code;(void)status;(void)mime;(void)body;(void)size;assert(!"unexpected reactor response"); }

static void finished(int fd)
{ (void)fd;__atomic_add_fetch(&closed_count,1,__ATOMIC_RELAXED); }
static void put(const char *name,const void *body,size_t n)
{ int fd=open(name,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(!file_write(fd,body,n));close(fd); }
static const unsigned char mp4[]={0,0,0,8,'f','t','y','p',0,0,0,8,'m','d','a','t',0,0,0,8,'m','o','o','v'};

static char *request(const char *method,const char *uri,const char *headers,const char *body,size_t *length)
{
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    char raw[4096];snprintf(raw,sizeof(raw),"%s %s HTTP/1.1\r\n%s\r\n%s",method,uri,headers?headers:"",body?body:"");
    unsigned expected=__atomic_load_n(&closed_count,__ATOMIC_RELAXED)+1;
    assert(!http_files_start(NULL,pair[0],method,uri,raw,finished));
    size_t cap=8192,used=0;char *result=malloc(cap);assert(result);
    for(;;){struct pollfd p={.fd=pair[1],.events=POLLIN};assert(poll(&p,1,10000)>0);
        if(used+4097>cap){cap*=2;result=realloc(result,cap);assert(result);}
        ssize_t n=read(pair[1],result+used,4096);assert(n>=0);if(!n)break;used+=(size_t)n;}
    close(pair[1]);result[used]=0;*length=used;
    for(int tries=0;__atomic_load_n(&closed_count,__ATOMIC_RELAXED)<expected&&tries<100;tries++)usleep(1000);
    assert(__atomic_load_n(&closed_count,__ATOMIC_RELAXED)==expected);return result;
}

int main(void)
{
    char scratch[]="/tmp/car-file-api-XXXXXX";assert(mkdtemp(scratch));assert(!chdir(scratch));
    assert(!mkdir("logs",0700));assert(!mkdir("logs/20261027",0700));
    assert(!mkdir("recordings",0700));assert(!mkdir("recordings/20261027",0700));
    assert(!mkdir("recordings/detections",0700));assert(!mkdir("recordings/detections/20261027",0700));
    const char text[]="first line\n<script>alert(1)</script>\n中文\n";
    put("logs/20261027/log_info_20261027_120000_000.log",text,sizeof(text)-1);
    put("recordings/20261027/video_20261027_120000_000.mp4",mp4,sizeof(mp4));
    put("recordings/20261027/rec_120001.mp4",mp4,sizeof(mp4));
    const unsigned char open_mp4[]={0,0,0,8,'f','t','y','p',0,0,0,8,'m','d','a','t'};
    put("recordings/20261027/video_20261027_120002_000.mp4",open_mp4,sizeof(open_mp4));
    const char result_text[]="time：num[0] size[1280 720] FocalLength[unknown]\n";
    put("recordings/detections/20261027/result_20261027.txt",result_text,sizeof(result_text)-1);
    assert(!symlink("/etc/passwd","logs/20261027/unsafe.log"));
    assert(!file_relative("20261027/../secret"));assert(!file_relative("20261027/a/b.log"));
    assert(!file_relative("20261027/.private.log"));assert(file_relative("20261027/log_info_20261027_120000_000.log"));
    size_t length;char *response=request("GET","/api/files/preview?type=log&name=20261027%2Flog_info_20261027_120000_000.log",NULL,NULL,&length);
    assert(strstr(response,"200 OK")&&strstr(response,"first line\\u000a")&&strstr(response,"中文"));free(response);
    response=request("GET","/api/files/preview?type=log&name=20261027%2Funsafe.log",NULL,NULL,&length);
    assert(strstr(response,"400 Bad Request"));free(response);
    response=request("GET","/api/files/preview?type=log&name=20261027%2F..%2Fsecret",NULL,NULL,&length);
    assert(strstr(response,"400 Bad Request"));free(response);
    unsigned char *large=malloc(FILE_TEXT_LIMIT+10);memset(large,'x',FILE_TEXT_LIMIT+10);
    put("logs/20261027/large.log",large,FILE_TEXT_LIMIT+10);free(large);
    response=request("GET","/api/files/preview?type=log&name=20261027%2Flarge.log",NULL,NULL,&length);
    assert(strstr(response,"\"truncated\":true"));free(response);
    response=request("POST","/api/files/video-session",NULL,"20261027/video_20261027_120000_000.mp4",&length);
    assert(strstr(response,"409 Conflict"));free(response);
    response=request("POST","/api/files/video-session",NULL,"20261027/rec_120001.mp4",&length);
    assert(strstr(response,"200 OK")&&strstr(response,"HttpOnly; SameSite=Strict"));
    char *cookie=strstr(response,"fm_video=");assert(cookie);char token[65];memcpy(token,cookie+9,64);token[64]=0;
    char headers[256];snprintf(headers,sizeof(headers),"Cookie: fm_video=%s\r\nRange: bytes=8-15\r\n",token);
    assert(http_files_video_authorized("/api/files/video?name=20261027%2Frec_120001.mp4",headers)==0); /* Complete request required. */
    char authenticated[512];snprintf(authenticated,sizeof(authenticated),"GET / HTTP/1.1\r\n%s\r\n",headers);
    assert(http_files_video_authorized("/api/files/video?name=20261027%2Frec_120001.mp4",authenticated));
    assert(!http_files_video_authorized("/api/files/video?name=20261027%2Fvideo_20261027_120000_000.mp4",authenticated));
    free(response);
    response=request("GET","/api/files/video?name=20261027%2Frec_120001.mp4",headers,NULL,&length);
    assert(strstr(response,"206 Partial Content")&&strstr(response,"Content-Range: bytes 8-15/24"));
    char *data=strstr(response,"\r\n\r\n");assert(data);data+=4;assert(length-(size_t)(data-response)==8);assert(!memcmp(data,mp4+8,8));free(response);
    response=request("HEAD","/api/files/video?name=20261027%2Frec_120001.mp4",NULL,NULL,&length);
    assert(strstr(response,"Content-Length: 24\r\n"));assert(strstr(response,"\r\n\r\n")[4]==0);free(response);
    response=request("GET","/api/files/video?name=20261027%2Frec_120001.mp4","Range: bytes=99-\r\n",NULL,&length);
    assert(strstr(response,"416 Range Not Satisfiable")&&strstr(response,"Content-Range: bytes */24"));free(response);
    uint64_t first,last;assert(file_range("bytes=-4",24,&first,&last)==1&&first==20&&last==23);
    assert(file_range("bytes=0-1,4-5",24,&first,&last)<0);assert(file_range("bytes=18446744073709551616-",24,&first,&last)<0);
    response=request("DELETE","/api/files/video-session",headers,NULL,&length);assert(strstr(response,"Max-Age=0"));free(response);
    assert(!http_files_video_authorized("/api/files/video?name=20261027%2Frec_120001.mp4",authenticated));
    response=request("POST","/api/files/download-session",NULL,NULL,&length);
    cookie=strstr(response,"fm_archive=");assert(cookie);memcpy(token,cookie+11,64);token[64]=0;
    snprintf(authenticated,sizeof(authenticated),"GET / HTTP/1.1\r\nCookie: fm_archive=%s\r\n\r\n",token);
    assert(http_files_download_authorized(authenticated));
    assert(!http_files_video_authorized("/api/files/video?name=20261027%2Frec_120001.mp4",authenticated));
    pthread_mutex_lock(&file_session_lock);
    for(unsigned i=0;i<FILE_SESSION_COUNT;i++)file_sessions[i].expires=0;
    pthread_mutex_unlock(&file_session_lock);
    assert(!http_files_download_authorized(authenticated));free(response);
    response=request("GET","/api/files/pack",NULL,NULL,&length);assert(strstr(response,"200 OK"));
    data=strstr(response,"\r\n\r\n");assert(data);data+=4;size_t bytes=length-(size_t)(data-response);
    assert(bytes>4&&!memcmp(data,"PK\003\004",4));put("archive.zip",data,bytes);free(response);
    assert(system("python3 -m zipfile -l archive.zip > archive.list")==0);
    FILE *list=fopen("archive.list","r");assert(list);char names[8192]={0};assert(fread(names,1,sizeof(names)-1,list)>0);fclose(list);
    assert(strstr(names,"logs/20261027/log_info_")&&strstr(names,"recordings/20261027/rec_120001.mp4")&&strstr(names,"results/20261027/result_20261027.txt"));
    assert(!strstr(names,"120000_000.mp4")&&!strstr(names,"120002_000.mp4")&&!strstr(names,"unsafe.log"));
    response=request("POST","/api/files/clear",NULL,NULL,&length);assert(strstr(response,"200 OK")&&strstr(response,"\"kept_active\":true"));free(response);
    assert(!access("recordings/20261027/video_20261027_120000_000.mp4",F_OK));
    assert(!access("recordings/20261027/video_20261027_120002_000.mp4",F_OK));
    assert(access("recordings/20261027/rec_120001.mp4",F_OK)<0);
    assert(access("recordings/detections/20261027/result_20261027.txt",F_OK)<0);
    struct stat st;assert(!lstat("logs/20261027/unsafe.log",&st)&&S_ISLNK(st.st_mode));
    assert(!chdir("/tmp"));file_remove_tree(scratch);
    puts("PASS: bounded previews, MP4 ranges, file-bound expiring sessions, archive safety and active-recording preservation");
    return 0;
}
