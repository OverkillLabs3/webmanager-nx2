"""A stand-in for the plugin's HTTP API on a PC folder, to try the web page in a browser.
Usage: python3 tests/mock_server.py FOLDER [PORT]   (default port 8899, listens on 127.0.0.1 only)"""
import json
import os
import shutil
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
PAGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'source', 'ui.html')


def real(path):
    parts = [p for p in path.split('/') if p]
    if '..' in parts:
        raise ValueError('bad path')
    return os.path.join(ROOT, *parts)


class H(BaseHTTPRequestHandler):
    def send(self, code, body, ctype='application/json', extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def args(self):
        q = urllib.parse.urlparse(self.path)
        return q.path, {k: v[0] for k, v in urllib.parse.parse_qs(q.query).items()}

    def do_GET(self):
        path, a = self.args()
        if path in ('/', '/index.html'):
            return self.send(200, open(PAGE, 'rb').read(), 'text/html; charset=utf-8')
        if path == '/api/space':
            u = shutil.disk_usage(ROOT)
            return self.send(200, {'free': u.free, 'total': u.total})
        try:
            p = real(a.get('path', '/'))
        except ValueError:
            return self.send(400, {'error': 'Invalid path'})
        if path == '/api/list':
            if not os.path.isdir(p):
                return self.send(404, {'error': 'Cannot open folder'})
            ents = [{'n': n, 'd': int(os.path.isdir(os.path.join(p, n))), 's': 0 if os.path.isdir(os.path.join(p, n)) else os.path.getsize(os.path.join(p, n))} for n in os.listdir(p)]
            return self.send(200, {'path': '/' + os.path.relpath(p, ROOT).replace('\\', '/').strip('.') if p != ROOT else '/', 'entries': ents})
        if path == '/api/get' and os.path.isfile(p):
            ctype = 'text/plain; charset=utf-8' if a.get('inline') == '1' else 'application/octet-stream'
            return self.send(200, open(p, 'rb').read(), ctype)
        self.send(404, {'error': 'Not found'})

    def do_PUT(self):
        path, a = self.args()
        if self.headers.get('X-Webfm') != '1':
            return self.send(403, {'error': 'Missing X-Webfm header'})
        n = int(self.headers.get('Content-Length', 0))
        data = self.rfile.read(n)
        p = real(a['path'])
        if not os.path.isdir(os.path.dirname(p)):
            return self.send(409, {'error': 'Cannot create file (folder missing or card full)'})
        open(p, 'wb').write(data)
        self.send(200, {'ok': True})

    def do_POST(self):
        path, a = self.args()
        if self.headers.get('X-Webfm') != '1':
            return self.send(403, {'error': 'Missing X-Webfm header'})
        if path == '/api/mkdir':
            p = real(a['path'])
            if os.path.exists(p):
                return self.send(409, {'error': 'Already exists'})
            os.mkdir(p)
        elif path == '/api/delete':
            p = real(a['path'])
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        elif path == '/api/rename':
            os.rename(real(a['from']), real(a['to']))
        else:
            return self.send(404, {'error': 'Not found'})
        self.send(200, {'ok': True})

    def log_message(self, *a):
        pass


ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2]) if len(sys.argv) > 2 else 8899), H).serve_forever()
