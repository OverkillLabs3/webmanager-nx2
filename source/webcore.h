// SPDX-License-Identifier: MIT
// Pure helpers of the web file manager: no console calls, so they build and test on a PC.
#ifndef WEBCORE_H
#define WEBCORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char method[8];
    char path[512];    // request path, still percent-encoded, without the query
    char query[1024];  // raw query string without '?'
    char host[128];
    int64_t content_length;  // -1 when absent
    bool webfm;        // header "X-Webfm: 1" present (a page of another site cannot send it)
    bool chunked;
} WebReq;

// Percent-decodes n bytes. Fails on a bad escape, a control character, a NUL or when cap is too small.
bool web_url_decode(const char *in, size_t n, char *out, size_t cap);
// Makes a clean absolute path: collapses "//" and "/./". Fails on "..", on a relative path or on overflow.
bool web_path_clean(const char *in, char *out, size_t cap);
// Accepts a dotted IPv4 address or "localhost", with an optional ":port". Refuses host names (DNS rebinding).
bool web_host_ok(const char *host);
// Reads and percent-decodes the value of key from a raw query string.
bool web_query_get(const char *query, const char *key, char *out, size_t cap);
// JSON string escape. Returns the length written, or (size_t)-1 when cap is too small.
size_t web_json_escape(const char *in, char *out, size_t cap);
// Parses the request line and the headers of a request head (NUL terminated, lines end in CRLF).
bool web_parse_head(const char *head, WebReq *r);
// Percent-encodes a file name for a Content-Disposition filename* value.
size_t web_url_encode(const char *in, char *out, size_t cap);
#endif
