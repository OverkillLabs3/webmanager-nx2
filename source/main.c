// SPDX-License-Identifier: MIT
//
// webfm: a web file manager for the SD card, built as a kaiHEN plugin
// (/kaihen/plugins/webfm.nso). It starts at boot and listens on port 8888.
//
// Requests that change anything (PUT, POST) need the header "X-Webfm: 1", which a page of another
// web site cannot send, and every request must name the console by its IP address in the Host
// header, which stops DNS rebinding. There is no password: use it only on a network you trust.

#include <switch.h>
#include <kaihen/plugin.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ui_html.h"
#include "webcore.h"

#define HTTP_PORT          8888
#define MAX_CLIENTS        3
#define LISTEN_BACKLOG     8
#define HEAD_MAX           6144
#define BUF_SIZE           0x10000
#define DIR_BATCH          8
#define SESSION_STACK_SIZE 0x10000
#define SESSION_PRIORITY   0x2C
#define IO_TIMEOUT_S       30
#define ONE_SECOND_NS      1000000000ULL
#define PART_SUFFIX        ".wfm-part"

// Plugin process setup (see <kaihen/plugin.h>).
u32    __nx_applet_type     = AppletType_None;
size_t __nx_heap_size       = 0x200000;
u32    __nx_fs_num_sessions = 3;
void __appInit(void) {}
void __appExit(void) {}

static FsFileSystem g_sd;

typedef struct {
    int      fd;
    char     head[HEAD_MAX];
    size_t   body_off, body_len;  // body bytes that arrived together with the head
    WebReq   req;
    char     path[FS_MAX_PATH];
    char     path2[FS_MAX_PATH];
    char     esc[FS_MAX_PATH * 6 + 8];
    size_t   olen;
    bool     oerr;
    FsDirectoryEntry dirents[DIR_BATCH];
    u8       buf[BUF_SIZE];
} Session;

enum { SLOT_FREE = 0, SLOT_RUNNING = 1, SLOT_DONE = 2 };
typedef struct {
    Thread thread;
    int    state;
    int    fd;
} Slot;

static Slot    g_slots[MAX_CLIENTS];
static Session g_sess[MAX_CLIENTS];

//-----------------------------------------------------------------------------
// Sockets and replies
//-----------------------------------------------------------------------------

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static bool send_all(int fd, const void *buf, size_t len) {
    const u8 *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

// len < 0: no Content-Length, the body ends when the connection closes. extra holds whole header lines.
static bool send_head(Session *s, int code, const char *ctype, s64 len, const char *extra) {
    char h[768];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n",
                     code, status_text(code), ctype);
    if (len >= 0)
        n += snprintf(h + n, sizeof h - (size_t)n, "Content-Length: %lld\r\n", (long long)len);
    n += snprintf(h + n, sizeof h - (size_t)n, "%s\r\n", extra ? extra : "");
    if (n <= 0 || (size_t)n >= sizeof h)
        return false;
    return send_all(s->fd, h, (size_t)n);
}

static void reply_json(Session *s, int code, const char *json) {
    size_t n = strlen(json);
    if (send_head(s, code, "application/json", (s64)n, NULL))
        send_all(s->fd, json, n);
}

static void reply_error(Session *s, int code, const char *msg) {
    char esc[128], body[192];
    if (web_json_escape(msg, esc, sizeof esc) == (size_t)-1)
        strcpy(esc, "error");
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", esc);
    reply_json(s, code, body);
}

// Buffered output for the streamed directory listing.
static void out_flush(Session *s) {
    if (s->olen && !send_all(s->fd, s->buf, s->olen))
        s->oerr = true;
    s->olen = 0;
}

static void out_put(Session *s, const char *data, size_t len) {
    if (s->oerr)
        return;
    if (len > BUF_SIZE)
        len = BUF_SIZE;
    if (s->olen + len > BUF_SIZE)
        out_flush(s);
    memcpy(s->buf + s->olen, data, len);
    s->olen += len;
}

//-----------------------------------------------------------------------------
// File operations
//-----------------------------------------------------------------------------

static bool path_param(Session *s, const char *key, char *out) {
    char raw[FS_MAX_PATH];
    if (!web_query_get(s->req.query, key, raw, sizeof raw))
        return false;
    return web_path_clean(raw, out, FS_MAX_PATH);
}

static bool entry_type(const char *path, FsDirEntryType *type) {
    return R_SUCCEEDED(fsFsGetEntryType(&g_sd, path, type));
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void h_list(Session *s) {
    char raw[FS_MAX_PATH];
    if (!web_query_get(s->req.query, "path", raw, sizeof raw))
        strcpy(raw, "/");
    if (!web_path_clean(raw, s->path, sizeof s->path))
        return reply_error(s, 400, "Invalid path");
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(&g_sd, s->path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir)))
        return reply_error(s, 404, "Cannot open folder");
    if (!send_head(s, 200, "application/json", -1, NULL)) {
        fsDirClose(&dir);
        return;
    }
    s->olen = 0;
    s->oerr = false;
    out_put(s, "{\"path\":\"", 9);
    if (web_json_escape(s->path, s->esc, sizeof s->esc) != (size_t)-1)
        out_put(s, s->esc, strlen(s->esc));
    out_put(s, "\",\"entries\":[", 13);
    bool first = true, single = false;
    int skipped = 0, fails = 0;
    Result last_error = 0;
    for (;;) {
        s64 count = 0;
        // The file system can fail on an entry it cannot convert (some non-ASCII names). Read one entry at
        // a time from then on and carry on past the failures instead of dropping the rest of the folder.
        Result rc = fsDirRead(&dir, &count, single ? 1 : DIR_BATCH, s->dirents);
        if (R_FAILED(rc)) {
            last_error = rc;
            skipped++;
            single = true;
            if (++fails > 16)
                break;
            continue;
        }
        fails = 0;
        if (count <= 0)
            break;
        for (s64 i = 0; i < count; i++) {
            FsDirectoryEntry *e = &s->dirents[i];
            if (web_json_escape(e->name, s->esc, sizeof s->esc) == (size_t)-1)
                continue;
            char tail[48];
            if (!first)
                out_put(s, ",", 1);
            first = false;
            out_put(s, "{\"n\":\"", 6);
            out_put(s, s->esc, strlen(s->esc));
            int n = snprintf(tail, sizeof tail, "\",\"d\":%d,\"s\":%lld}",
                             e->type == FsDirEntryType_Dir ? 1 : 0, (long long)e->file_size);
            out_put(s, tail, (size_t)n);
        }
    }
    fsDirClose(&dir);
    char end[64];
    int en = snprintf(end, sizeof end, "],\"skipped\":%d,\"err\":%u}", skipped, (unsigned)last_error);
    out_put(s, end, (size_t)en);
    out_flush(s);
}

static void h_get(Session *s) {
    if (!path_param(s, "path", s->path))
        return reply_error(s, 400, "Invalid path");
    FsFile f;
    if (R_FAILED(fsFsOpenFile(&g_sd, s->path, FsOpenMode_Read, &f)))
        return reply_error(s, 404, "Cannot open file");
    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&f, &size))) {
        fsFileClose(&f);
        return reply_error(s, 500, "Cannot read file");
    }
    char extra[FS_MAX_PATH * 3 + 64];
    const char *ctype = "application/octet-stream";
    extra[0] = 0;
    char inl[8];
    if (web_query_get(s->req.query, "inline", inl, sizeof inl) && inl[0] == '1') {
        ctype = "text/plain; charset=utf-8";
    } else {
        char enc[FS_MAX_PATH * 3 + 8];
        if (web_url_encode(base_name(s->path), enc, sizeof enc) != (size_t)-1)
            snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename*=UTF-8''%s\r\n", enc);
    }
    if (send_head(s, 200, ctype, size, extra[0] ? extra : NULL)) {
        s64 off = 0;
        while (off < size) {
            u64 want = (u64)(size - off);
            if (want > BUF_SIZE)
                want = BUF_SIZE;
            u64 got = 0;
            if (R_FAILED(fsFileRead(&f, off, s->buf, want, FsReadOption_None, &got)) || got == 0 ||
                !send_all(s->fd, s->buf, got))
                break;
            off += (s64)got;
        }
    }
    fsFileClose(&f);
}

static void h_put(Session *s) {
    if (!path_param(s, "path", s->path) || strcmp(s->path, "/") == 0)
        return reply_error(s, 400, "Invalid path");
    if (s->req.content_length < 0)
        return reply_error(s, 411, "Content-Length needed");
    if (strlen(s->path) + sizeof PART_SUFFIX >= FS_MAX_PATH)
        return reply_error(s, 400, "Path too long");
    FsDirEntryType type;
    bool exists = entry_type(s->path, &type);
    if (exists && type != FsDirEntryType_File)
        return reply_error(s, 409, "A folder with that name exists");

    // The data goes to a temporary file that replaces the target after the last byte arrived.
    size_t plen = strlen(s->path);
    memcpy(s->path2, s->path, plen);
    memcpy(s->path2 + plen, PART_SUFFIX, sizeof PART_SUFFIX);
    fsFsDeleteFile(&g_sd, s->path2);
    s64 total = s->req.content_length;
    if (R_FAILED(fsFsCreateFile(&g_sd, s->path2, total, 0)))
        return reply_error(s, 409, "Cannot create file (folder missing or card full)");
    FsFile f;
    if (R_FAILED(fsFsOpenFile(&g_sd, s->path2, FsOpenMode_Write | FsOpenMode_Append, &f))) {
        fsFsDeleteFile(&g_sd, s->path2);
        return reply_error(s, 500, "Cannot open file");
    }

    bool ok = true;
    s64 off = 0;
    size_t have = s->body_len;
    if ((s64)have > total)
        have = (size_t)total;
    if (have && R_FAILED(fsFileWrite(&f, 0, s->head + s->body_off, have, FsWriteOption_None)))
        ok = false;
    off = (s64)have;
    while (ok && off < total) {
        size_t want = (size_t)(total - off);
        if (want > BUF_SIZE)
            want = BUF_SIZE;
        ssize_t n = recv(s->fd, s->buf, want, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || R_FAILED(fsFileWrite(&f, off, s->buf, (u64)n, FsWriteOption_None))) {
            ok = false;
            break;
        }
        off += n;
    }
    if (ok && R_FAILED(fsFileFlush(&f)))
        ok = false;
    fsFileClose(&f);
    if (ok) {
        if (exists)
            fsFsDeleteFile(&g_sd, s->path);
        ok = R_SUCCEEDED(fsFsRenameFile(&g_sd, s->path2, s->path));
    }
    if (!ok)
        fsFsDeleteFile(&g_sd, s->path2);
    fsFsCommit(&g_sd);
    if (ok)
        reply_json(s, 200, "{\"ok\":true}");
    else
        reply_error(s, 500, "Upload failed");
}

static void h_mkdir(Session *s) {
    if (!path_param(s, "path", s->path) || strcmp(s->path, "/") == 0)
        return reply_error(s, 400, "Invalid path");
    FsDirEntryType type;
    if (entry_type(s->path, &type))
        return reply_error(s, 409, "Already exists");
    if (R_FAILED(fsFsCreateDirectory(&g_sd, s->path)))
        return reply_error(s, 409, "Cannot create folder");
    fsFsCommit(&g_sd);
    reply_json(s, 200, "{\"ok\":true}");
}

static void h_delete(Session *s) {
    if (!path_param(s, "path", s->path) || strcmp(s->path, "/") == 0)
        return reply_error(s, 400, "Invalid path");
    FsDirEntryType type;
    if (!entry_type(s->path, &type))
        return reply_error(s, 404, "Not found");
    Result rc = type == FsDirEntryType_Dir ? fsFsDeleteDirectoryRecursively(&g_sd, s->path)
                                           : fsFsDeleteFile(&g_sd, s->path);
    fsFsCommit(&g_sd);
    if (R_FAILED(rc))
        return reply_error(s, 500, "Delete failed");
    reply_json(s, 200, "{\"ok\":true}");
}

static void h_rename(Session *s) {
    if (!path_param(s, "from", s->path) || !path_param(s, "to", s->path2) ||
        strcmp(s->path, "/") == 0 || strcmp(s->path2, "/") == 0)
        return reply_error(s, 400, "Invalid path");
    FsDirEntryType type, other;
    if (!entry_type(s->path, &type))
        return reply_error(s, 404, "Not found");
    if (entry_type(s->path2, &other))
        return reply_error(s, 409, "The new name already exists");
    Result rc = type == FsDirEntryType_Dir ? fsFsRenameDirectory(&g_sd, s->path, s->path2)
                                           : fsFsRenameFile(&g_sd, s->path, s->path2);
    fsFsCommit(&g_sd);
    if (R_FAILED(rc))
        return reply_error(s, 500, "Rename failed");
    reply_json(s, 200, "{\"ok\":true}");
}

static void h_space(Session *s) {
    s64 free_b = 0, total_b = 0;
    if (R_FAILED(fsFsGetFreeSpace(&g_sd, "/", &free_b)) || R_FAILED(fsFsGetTotalSpace(&g_sd, "/", &total_b)))
        return reply_error(s, 500, "Cannot read card size");
    char body[96];
    snprintf(body, sizeof body, "{\"free\":%lld,\"total\":%lld}", (long long)free_b, (long long)total_b);
    reply_json(s, 200, body);
}

static void h_ui(Session *s) {
    static const char csp[] =
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; img-src data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n";
    if (send_head(s, 200, "text/html; charset=utf-8", (s64)ui_html_len, csp))
        send_all(s->fd, ui_html, ui_html_len);
}

//-----------------------------------------------------------------------------
// One request per connection
//-----------------------------------------------------------------------------

static void handle(Session *s) {
    size_t n = 0;
    for (;;) {
        if (n >= HEAD_MAX - 1)
            return reply_error(s, 431, "Request too large");
        ssize_t r = recv(s->fd, s->head + n, HEAD_MAX - 1 - n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
        n += (size_t)r;
        s->head[n] = 0;
        char *end = strstr(s->head, "\r\n\r\n");
        if (end) {
            s->body_off = (size_t)(end + 4 - s->head);
            s->body_len = n - s->body_off;
            break;
        }
    }
    if (!web_parse_head(s->head, &s->req))
        return reply_error(s, 400, "Bad request");
    if (!web_host_ok(s->req.host))
        return reply_error(s, 403, "Use the IP address of the console");
    if (s->req.chunked)
        return reply_error(s, 411, "Chunked bodies are not supported");

    const char *m = s->req.method, *p = s->req.path;
    bool get = strcmp(m, "GET") == 0, put = strcmp(m, "PUT") == 0, post = strcmp(m, "POST") == 0;
    if (get && (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0))
        return h_ui(s);
    if (get && strcmp(p, "/favicon.ico") == 0) {
        send_head(s, 204, "text/plain", 0, NULL);
        return;
    }
    if (get && strcmp(p, "/api/list") == 0)
        return h_list(s);
    if (get && strcmp(p, "/api/get") == 0)
        return h_get(s);
    if (get && strcmp(p, "/api/space") == 0)
        return h_space(s);
    if (put || post) {
        if (!s->req.webfm)
            return reply_error(s, 403, "Missing X-Webfm header");
        if (put && strcmp(p, "/api/put") == 0)
            return h_put(s);
        if (post && strcmp(p, "/api/mkdir") == 0)
            return h_mkdir(s);
        if (post && strcmp(p, "/api/delete") == 0)
            return h_delete(s);
        if (post && strcmp(p, "/api/rename") == 0)
            return h_rename(s);
    }
    if (strncmp(p, "/api/", 5) == 0 || strcmp(p, "/") == 0)
        return reply_error(s, 405, "Method not allowed");
    reply_error(s, 404, "Not found");
}

static void session_main(void *arg) {
    Slot *slot = arg;
    Session *s = &g_sess[slot - g_slots];
    s->fd = slot->fd;
    struct timeval tv = { .tv_sec = IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    handle(s);
    close_fd(&s->fd);
    slot->fd = -1;
    slot->state = SLOT_DONE;
}

static void reap_slots(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_slots[i].state == SLOT_DONE) {
            threadWaitForExit(&g_slots[i].thread);
            threadClose(&g_slots[i].thread);
            g_slots[i].state = SLOT_FREE;
        }
    }
}

static void reject(int fd) {
    static const char msg[] =
        "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\nRetry-After: 2\r\n\r\n";
    send(fd, msg, sizeof msg - 1, 0);
    close(fd);
}

static void start_session(int fd) {
    reap_slots();
    Slot *slot = NULL;
    for (int i = 0; i < MAX_CLIENTS && !slot; i++)
        if (g_slots[i].state == SLOT_FREE)
            slot = &g_slots[i];
    if (!slot) {
        reject(fd);
        return;
    }
    slot->fd = fd;
    if (R_FAILED(threadCreate(&slot->thread, session_main, slot, NULL, SESSION_STACK_SIZE, SESSION_PRIORITY, -2))) {
        slot->fd = -1;
        reject(fd);
        return;
    }
    slot->state = SLOT_RUNNING;
    if (R_FAILED(threadStart(&slot->thread))) {
        threadClose(&slot->thread);
        slot->state = SLOT_FREE;
        slot->fd = -1;
        reject(fd);
    }
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, LISTEN_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Waits up to a second for a client. Returns false if the listener is broken and must be rebuilt.
static bool accept_one(int listener, int *failures) {
    struct pollfd p = { .fd = listener, .events = POLLIN };
    int r = poll(&p, 1, 1000);
    if (r == 0 || (r < 0 && errno == EINTR))
        return true;
    if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return false;
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) {
        if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        svcSleepThread(ONE_SECOND_NS / 10);
        return ++*failures < 5;
    }
    *failures = 0;
    start_session(fd);
    return true;
}

static void serve(Handle ready) {
    bool signaled = false;
    int listener = -1, failures = 0;
    for (;;) {
        reap_slots();
        if (listener < 0) {
            listener = make_listener();
            if (!signaled) {
                // Do not hold up the bootstrap if the network is not up yet; the listener is retried below.
                svcSignalEvent(ready);
                signaled = true;
            }
            if (listener < 0) {
                svcSleepThread(ONE_SECOND_NS);
                continue;
            }
            failures = 0;
        }
        if (!accept_one(listener, &failures)) {
            close_fd(&listener);
            svcSleepThread(ONE_SECOND_NS);
        }
    }
}

static Result sockets_init(void) {
    SocketInitConfig cfg = {
        .tcp_tx_buf_size     = 0x10000,
        .tcp_rx_buf_size     = 0x10000,
        .tcp_tx_buf_max_size = 0,
        .tcp_rx_buf_max_size = 0,
        .udp_tx_buf_size     = 0,
        .udp_rx_buf_size     = 0,
        .sb_efficiency       = 4,
        .num_bsd_sessions    = MAX_CLIENTS + 1,
        .bsd_service_type    = BsdServiceType_Auto,
    };
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        cfg.num_bsd_sessions = 2;
        rc = socketInitialize(&cfg);
    }
    return rc;
}

int main(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_slots[i].fd = -1;

    Handle ready;
    Result rc = pluginRuntimeInitialize(&ready);
    if (R_FAILED(rc))
        return rc;

    rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = fsOpenSdCardFileSystem(&g_sd);
        if (R_SUCCEEDED(rc)) {
            rc = sockets_init();
            if (R_SUCCEEDED(rc)) {
                serve(ready);  // does not return
                socketExit();
            }
            fsFsClose(&g_sd);
        }
        fsExit();
    }
    pluginRuntimeExit();
    return rc;
}
