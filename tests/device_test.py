"""End-to-end test of the web file manager plugin on a console. Usage: python3 tests/device_test.py CONSOLE_IP[:PORT]
Works in a throwaway folder on the SD card and removes it at the end."""
import hashlib
import http.client
import json
import os
import socket
import sys
import threading
import time
import urllib.parse

if len(sys.argv) < 2:
    sys.exit('usage: python3 tests/device_test.py CONSOLE_IP[:PORT]')
HOST = sys.argv[1]
H, P = (HOST.split(':') + ['8888'])[:2]
P = int(P)
BASE = '/webfm_full_test'
results = []


def req(method, path, params=None, body=None, headers=None, raw=False, timeout=60):
    q = urllib.parse.urlencode(params or {}, quote_via=urllib.parse.quote)
    h = dict(headers or {})
    if method in ('PUT', 'POST'):
        h.setdefault('X-Webfm', '1')
    c = http.client.HTTPConnection(H, P, timeout=timeout)
    c.request(method, path + ('?' + q if q else ''), body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    c.close()
    if raw:
        return r.status, data, r
    try:
        return r.status, json.loads(data) if data else None
    except ValueError:
        return r.status, data


def check(name, ok, detail=''):
    results.append((name, ok, detail))
    print(('PASS ' if ok else 'FAIL ') + name + (('  ' + str(detail)) if detail else ''), flush=True)


def listing(path):
    st, j = req('GET', '/api/list', {'path': path})
    return st, {e['n']: e for e in j['entries']} if st == 200 else {}


def put(path, data, headers=None, **kw):
    return req('PUT', '/api/put', {'path': path}, body=data, headers=headers, **kw)


def sha(b):
    return hashlib.sha256(b).hexdigest()


try:
    st, j = req('GET', '/api/space')
    check('space', st == 200 and j['total'] > j['free'] > 0, '%.1f GB free' % (j['free'] / 1e9))
    st, body, r = req('GET', '/', raw=True)
    check('page served', st == 200 and b'Switch 2 Files' in body and 'text/html' in r.getheader('Content-Type'), '%d bytes' % len(body))
    check('page has security headers', 'default-src' in (r.getheader('Content-Security-Policy') or '') and r.getheader('X-Frame-Options') == 'DENY')
    st, b, r = req('GET', '/favicon.ico', raw=True)
    check('favicon 204', st == 204)

    st, j = req('POST', '/api/mkdir', {'path': BASE})
    check('mkdir', st == 200)
    st, j = req('POST', '/api/mkdir', {'path': BASE})
    check('mkdir existing -> 409', st == 409)
    st, j = req('POST', '/api/mkdir', {'path': BASE + '/a/b/c'})
    check('mkdir missing parent -> error', st in (409, 500), st)

    # small, empty, binary, unicode, special names
    names = {'plain.txt': b'hello', 'empty.bin': b'', 'with space.txt': b'sp', 'hash#pct%25.txt': b'h',
             'café üö.txt': 'latin'.encode(), 'абв.txt': 'cyrillic'.encode(),
             'smile😀.txt': 'emoji'.encode(), 'a&b=c.txt': b'amp', 'semi;colon+plus.txt': b'sc'}
    st, j = put(BASE + '/quote"name.txt', b'q')
    check('name with " refused by the card format (409)', st == 409, st)
    st, j = put(BASE + '/日本語.txt', b'x')
    check('CJK name refused by the console file system (409, same as FTP)', st == 409, st)
    for n, d in names.items():
        st, j = put(BASE + '/' + n, d)
        if st != 200:
            check('put ' + repr(n), False, (st, j))
            continue
        st, got, r = req('GET', '/api/get', {'path': BASE + '/' + n}, raw=True)
        check('put+get ' + repr(n), st == 200 and got == d and r.getheader('Content-Length') == str(len(d)), '%d bytes' % len(d))
    st, ents = listing(BASE)
    ascii_names = {n: d for n, d in names.items() if n.isascii()}
    check('listing has every ASCII name with the right size', all(n in ents and ents[n]['s'] == len(d) and ents[n]['d'] == 0 for n, d in ascii_names.items()), len(ents))
    hidden = [n for n in names if not n.isascii() and n not in ents]
    check('console file system hides non-ASCII names from listings (known limit, still reachable by name)', len(hidden) == len([n for n in names if not n.isascii()]), '%d hidden' % len(hidden))

    # overwrite
    st, j = put(BASE + '/plain.txt', b'second version, longer')
    st, got, _ = req('GET', '/api/get', {'path': BASE + '/plain.txt'}, raw=True)
    check('overwrite replaces content', got == b'second version, longer')
    st, ents = listing(BASE)
    check('no .wfm-part leftovers', not [n for n in ents if n.endswith('.wfm-part')])

    # inline text + download headers
    st, got, r = req('GET', '/api/get', {'path': BASE + '/plain.txt', 'inline': '1'}, raw=True)
    check('inline is text/plain', 'text/plain' in r.getheader('Content-Type') and not r.getheader('Content-Disposition'))
    st, got, r = req('GET', '/api/get', {'path': BASE + '/café üö.txt'}, raw=True)
    check('download has UTF-8 filename', "filename*=UTF-8''" in (r.getheader('Content-Disposition') or ''), r.getheader('Content-Disposition'))

    # large files, speed and integrity
    for mb in (20, 100):
        data = os.urandom(mb * 1024 * 1024)
        t = time.time()
        st, j = put(BASE + '/big%d.bin' % mb, data, timeout=600)
        up = time.time() - t
        t = time.time()
        st2, got, _ = req('GET', '/api/get', {'path': BASE + '/big%d.bin' % mb}, raw=True, timeout=600)
        down = time.time() - t
        check('large file %d MB integrity' % mb, st == 200 and st2 == 200 and sha(got) == sha(data),
              'up %.1f MB/s, down %.1f MB/s' % (mb / up, mb / down))

    # folders: nested create, rename, move, recursive delete
    for d in ('f1', 'f1/f2', 'f1/f2/f3'):
        req('POST', '/api/mkdir', {'path': BASE + '/' + d})
    put(BASE + '/f1/f2/f3/deep.txt', b'deep')
    st, j = req('POST', '/api/rename', {'from': BASE + '/f1', 'to': BASE + '/f1renamed'})
    st2, got, _ = req('GET', '/api/get', {'path': BASE + '/f1renamed/f2/f3/deep.txt'}, raw=True)
    check('rename folder keeps contents', st == 200 and got == b'deep')
    st, j = req('POST', '/api/rename', {'from': BASE + '/plain.txt', 'to': BASE + '/f1renamed/moved.txt'})
    st2, got, _ = req('GET', '/api/get', {'path': BASE + '/f1renamed/moved.txt'}, raw=True)
    check('move file into folder', st == 200 and got == b'second version, longer')
    st, j = req('POST', '/api/rename', {'from': BASE + '/f1renamed/moved.txt', 'to': BASE + '/plain.txt'})
    check('rename back', st == 200)
    st, j = req('POST', '/api/rename', {'from': BASE + '/plain.txt', 'to': BASE + '/empty.bin'})
    check('rename onto existing -> 409', st == 409)
    st, j = req('POST', '/api/rename', {'from': BASE + '/nothere', 'to': BASE + '/x'})
    check('rename missing -> 404', st == 404)

    # concurrency: 3 slow-ish downloads in parallel while a 4th gets 503 or waits
    big = BASE + '/big100.bin'
    out = {}

    def dl(i):
        t = time.time()
        st, data, _ = req('GET', '/api/get', {'path': big}, raw=True, timeout=600)
        out[i] = (st, len(data), time.time() - t)
    th = [threading.Thread(target=dl, args=(i,)) for i in range(3)]
    for t in th:
        t.start()
    time.sleep(1.0)
    st4 = None
    try:
        st4, _ = req('GET', '/api/space')
    except Exception as e:
        st4 = repr(e)
    for t in th:
        t.join()
    check('3 parallel downloads complete', all(v[0] == 200 and v[1] == 100 * 1024 * 1024 for v in out.values()),
          ', '.join('%.1f MB/s' % (v[1] / 1e6 / v[2]) for v in out.values()))
    check('4th connection while 3 busy -> 503 (or served)', st4 in (503, 200), st4)

    # protocol hardening
    st, j = req('POST', '/api/delete', {'path': BASE + '/plain.txt'}, headers={'X-Webfm': '0'})
    check('mutation with wrong header value -> 403', st == 403)
    st, j = req('GET', '/api/space', headers={'Host': 'evil.example'})
    check('bad Host -> 403', st == 403)
    st, j = req('GET', '/api/space', headers={'Host': H + '.evil.com'})
    check('lookalike Host -> 403', st == 403)
    st, j = req('GET', '/api/list', {'path': '/../etc'})
    check('.. in path -> 400', st == 400)
    st, j = req('GET', '/api/list', {'path': 'relative'})
    check('relative path -> 400', st == 400)
    st, j = req('GET', '/api/list', {'path': BASE + '/nothere'})
    check('list missing folder -> 404', st == 404)
    st, j = req('GET', '/api/get', {'path': BASE + '/nothere.txt'})
    check('get missing file -> 404', st == 404)
    st, j = req('GET', '/api/get', {'path': BASE})
    check('get a folder -> error, not a crash', st in (404, 400, 500), st)
    st, j = put(BASE, b'x')
    check('put onto a folder -> 409', st == 409)
    st, j = put(BASE + '/nofolder/x.txt', b'x')
    check('put into missing folder -> 409', st == 409)
    st, j = put('/', b'x')
    check('put on root -> 400', st == 400)
    st, j = req('POST', '/api/delete', {'path': '/'})
    check('delete root -> 400', st == 400)
    st, j = req('DELETE', '/api/get', {'path': '/x'}, headers={'X-Webfm': '1'})
    check('unknown method -> 4xx', 400 <= st < 500, st)
    st, j = req('GET', '/api/nothing')
    check('unknown api -> 405 or 404', st in (404, 405), st)
    st, j = req('GET', '/nothing')
    check('unknown page -> 404', st == 404)
    st, j = req('POST', '/api/mkdir', {'path': BASE + '/' + 'x' * 300})
    check('very long name handled', st in (200, 400, 409, 500), st)
    req('POST', '/api/delete', {'path': BASE + '/' + 'x' * 300})

    # raw socket abuse
    def raw_send(data, wait=2.0):
        s = socket.create_connection((H, P), timeout=10)
        s.sendall(data)
        s.settimeout(wait)
        try:
            resp = s.recv(4096)
        except socket.timeout:
            resp = b''
        s.close()
        return resp
    r1 = raw_send(b'PUT /api/put?path=/webfm_full_test/chunk.txt HTTP/1.1\r\nHost: %s\r\nX-Webfm: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n' % HOST.encode())
    check('chunked upload -> 411', b' 411 ' in r1.split(b'\r\n')[0], r1.split(b'\r\n')[0])
    r2 = raw_send(b'GET /' + b'a' * 7000 + b' HTTP/1.1\r\nHost: %s\r\n\r\n' % HOST.encode())
    check('oversized head -> 431', b' 431 ' in r2.split(b'\r\n')[0], r2.split(b'\r\n')[0])
    r3 = raw_send(b'garbage\r\n\r\n')
    check('garbage request -> 400', b' 400 ' in r3.split(b'\r\n')[0], r3.split(b'\r\n')[0])
    r4 = raw_send(b'PUT /api/put?path=/webfm_full_test/trunc.bin HTTP/1.1\r\nHost: %s\r\nX-Webfm: 1\r\nContent-Length: 1000000\r\n\r\nonly a few bytes' % HOST.encode(), wait=0.5)
    time.sleep(0.5)
    st, ents = listing(BASE)
    check('aborted upload leaves no partial file', 'trunc.bin' not in ents and not [n for n in ents if n.endswith('.wfm-part')], [n for n in ents if 'trunc' in n or n.endswith('.part')])

    # stalled client must not block the server
    stall = socket.create_connection((H, P), timeout=10)
    stall.sendall(b'GET /api/space HTTP/1.1\r\nHost: ' + HOST.encode() + b'\r\n')
    t = time.time()
    st, j = req('GET', '/api/space')
    check('server answers while a client stalls', st == 200 and time.time() - t < 5, '%.2fs' % (time.time() - t))
    stall.close()

    # big directory listing
    t = time.time()
    st, ents = listing('/retroarch/assets/ozone/png/icons')
    check('large directory listing', st == 200 and len(ents) > 100, '%d entries in %.2fs' % (len(ents), time.time() - t))
    t = time.time()
    st, ents = listing('/switch')
    check('/switch listing', st == 200, '%d entries in %.2fs' % (len(ents), time.time() - t))

    # recursive delete
    st, j = req('POST', '/api/delete', {'path': BASE + '/f1renamed'})
    st2, ents = listing(BASE)
    check('recursive delete', st == 200 and 'f1renamed' not in ents)
finally:
    for n in names:
        if not n.isascii():
            req('POST', '/api/delete', {'path': BASE + '/' + n})
    st, j = req('POST', '/api/delete', {'path': BASE}, timeout=120)
    st2, ents = listing('/')
    check('cleanup: test folder removed', BASE.strip('/') not in ents, st)
    st, j = req('GET', '/api/space')
    check('server still healthy at the end', st == 200)

failed = [r for r in results if not r[1]]
print('\n%d checks, %d failed' % (len(results), len(failed)))
for n, ok, d in failed:
    print('FAILED:', n, d)
sys.exit(1 if failed else 0)
