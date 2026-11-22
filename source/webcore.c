// SPDX-License-Identifier: MIT
#include "webcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool web_url_decode(const char *in, size_t n, char *out, size_t cap) {
    if (cap == 0) return false;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= n) return false;
            int h = hexv(in[i + 1]), l = hexv(in[i + 2]);
            if (h < 0 || l < 0) return false;
            c = (unsigned char)((h << 4) | l);
            i += 2;
        }
        if (c < 0x20 || c == 0x7f) return false;
        if (o + 1 >= cap) return false;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return true;
}

bool web_path_clean(const char *in, char *out, size_t cap) {
    if (!in || in[0] != '/' || cap < 2) return false;
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') return false;
        if (o + 1 + len + 1 > cap) return false;
        out[o++] = '/';
        memcpy(out + o, s, len);
        o += len;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    return true;
}

bool web_host_ok(const char *host) {
    char h[128];
    size_t n = strlen(host);
    if (n == 0 || n >= sizeof h) return false;
    memcpy(h, host, n + 1);
    char *colon = strchr(h, ':');
    if (colon) {
        *colon = 0;
        if (colon[1] == 0 || strlen(colon + 1) > 5) return false;
        for (const char *p = colon + 1; *p; p++)
            if (*p < '0' || *p > '9') return false;
    }
    if (strcmp(h, "localhost") == 0) return true;
    int parts = 0;
    const char *p = h;
    while (*p) {
        int digits = 0, value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 3) return false;
        }
        if (digits == 0 || value > 255) return false;
        parts++;
        if (*p == '.') {
            p++;
            if (!*p) return false;
        } else if (*p) {
            return false;
        }
    }
    return parts == 4;
}

bool web_query_get(const char *query, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *end = strchr(p, '&');
        size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg > klen && memcmp(p, key, klen) == 0 && p[klen] == '=')
            return web_url_decode(p + klen + 1, seg - klen - 1, out, cap);
        if (!end) break;
        p = end + 1;
    }
    return false;
}

size_t web_json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char tmp[8];
        size_t len;
        if (*p == '"' || *p == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)*p;
            len = 2;
        } else if (*p < 0x20) {
            len = (size_t)snprintf(tmp, sizeof tmp, "\\u%04x", *p);
        } else {
            tmp[0] = (char)*p;
            len = 1;
        }
        if (o + len + 1 > cap) return (size_t)-1;
        memcpy(out + o, tmp, len);
        o += len;
    }
    if (o + 1 > cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

size_t web_url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                     *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t len = plain ? 1 : 3;
        if (o + len + 1 > cap) return (size_t)-1;
        if (plain) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    if (o + 1 > cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

static bool header_is(const char *line, size_t name_len, const char *name) {
    if (name_len != strlen(name)) return false;
    for (size_t i = 0; i < name_len; i++) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != name[i]) return false;
    }
    return true;
}

bool web_parse_head(const char *head, WebReq *r) {
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    const char *p = head;
    const char *sp = strchr(p, ' ');
    if (!sp || sp == p || (size_t)(sp - p) >= sizeof r->method) return false;
    memcpy(r->method, p, (size_t)(sp - p));
    p = sp + 1;
    sp = strchr(p, ' ');
    const char *eol = strstr(p, "\r\n");
    if (!sp || !eol || sp > eol || sp == p) return false;
    if (strncmp(sp + 1, "HTTP/1.", 7) != 0) return false;
    const char *q = memchr(p, '?', (size_t)(sp - p));
    size_t plen = (size_t)((q ? q : sp) - p);
    if (plen == 0 || plen >= sizeof r->path || p[0] != '/') return false;
    memcpy(r->path, p, plen);
    if (q) {
        size_t qlen = (size_t)(sp - q - 1);
        if (qlen >= sizeof r->query) return false;
        memcpy(r->query, q + 1, qlen);
    }
    p = eol + 2;
    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        eol = strstr(p, "\r\n");
        if (!eol) return false;
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon) return false;
        size_t nlen = (size_t)(colon - p);
        const char *v = colon + 1;
        while (v < eol && (*v == ' ' || *v == '\t')) v++;
        size_t vlen = (size_t)(eol - v);
        while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
        if (header_is(p, nlen, "content-length")) {
            if (vlen == 0 || vlen > 18) return false;
            int64_t n = 0;
            for (size_t i = 0; i < vlen; i++) {
                if (v[i] < '0' || v[i] > '9') return false;
                n = n * 10 + (v[i] - '0');
            }
            r->content_length = n;
        } else if (header_is(p, nlen, "host")) {
            if (vlen >= sizeof r->host) return false;
            memcpy(r->host, v, vlen);
        } else if (header_is(p, nlen, "x-webfm")) {
            r->webfm = vlen == 1 && v[0] == '1';
        } else if (header_is(p, nlen, "transfer-encoding")) {
            r->chunked = true;
        }
        p = eol + 2;
    }
    return true;
}
