// SPDX-License-Identifier: MIT
#include "../source/webcore.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char o[64];
    assert(web_url_decode("a%20b%2Fc", 9, o, sizeof o) && !strcmp(o, "a b/c"));
    assert(!web_url_decode("a%2", 3, o, sizeof o));
    assert(!web_url_decode("a%zz", 4, o, sizeof o));
    assert(!web_url_decode("a%00b", 5, o, sizeof o));
    assert(!web_url_decode("a%0ab", 5, o, sizeof o));
    assert(!web_url_decode("abcdef", 6, o, 6));
    assert(web_url_decode("abcde", 5, o, 6));

    assert(web_path_clean("/", o, sizeof o) && !strcmp(o, "/"));
    assert(web_path_clean("//switch///x/./y/", o, sizeof o) && !strcmp(o, "/switch/x/y"));
    assert(!web_path_clean("/a/../b", o, sizeof o));
    assert(!web_path_clean("/a/..", o, sizeof o));
    assert(!web_path_clean("a/b", o, sizeof o));
    assert(web_path_clean("/..a/b..", o, sizeof o) && !strcmp(o, "/..a/b.."));
    assert(!web_path_clean("/aaaaaaaa", o, 8));

    assert(web_host_ok("192.0.2.10:8888") && web_host_ok("192.0.2.10") && web_host_ok("localhost:8888"));
    assert(!web_host_ok("evil.com") && !web_host_ok("evil.com:8888") && !web_host_ok("192.0.2") && !web_host_ok("192.0.2.256"));
    assert(!web_host_ok("1.2.3.4:") && !web_host_ok("1.2.3.4:x") && !web_host_ok("") && !web_host_ok("1.2.3.4.5"));
    assert(!web_host_ok("192.0.2.10.evil.com") && !web_host_ok("1..2.3"));

    assert(web_query_get("path=%2Fswitch&x=1", "path", o, sizeof o) && !strcmp(o, "/switch"));
    assert(web_query_get("a=1&path=/x%20y", "path", o, sizeof o) && !strcmp(o, "/x y"));
    assert(!web_query_get("apath=1", "path", o, sizeof o));
    assert(!web_query_get("", "path", o, sizeof o));

    assert(web_json_escape("a\"b\\c\n", o, sizeof o) != (size_t)-1 && !strcmp(o, "a\\\"b\\\\c\\u000a"));
    assert(web_json_escape("toolong", o, 4) == (size_t)-1);

    assert(web_url_encode("a b/\xc3\xa9", o, sizeof o) != (size_t)-1 && !strcmp(o, "a%20b%2F%C3%A9"));

    WebReq r;
    assert(web_parse_head("PUT /api/put?path=%2Fa HTTP/1.1\r\nHost: 192.0.2.10:8888\r\nContent-Length: 12\r\nX-WebFM: 1\r\n\r\n", &r));
    assert(!strcmp(r.method, "PUT") && !strcmp(r.path, "/api/put") && !strcmp(r.query, "path=%2Fa"));
    assert(r.content_length == 12 && r.webfm && !r.chunked && !strcmp(r.host, "192.0.2.10:8888"));
    assert(web_parse_head("GET / HTTP/1.1\r\nhost:  x \r\n\r\n", &r) && !strcmp(r.path, "/") && r.content_length == -1 && !r.webfm);
    assert(!web_parse_head("GET / HTTP/2\r\n\r\n", &r));
    assert(!web_parse_head("GET  HTTP/1.1\r\n\r\n", &r));
    assert(!web_parse_head("GET /\r\n\r\n", &r));
    assert(!web_parse_head("POST /x HTTP/1.1\r\nContent-Length: 12a\r\n\r\n", &r));
    assert(web_parse_head("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &r) && r.chunked);
    assert(!web_parse_head("GET /x HTTP/1.1\r\nbadheader\r\n\r\n", &r));
    printf("webcore tests passed\n");
    return 0;
}
