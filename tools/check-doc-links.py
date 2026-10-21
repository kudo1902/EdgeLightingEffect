#!/usr/bin/env python3
"""Check the documentation's links.

Every relative link in a tracked .md or .html file (Markdown links, href, src,
url()) must name a file that exists, and a #fragment on one must name a heading
or an id in it; every docs/... path written anywhere in the tree (code comments
included) must exist - except one written as a git revision (`f153e1c:docs/...`), which
names a file in history. Exits non-zero on any failure.

    python3 tools/check-doc-links.py [REPO_ROOT]
"""
import os, re, subprocess, sys, html
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
os.chdir(root)
tracked = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard'], capture_output=True, text=True).stdout.split('\n')
tracked = [f for f in tracked if f and os.path.exists(f) and not f.startswith('external/')]
docs = [f for f in tracked if f.endswith(('.md', '.html'))]
text_ext = ('.md', '.html', '.h', '.cpp', '.c', '.frag', '.vert', '.glsl', '.txt', '.in', '.py', '.json', '.cmake')
def slug(h):
    h = re.sub(r'<[^>]+>', '', h).strip().lower()
    h = re.sub(r'[`*_~]', '', h)  # markdown emphasis/code markers
    h = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', h)
    out = ''.join(c for c in h if c.isalnum() or c in ' -_')
    return out.replace(' ', '-')
anchor_cache = {}
def anchors(path):
    if path in anchor_cache: return anchor_cache[path]
    s = open(path, encoding='utf-8', errors='replace').read()
    a = set()
    if path.endswith('.md'):
        seen = {}
        incode = False
        for line in s.split('\n'):
            if line.startswith('```'): incode = not incode
            if incode: continue
            m = re.match(r'#{1,6}\s+(.*)', line)
            if m:
                b = slug(m.group(1)); n = seen.get(b, 0); seen[b] = n + 1
                a.add(b if n == 0 else f'{b}-{n}')
        a |= set(re.findall(r'<a\s+(?:name|id)="([^"]+)"', s))
    else:
        a |= set(re.findall(r'\bid="([^"]+)"', s))
    anchor_cache[path] = a
    return a
bad = []
for f in docs:
    s = open(f, encoding='utf-8', errors='replace').read()
    d = os.path.dirname(f)
    targets = []
    if f.endswith('.md'):
        # drop fenced code blocks
        s2 = re.sub(r'```.*?```', '', s, flags=re.S)
        targets += re.findall(r'\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\)', s2)
        targets += re.findall(r'(?:href|src)="([^"]+)"', s2)
    else:
        targets += re.findall(r'(?:href|src)="([^"]+)"', s)
        targets += re.findall(r"url\(\s*['\"]?([^'\")$]+)['\"]?\s*\)", s)
    for t in targets:
        t = html.unescape(t)
        if re.match(r'^[a-z]+:', t) or t.startswith('//') or '${' in t: continue
        path, _, frag = t.partition('#')
        path = path.split('?')[0]
        if path == '':
            if frag and f.endswith(('.md', '.html')) and frag not in anchors(f):
                bad.append((f, t, 'anchor')); 
            continue
        full = os.path.normpath(os.path.join(d, path))
        if not os.path.exists(full):
            bad.append((f, t, 'missing')); continue
        if frag and full.endswith(('.md', '.html')) and frag not in anchors(full):
            bad.append((f, t, 'anchor'))
# docs/... path mentions anywhere
for f in tracked:
    if not f.endswith(text_ext) and os.path.basename(f) not in ('CMakeLists.txt',): continue
    try: s = open(f, encoding='utf-8').read()
    except Exception: continue
    for m in re.finditer(r'(?<![\w.:-])((?:\.\./)*docs/[A-Za-z0-9_./-]*?\.(?:md|html|png))', s):
        p = m.group(1)
        cand = [os.path.normpath(os.path.join(os.path.dirname(f), p)), os.path.normpath(re.sub(r'^(\.\./)+', '', p))]
        if not any(os.path.exists(c) for c in cand):
            bad.append((f, p, 'path'))
from collections import Counter
print('files checked:', len(docs), 'problems:', len(bad), dict(Counter(k for _, _, k in bad)))
for b in bad: print('  %-8s %s -> %s' % (b[2], b[0], b[1]))
sys.exit(1 if bad else 0)
