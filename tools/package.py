"""Builds the SD-card zip of the release: python3 tools/package.py [version]
Needs webfm.nso (make). Writes dist/webmanager-nx2-<version>.zip and dist/SHA256SUMS.txt."""
import hashlib
import os
import sys
import zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
version = sys.argv[1] if len(sys.argv) > 1 else '1.0.0'
nso = os.path.join(root, 'webfm.nso')
if not os.path.exists(nso):
    sys.exit('webfm.nso is missing: run make first')

os.makedirs(os.path.join(root, 'dist'), exist_ok=True)
out = os.path.join(root, 'dist', 'webmanager-nx2-%s.zip' % version)
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    i = zipfile.ZipInfo('kaihen/plugins/webfm.nso', (2026, 1, 1, 0, 0, 0))
    i.compress_type = zipfile.ZIP_DEFLATED
    i.external_attr = 0o644 << 16
    z.writestr(i, open(nso, 'rb').read())
sha = hashlib.sha256(open(out, 'rb').read()).hexdigest()
open(os.path.join(root, 'dist', 'SHA256SUMS.txt'), 'w').write('%s  %s\n' % (sha, os.path.basename(out)))
print(out)
print(sha)
