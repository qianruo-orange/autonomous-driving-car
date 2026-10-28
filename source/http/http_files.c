/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded file workers: previews, native MP4 ranges, whole-page pack/clear. */
#define _GNU_SOURCE
#include "http/http_internal.h"
#include "video/video_rec.h"
#include "yolo/yolo_events.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILE_TEXT_LIMIT 65536u
#define FILE_PACK_LIMIT (8ull * 1024u * 1024u * 1024u)
#define FILE_SESSION_COUNT 64u
#define FILE_WORKERS 4u
typedef struct { char token[65], name[256]; uint64_t expires; } file_session_t;
static file_session_t file_sessions[FILE_SESSION_COUNT];
static pthread_mutex_t file_session_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_bulk_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned file_workers;

typedef struct {
    int fd; char method[8], uri[HTTP_URI_MAX], log_root[256]; char *request;
    void (*on_close)(int);
} file_job_t;

static uint64_t file_now(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec*1000u+ts.tv_nsec/1000000u; }

static int file_header(const char *request, const char *key, char *out, size_t cap)
{
    size_t n = strlen(key); const char *line = strstr(request, "\r\n");
    while (line && line[2] && line[2] != '\r') {
        line += 2; const char *end = strstr(line, "\r\n"); if (!end) return 0;
        if ((size_t)(end-line) > n && !strncasecmp(line,key,n) && line[n]==':') {
            const char *value=line+n+1; while (value<end && (*value==' ' || *value=='\t')) value++;
            size_t length=(size_t)(end-value); if (length>=cap) return 0;
            memcpy(out,value,length);out[length]=0;return 1;
        }
        line=end;
    }
    return 0;
}

static int file_query(const char *uri, const char *key, char *out, size_t cap)
{
    const char *q=strchr(uri,'?'); if (!q) return 0; q++;
    size_t n=strlen(key);
    while (*q) {
        const char *end=strchr(q,'&'); size_t length=end?(size_t)(end-q):strlen(q);
        if (length>n && !memcmp(q,key,n) && q[n]=='=') {
            char encoded[HTTP_URI_MAX]; if (length-n-1>=sizeof(encoded)) return 0;
            memcpy(encoded,q+n+1,length-n-1); encoded[length-n-1]=0;
            return http_url_decode(encoded,out,cap)>0;
        }
        if (!end) break; q=end+1;
    }
    return 0;
}

static int file_day(const char *s)
{ if (strlen(s)!=8) return 0; for(int i=0;i<8;i++) if(s[i]<'0'||s[i]>'9') return 0; return 1; }

static int file_relative(const char *name)
{
    size_t n=name?strlen(name):0; if(n<10 || n>=256 || name[8]!='/' || strstr(name,"..")) return 0;
    for(size_t i=0;i<8;i++) if(name[i]<'0'||name[i]>'9') return 0;
    for(size_t i=9;i<n;i++) if(!((name[i]>='a'&&name[i]<='z') || (name[i]>='A'&&name[i]<='Z') ||
        (name[i]>='0'&&name[i]<='9') || name[i]=='_' || name[i]=='-' || name[i]=='.')) return 0;
    return name[9]!='.';
}

static int file_kind(const char *name, int kind)
{
    size_t n=strlen(name);
    if(kind==1) return n>4 && !strcmp(name+n-4,".mp4") &&
        (!strncmp(name,"rec_",4)||!strncmp(name,"video_",6));
    if(kind==2) return n>4 && !strcmp(name+n-4,".txt") &&
        (!strncmp(name,"detections_",11)||!strncmp(name,"result_",7));
    return n>4 && (strstr(name,".log")!=NULL) && name[0]!='.';
}

static int file_open_relative(const char *root,const char *name)
{
    if(!file_relative(name)){errno=EINVAL;return -1;}
    int r=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); if(r<0)return -1;
    char day[9];memcpy(day,name,8);day[8]=0;
    int d=openat(r,day,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(r);if(d<0)return -1;
    int fd=openat(d,name+9,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);close(d);
    struct stat info;
    if(fd>=0 && (fstat(fd,&info)||!S_ISREG(info.st_mode))){close(fd);errno=EINVAL;return -1;}
    return fd;
}

static uint64_t file_be64(const unsigned char *s)
{ uint64_t n=0;for(int i=0;i<8;i++)n=n*256+s[i];return n; }
static uint32_t file_be32(const unsigned char *s)
{ return (uint32_t)s[0]<<24 | (uint32_t)s[1]<<16 | (uint32_t)s[2]<<8 | s[3]; }

/* Preview/archive require a complete MP4. Creation may precede status
 * publication; a complete moov protects those readers as well. Deletion uses
 * the recorder's lifecycle guard and does not require a valid container. */
static int file_mp4_closed(int fd,const char *relative)
{
    video_rec_status_t status={0};video_rec_status(&status);
    if(status.file[0] && !strcmp(status.file,relative))return 0;
    struct stat st;if(fstat(fd,&st)||st.st_size<16)return 0;
    off_t offset=0;int moov=0,ftyp=0;
    for(int count=0;offset<st.st_size && count<10000;count++){
        unsigned char h[16];if(pread(fd,h,8,offset)!=8)return 0;
        uint64_t length=file_be32(h);unsigned minimum=8;
        if(length==1){if(pread(fd,h+8,8,offset+8)!=8)return 0;length=file_be64(h+8);minimum=16;}
        if(length<minimum || length>(uint64_t)(st.st_size-offset))return 0;
        if(!memcmp(h+4,"ftyp",4))ftyp=1;if(!memcmp(h+4,"moov",4))moov=1;
        offset+=(off_t)length;
    }
    return offset==st.st_size && ftyp && moov;
}

static int file_send(int fd,const void *bytes,size_t length,uint64_t started)
{
    size_t offset=0;uint64_t progress=file_now();
    while(offset<length){
        if(file_now()-started>1800000 || file_now()-progress>30000){errno=ETIMEDOUT;return -1;}
        ssize_t n=send(fd,(const char*)bytes+offset,length-offset,MSG_NOSIGNAL);
        if(n>0){offset+=(size_t)n;progress=file_now();continue;}
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            struct pollfd p={.fd=fd,.events=POLLOUT};int rc=poll(&p,1,1000);
            if(rc<0 && errno==EINTR)continue;
            if(rc<0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL)))return -1;continue;
        }
        return -1;
    }
    return 0;
}

static void file_response(file_job_t *job,int status,const char *mime,const char *extra,
                          const void *body,size_t length)
{
    const char *reason=status==200?"OK":status==206?"Partial Content":status==400?"Bad Request":
        status==401?"Unauthorized":status==404?"Not Found":status==405?"Method Not Allowed":
        status==409?"Conflict":status==413?"Payload Too Large":status==416?"Range Not Satisfiable":"Internal Server Error";
    char header[1024];int n=snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%sConnection: close\r\n\r\n",
        status,reason,mime,length,extra?extra:"");uint64_t now=file_now();
    if(n>0 && (size_t)n<sizeof(header) && !file_send(job->fd,header,(size_t)n,now) && body && strcmp(job->method,"HEAD"))
        (void)file_send(job->fd,body,length,now);
}

static void file_error(file_job_t *job,int error)
{
    int code=error==ENOENT?404:error==EINVAL||error==ELOOP?400:error==EAGAIN?409:error==EFBIG?413:500;
    const char *text=code==409?"File is recording or changed; retry after it is saved":
        code==404?"No matching saved file":code==413?"Files exceed the archive size limit":"Unable to process files";
    file_response(job,code,"text/plain; charset=utf-8",NULL,text,strlen(text));
}

static void file_cookie_token_key(const char *request,const char *key,char token[65])
{
    token[0]=0;char cookies[4096];if(!file_header(request,"Cookie",cookies,sizeof(cookies)))return;
    char *p=cookies;
    while(*p){while(*p==' '||*p==';')p++;char *end=strchr(p,';');size_t n=end?(size_t)(end-p):strlen(p);
        size_t prefix=strlen(key);
        if(n==prefix+65 && !memcmp(p,key,prefix)&&p[prefix]=='='){memcpy(token,p+prefix+1,64);token[64]=0;return;}
        if(!end)break;p=end+1;}
}

static void file_cookie_token(const char *request,char token[65])
{ file_cookie_token_key(request,"fm_video",token); }

int http_files_download_authorized(const char *request)
{
    char token[65];file_cookie_token_key(request,"fm_archive",token);if(!token[0])return 0;
    int valid=0;uint64_t now=file_now();pthread_mutex_lock(&file_session_lock);
    for(unsigned i=0;i<FILE_SESSION_COUNT;i++)if(file_sessions[i].expires>now &&
        !strcmp(file_sessions[i].token,token)&&!strcmp(file_sessions[i].name,"__archive__")){valid=1;break;}
    pthread_mutex_unlock(&file_session_lock);return valid;
}

int http_files_video_authorized(const char *uri,const char *request)
{
    char name[256],token[65];if(!file_query(uri,"name",name,sizeof(name))||!file_relative(name))return 0;
    file_cookie_token(request,token);if(!token[0])return 0;
    int valid=0;uint64_t now=file_now();pthread_mutex_lock(&file_session_lock);
    for(unsigned i=0;i<FILE_SESSION_COUNT;i++)if(file_sessions[i].expires>now &&
        !strcmp(file_sessions[i].token,token)&&!strcmp(file_sessions[i].name,name)){valid=1;break;}
    pthread_mutex_unlock(&file_session_lock);return valid;
}

static void file_session(file_job_t *job)
{
    int archive=!strcmp(job->uri,"/api/files/download-session");
    const char *key=archive?"fm_archive":"fm_video";
    char old[65];file_cookie_token_key(job->request,key,old);
    if(!strcmp(job->method,"DELETE")){
        pthread_mutex_lock(&file_session_lock);
        for(unsigned i=0;i<FILE_SESSION_COUNT;i++)if(!strcmp(file_sessions[i].token,old))file_sessions[i].expires=0;
        pthread_mutex_unlock(&file_session_lock);
        char cookie[256];snprintf(cookie,sizeof(cookie),"Set-Cookie: %s=; Path=/api/files; Max-Age=0; HttpOnly; SameSite=Strict\r\n",key);
        file_response(job,200,"application/json",cookie,"{}",2);return;
    }
    if(strcmp(job->method,"POST")){file_response(job,405,"text/plain",NULL,NULL,0);return;}
    const char *name=archive?"__archive__":http_body_start(job->request);
    if(!archive){
        if(!file_relative(name)||!file_kind(name+9,1)){file_error(job,EINVAL);return;}
        int fd=file_open_relative(PATH_RECORDINGS,name);if(fd<0){file_error(job,errno);return;}
        int closed=file_mp4_closed(fd,name);close(fd);if(!closed){file_error(job,EAGAIN);return;}
    }
    unsigned char bytes[32];size_t got=0;
    while(got<sizeof(bytes)){ssize_t n=getrandom(bytes+got,sizeof(bytes)-got,0);if(n<0&&errno==EINTR)continue;
        if(n<=0){file_error(job,EIO);return;}got+=(size_t)n;}
    file_session_t session={0};for(unsigned i=0;i<32;i++)snprintf(session.token+i*2,3,"%02x",bytes[i]);
    safe_strncpy(session.name,sizeof(session.name),name);session.expires=file_now()+300000;
    pthread_mutex_lock(&file_session_lock);unsigned slot=0;
    for(unsigned i=0;i<FILE_SESSION_COUNT;i++){
        if(old[0]&&!strcmp(file_sessions[i].token,old))file_sessions[i].expires=0;
        if(file_sessions[i].expires<file_sessions[slot].expires)slot=i;
    }
    file_sessions[slot]=session;pthread_mutex_unlock(&file_session_lock);
    char cookie[256];snprintf(cookie,sizeof(cookie),"Set-Cookie: %s=%s; Path=/api/files; Max-Age=300; HttpOnly; SameSite=Strict\r\n",key,session.token);
    file_response(job,200,"application/json",cookie,"{\"expires_in\":300}",18);
}

/* JSON escaping keeps valid UTF-8 and replaces invalid/truncated sequences. */
static size_t file_escape(const unsigned char *in,size_t length,char *out)
{
    size_t at=0;
    for(size_t i=0;i<length;){unsigned char c=in[i];
        if(c=='"'||c=='\\'){out[at++]='\\';out[at++]=(char)c;i++;}
        else if(c<32){at+=(size_t)sprintf(out+at,"\\u%04x",c);i++;}
        else if(c<128){out[at++]=(char)c;i++;}
        else {unsigned n=c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;int valid=n&&i+n<=length;
            for(unsigned j=1;valid&&j<n;j++)if((in[i+j]&0xc0)!=0x80)valid=0;
            if(valid && n==3 && ((c==0xe0&&in[i+1]<0xa0)||(c==0xed&&in[i+1]>=0xa0)))valid=0;
            if(valid && n==4 && ((c==0xf0&&in[i+1]<0x90)||(c==0xf4&&in[i+1]>=0x90)))valid=0;
            if(valid){memcpy(out+at,in+i,n);at+=n;i+=n;}else{memcpy(out+at,"\\ufffd",6);at+=6;i++;}
        }
    }
    out[at]=0;return at;
}

static void file_preview(file_job_t *job)
{
    if(strcmp(job->method,"GET")){file_response(job,405,"text/plain",NULL,NULL,0);return;}
    char name[256],type[32];if(!file_query(job->uri,"name",name,sizeof(name))||
        !file_query(job->uri,"type",type,sizeof(type))||!file_relative(name)){file_error(job,EINVAL);return;}
    int kind=!strcmp(type,"log")?0:!strcmp(type,"result")?2:-1;
    if(kind<0||!file_kind(name+9,kind)){file_error(job,EINVAL);return;}
    /* Preview only a bounded prefix. Daily reports are append-only, so pinning
       the original regular inode avoids copying an entire day for 64 KiB. */
    int fd=file_open_relative(kind==2?YOLO_EVENTS_DIR:job->log_root,name);
    if(fd<0){file_error(job,errno);return;}struct stat st;
    if(fstat(fd,&st)){close(fd);file_error(job,EIO);return;}
    unsigned char *text=malloc(FILE_TEXT_LIMIT);char *json=malloc(FILE_TEXT_LIMIT*6+256);
    if(!text||!json){free(text);free(json);close(fd);file_error(job,ENOMEM);return;}
    size_t used=0,limit=st.st_size>(off_t)FILE_TEXT_LIMIT?FILE_TEXT_LIMIT:(size_t)st.st_size;int failed=0;
    while(used<limit){ssize_t n=read(fd,text+used,limit-used);
        if(n<0&&errno==EINTR)continue;if(n<0){failed=1;break;}if(!n)break;used+=(size_t)n;}
    close(fd);if(failed){free(text);free(json);file_error(job,EIO);return;}
    int prefix=snprintf(json,256,"{\"total_bytes\":%" PRIuMAX ",\"truncated\":%s,\"text\":\"",(uintmax_t)st.st_size,
        st.st_size>(off_t)used?"true":"false");size_t n=file_escape(text,used,json+prefix);
    memcpy(json+prefix+n,"\"}",3);file_response(job,200,"application/json; charset=utf-8",NULL,json,(size_t)prefix+n+2);
    free(text);free(json);
}

static int file_range(const char *range,uint64_t size,uint64_t *first,uint64_t *last)
{
    if(!range[0]){*first=0;*last=size?size-1:0;return size?0:-1;}
    if(strncmp(range,"bytes=",6)||strchr(range,','))return -1;
    const char *p=range+6;char *end;errno=0;
    if(*p=='-'){
        if(p[1]<'0'||p[1]>'9')return -1;uint64_t suffix=strtoull(p+1,&end,10);
        if(errno||*end||!suffix||!size)return -1;*first=suffix>=size?0:size-suffix;*last=size-1;return 1;
    }
    if(*p<'0'||*p>'9')return -1;*first=strtoull(p,&end,10);if(errno||*end!='-'||*first>=size)return -1;
    p=end+1;if(!*p){*last=size-1;return 1;}if(*p<'0'||*p>'9')return -1;
    *last=strtoull(p,&end,10);if(errno||*end||*last<*first)return -1;if(*last>=size)*last=size-1;return 1;
}

static void file_video(file_job_t *job)
{
    if(strcmp(job->method,"GET")&&strcmp(job->method,"HEAD")){file_response(job,405,"text/plain",NULL,NULL,0);return;}
    char name[256];if(!file_query(job->uri,"name",name,sizeof(name))||!file_relative(name)||!file_kind(name+9,1)){
        file_error(job,EINVAL);return;}
    int fd=file_open_relative(PATH_RECORDINGS,name);if(fd<0){file_error(job,errno);return;}
    if(!file_mp4_closed(fd,name)){close(fd);file_error(job,EAGAIN);return;}
    struct stat st;if(fstat(fd,&st)){close(fd);file_error(job,EIO);return;}
    char range[256]="",extra[512];file_header(job->request,"Range",range,sizeof(range));
    uint64_t first,last;int partial=file_range(range,(uint64_t)st.st_size,&first,&last);
    if(partial<0){snprintf(extra,sizeof(extra),"Content-Range: bytes */%" PRIuMAX "\r\nAccept-Ranges: bytes\r\n",(uintmax_t)st.st_size);
        close(fd);file_response(job,416,"video/mp4",extra,NULL,0);return;}
    int n=snprintf(extra,sizeof(extra),"Accept-Ranges: bytes\r\nContent-Disposition: inline\r\n");
    if(partial)snprintf(extra+n,sizeof(extra)-(size_t)n,"Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIuMAX "\r\n",first,last,(uintmax_t)st.st_size);
    file_response(job,partial?206:200,"video/mp4",extra,NULL,(size_t)(last-first+1));
    if(strcmp(job->method,"HEAD")){
        unsigned char bytes[65536];uint64_t remaining=last-first+1,offset=first,started=file_now();
        while(remaining){size_t wanted=remaining>sizeof(bytes)?sizeof(bytes):(size_t)remaining;
            ssize_t got=pread(fd,bytes,wanted,(off_t)offset);if(got<0&&errno==EINTR)continue;
            if(got<=0||file_send(job->fd,bytes,(size_t)got,started))break;remaining-=(uint64_t)got;offset+=(uint64_t)got;}
    }
    close(fd);
}

static int file_write(int fd,const void *bytes,size_t size)
{
    size_t at=0;while(at<size){ssize_t n=write(fd,(const char*)bytes+at,size-at);if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;at+=(size_t)n;}return 0;
}

static int file_copy(int source,int target,off_t size)
{
    char buffer[65536];off_t offset=0;while(offset<size){size_t wanted=size-offset>(off_t)sizeof(buffer)?sizeof(buffer):(size_t)(size-offset);
        ssize_t n=pread(source,buffer,wanted,offset);if(n<0&&errno==EINTR)continue;if(n<=0){errno=EIO;return -1;}
        if(file_write(target,buffer,(size_t)n))return -1;offset+=n;}return 0;
}

static void file_remove_tree(const char *path)
{
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
    DIR *dir=fdopendir(fd);if(!dir){close(fd);return;}struct dirent *entry;
    while((entry=readdir(dir))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        struct stat st;if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        if(S_ISDIR(st.st_mode)){char child[PATH_MAX];if(snprintf(child,sizeof(child),"%s/%s",path,entry->d_name)<(int)sizeof(child))file_remove_tree(child);}
        else(void)unlinkat(fd,entry->d_name,0);
    }
    closedir(dir);(void)rmdir(path);
}

static int file_archive_command(const char *stage,int output)
{
    posix_spawn_file_actions_t actions;int rc=posix_spawn_file_actions_init(&actions);if(rc){errno=rc;return -1;}
    rc=posix_spawn_file_actions_adddup2(&actions,output,STDOUT_FILENO);
    if(!rc)rc=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    if(!rc)rc=posix_spawn_file_actions_addchdir_np(&actions,stage);
    /* H.264 files are already compressed. Stored ZIP needs no recompression,
       supports ZIP64 and opens directly in desktop file managers. */
    char *args[]={"zip","-q","-0","-r","-D","-",".",NULL};pid_t child;
    extern char **environ;if(!rc)rc=posix_spawnp(&child,"zip",&actions,NULL,args,environ);
    posix_spawn_file_actions_destroy(&actions);if(rc){errno=rc;return -1;}
    int status;do{rc=waitpid(child,&status,0);}while(rc<0&&errno==EINTR);
    if(rc>=0&&WIFEXITED(status)&&WEXITSTATUS(status)==12){
        /* Info-ZIP returns 12 for an empty selection. Return a valid empty ZIP. */
        static const unsigned char empty[22]={0x50,0x4b,0x05,0x06};
        if(ftruncate(output,0)||lseek(output,0,SEEK_SET)<0)return -1;
        return file_write(output,empty,sizeof(empty));
    }
    if(rc<0||!WIFEXITED(status)||WEXITSTATUS(status)){errno=EIO;return -1;}return 0;
}

/* Clear only regular entries in the managed day folders. Archive immutable
 * videos by hard link on the same filesystem; live text is a fixed-size copy. */
static int file_walk(const char *root,const char *stage,const char *category,int kind,int clear,
                     unsigned *count,uint64_t *total)
{
    int rootfd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(rootfd<0)return errno==ENOENT?0:-1;
    DIR *dir=fdopendir(rootfd);if(!dir){close(rootfd);return -1;}int result=0;struct dirent *day;
    while(!result && (day=readdir(dir))){if(!file_day(day->d_name))continue;
        int d=openat(rootfd,day->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(d<0)continue;
        DIR *files=fdopendir(d);if(!files){close(d);continue;}struct dirent *entry;
        while(!result && (entry=readdir(files))){if(!file_kind(entry->d_name,kind)||entry->d_name[0]=='.')continue;
            char relative[256];if(snprintf(relative,sizeof(relative),"%s/%s",day->d_name,entry->d_name)>=(int)sizeof(relative)||!file_relative(relative))continue;
            int source=openat(d,entry->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(source<0)continue;
            struct stat info;if(fstat(source,&info)||!S_ISREG(info.st_mode)){close(source);continue;}
            if(clear){
                close(source);
                /* Incomplete orphan MP4s are deletable too. Only the recorder
                   knows whether a path is open, being published or finalized. */
                int removed = kind==1 ? video_rec_delete_file(d,relative) : unlinkat(d,entry->d_name,0);
                if(removed<0){if(errno==ENOENT)continue;result=-1;break;}
                if(!removed)(*count)++;
                continue;
            }
            if(kind==1&&!file_mp4_closed(source,relative)){close(source);continue;}
            if(kind==2){close(source);source=yolo_events_export_report(relative);if(source<0){result=-1;break;}
                if(fstat(source,&info)){close(source);result=-1;break;}}
            if((uint64_t)info.st_size>FILE_PACK_LIMIT-*total){close(source);errno=EFBIG;result=-1;break;}
            char folder[PATH_MAX],target[PATH_MAX];
            if(snprintf(folder,sizeof(folder),"%s/%s",stage,category)>=(int)sizeof(folder)){close(source);errno=ENAMETOOLONG;result=-1;break;}
            if(mkdir(folder,0700)&&errno!=EEXIST){close(source);result=-1;break;}
            if(snprintf(folder,sizeof(folder),"%s/%s/%s",stage,category,day->d_name)>=(int)sizeof(folder)){
                close(source);errno=ENAMETOOLONG;result=-1;break;}
            if(mkdir(folder,0700)&&errno!=EEXIST){close(source);result=-1;break;}
            if(snprintf(target,sizeof(target),"%s/%s",folder,entry->d_name)>=(int)sizeof(target)){
                close(source);errno=ENAMETOOLONG;result=-1;break;}
            int linked=0;
            if(kind==1){
                if(!linkat(d,entry->d_name,AT_FDCWD,target,0)){
                    struct stat linked_info;if(!lstat(target,&linked_info)&&S_ISREG(linked_info.st_mode)&&
                        linked_info.st_dev==info.st_dev&&linked_info.st_ino==info.st_ino)linked=1;
                    else(void)unlink(target);
                }
            }
            if(!linked){int targetfd=open(target,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
                if(targetfd<0){close(source);result=-1;break;}
                off_t size=info.st_size;
                if(kind==0&&size){char tail[4096];size_t n=size>(off_t)sizeof(tail)?sizeof(tail):(size_t)size;
                    ssize_t got=pread(source,tail,n,size-(off_t)n);if(got>0){ssize_t at=got;while(at>0&&tail[at-1]!='\n')at--;size-=(got-at);}}
                result=file_copy(source,targetfd,size);int error=errno;close(targetfd);errno=error;
            }
            close(source);if(!result){*total+=(uint64_t)info.st_size;(*count)++;}
        }
        closedir(files);
    }
    int error=errno;closedir(dir);errno=error;return result;
}

static void file_bulk(file_job_t *job,int clear)
{
    unsigned logs=0,recordings=0,results=0;uint64_t total=0;int output=-1,error=0;
    char stage[PATH_MAX]="";
    pthread_mutex_lock(&file_bulk_lock);
    if(clear){
        if(file_walk(job->log_root,NULL,NULL,0,1,&logs,&total)||
           file_walk(PATH_RECORDINGS,NULL,NULL,1,1,&recordings,&total)||yolo_events_clear_reports(&results))error=errno;
    }else{
        snprintf(stage,sizeof(stage),"%s/.files-snapshot-XXXXXX",PATH_RECORDINGS);
        if(!mkdtemp(stage))error=errno;
        if(!error && (file_walk(job->log_root,stage,"logs",0,0,&logs,&total)||
            file_walk(PATH_RECORDINGS,stage,"recordings",1,0,&recordings,&total)||
            file_walk(YOLO_EVENTS_DIR,stage,"results",2,0,&results,&total)))error=errno;
        if(!error&&!logs&&!recordings&&!results)error=ENOENT;
        if(!error){char name[PATH_MAX];snprintf(name,sizeof(name),"%s/.files-pack-XXXXXX",PATH_RECORDINGS);
            output=mkstemp(name);if(output<0)error=errno;else{unlink(name);if(file_archive_command(stage,output))error=errno;}}
        if(stage[0])file_remove_tree(stage);
    }
    pthread_mutex_unlock(&file_bulk_lock);
    if(error){if(output>=0)close(output);file_error(job,error);return;}
    if(clear){char json[256];int n=snprintf(json,sizeof(json),"{\"deleted\":%u,\"logs\":%u,\"recordings\":%u,\"results\":%u,\"kept_active\":true}",
        logs+recordings+results,logs,recordings,results);file_response(job,200,"application/json",NULL,json,(size_t)n);return;}
    struct stat info;if(fstat(output,&info)||lseek(output,0,SEEK_SET)<0){close(output);file_error(job,EIO);return;}
    file_response(job,200,"application/zip","Content-Disposition: attachment; filename=\"autonomous-driving-car_files.zip\"\r\n",NULL,(size_t)info.st_size);
    char bytes[65536];uint64_t started=file_now();ssize_t n;
    while((n=read(output,bytes,sizeof(bytes)))!=0){if(n<0&&errno==EINTR)continue;if(n<0||file_send(job->fd,bytes,(size_t)n,started))break;}
    close(output);
}

static void *file_worker(void *argument)
{
    file_job_t *job=argument;(void)nice(10);int flags=fcntl(job->fd,F_GETFL);
    if(flags>=0&&fcntl(job->fd,F_SETFL,flags|O_NONBLOCK)>=0){
        if(!strcmp(job->uri,"/api/files/video-session")||!strcmp(job->uri,"/api/files/download-session"))file_session(job);
        else if(!strncmp(job->uri,"/api/files/video?",17))file_video(job);
        else if(!strncmp(job->uri,"/api/files/preview?",19))file_preview(job);
        else if(!strcmp(job->uri,"/api/files/pack")&&!strcmp(job->method,"GET"))file_bulk(job,0);
        else if(!strcmp(job->uri,"/api/files/clear")&&!strcmp(job->method,"POST"))file_bulk(job,1);
        else file_response(job,405,"text/plain",NULL,NULL,0);
    }
    /* Return admission before close: the OS may immediately reuse the fd. */
    if(job->on_close)job->on_close(job->fd);close(job->fd);free(job->request);free(job);
    pthread_mutex_lock(&file_worker_lock);file_workers--;pthread_mutex_unlock(&file_worker_lock);return NULL;
}

/* The reactor detaches before starting this worker. Failure retains socket
 * ownership in the caller, which sends a short error and closes it once. */
int http_files_start(app_ctx_t *app,int fd,const char *method,const char *uri,const char *request,void(*on_close)(int))
{
    pthread_mutex_lock(&file_worker_lock);
    if(file_workers>=FILE_WORKERS){pthread_mutex_unlock(&file_worker_lock);errno=EBUSY;return -1;}
    file_workers++;pthread_mutex_unlock(&file_worker_lock);
    file_job_t *job=calloc(1,sizeof(*job));int error=ENOMEM;
    if(!job)goto failed;job->fd=fd;job->on_close=on_close;
    safe_strncpy(job->method,sizeof(job->method),method);safe_strncpy(job->uri,sizeof(job->uri),uri);
    safe_strncpy(job->log_root,sizeof(job->log_root),app&&app->cfg?app->cfg->log_dir:PATH_LOGS);
    job->request=strndup(request,32768);if(!job->request){free(job);goto failed;}
    pthread_attr_t attr;error=pthread_attr_init(&attr);
    if(!error){error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);pthread_t thread;
        if(!error)error=pthread_create(&thread,&attr,file_worker,job);pthread_attr_destroy(&attr);if(!error)return 0;}
    free(job->request);free(job);
failed:
    pthread_mutex_lock(&file_worker_lock);file_workers--;pthread_mutex_unlock(&file_worker_lock);errno=error;return -1;
}
