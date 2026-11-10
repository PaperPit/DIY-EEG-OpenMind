#!/usr/bin/env python3
"""Check that every local href/src in the site's HTML points to an existing file.

Run from the repository root: python3 tools/check-links.py
External links (http, https, mailto) and in-page anchors are skipped.
"""
import pathlib
import re
import sys
from urllib.parse import unquote, urlsplit

root = pathlib.Path(__file__).resolve().parent.parent
pages = [p for p in root.rglob('*.html') if '.git' not in p.parts]
bad = []
attr = re.compile(r'''(?:href|src)\s*=\s*["']([^"'#][^"']*)["']''', re.I)
for page in pages:
    text = page.read_text(encoding='utf-8', errors='replace')
    # skip template literals with ${...}: they are built at runtime
    for m in attr.finditer(text):
        url = m.group(1).strip()
        if '${' in url or url.startswith(('http:', 'https:', 'mailto:', 'data:', 'javascript:', '//', 'tel:')):
            continue
        path = unquote(urlsplit(url).path)
        if not path:
            continue
        target = (page.parent / path).resolve()
        if not target.exists():
            line = text.count('\n', 0, m.start()) + 1
            bad.append(f'{page.relative_to(root)}:{line}: {url}')
for b in bad:
    print('missing:', b)
print(f'{len(pages)} pages checked, {len(bad)} broken links')
sys.exit(1 if bad else 0)
