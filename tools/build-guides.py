#!/usr/bin/env python3
"""One-time migration of the 3D guides to the shared runtime (kept for reference and re-runs).

For every guide page it:
  * replaces the Google Fonts link with the self-hosted fonts (docs/vendor/fonts/fonts.css);
  * loads docs/vendor/three-kit.r160.min.js, docs/assets/board-data.js and docs/assets/guide-kit.js
    instead of a private 1 MB copy of three.js and of the board textures;
  * rewrites the inline module: imports -> window.THREE_KIT, board data -> window.EEG_BOARD,
    renderer.setAnimationLoop -> GuideKit.loop (render on demand), pixel ratio -> GuideKit.dpr().

Usage: python3 tools/build-guides.py <page.html> <module.js> <steps-spec> <idleFps>
  (01 and 02 were then edited by hand: idleFps:()=>P.some(p=>p.hl.visible)?20:0, the highlight pulse only)
  module.js  readable source of the page module (with ES imports, as in the original guides)
  steps-spec prefix and number of step pictures in docs/img/steps, e.g. a:16
"""
import re
import sys

page, module, steps, idle = sys.argv[1:5]
html = open(page, encoding='utf-8').read()
js = open(module, encoding='utf-8').read()

# ---- head/body HTML: everything before the first <script> of the old page
first = re.search(r'<script\b', html)
shell = html[:first.start()]
shell = re.sub(r'<link rel="preconnect" href="https://fonts.googleapis.com">\s*', '', shell)
shell = re.sub(r'<link rel="stylesheet" href="https://fonts.googleapis.com/[^"]*">',
               '<link rel="stylesheet" href="vendor/fonts/fonts.css">\n'
               '<script src="assets/guide-kit.js"></script>', shell)
shell = shell.replace('<html lang="ru">', f'<html lang="ru" data-steps="{steps}">', 1)
assert 'guide-kit.js' in shell, 'fonts link not found'
# shared look goes after the page's own <style>
i = shell.rfind('</style>') + len('</style>')
shell = shell[:i] + '\n<link rel="stylesheet" href="assets/guide.css">' + shell[i:]
shell = shell.rstrip() + '\n'

# ---- module
js = re.sub(r"^import .*?;\n", '', js, flags=re.M)
lines = js.split('\n')
out, have = [], set()
for ln in lines:
    m = re.match(r'const (PADS|TEX_TOP|TEX_BOT) = ', ln)
    if m:
        have.add(m.group(1))
        continue
    out.append(ln)
js = '\n'.join(out).strip('\n')
header = ('const {THREE, OrbitControls, CSS2DRenderer, CSS2DObject, RoomEnvironment, mergeGeometries} = window.THREE_KIT;\n'
          f"const {{{', '.join(sorted(have))}}} = window.EEG_BOARD;\n")
n = js.count('renderer.setPixelRatio(Math.min(devicePixelRatio,2));')
assert n == 1, n
js = js.replace('renderer.setPixelRatio(Math.min(devicePixelRatio,2));', 'renderer.setPixelRatio(GuideKit.dpr());')
m = re.search(r'renderer\.setAnimationLoop\((.*?)\);\n', js + '\n')
assert m, 'no setAnimationLoop'
js = js[:m.start()] + f'GuideKit.loop(renderer, {m.group(1)}, {{controls, resize, idleFps:{idle}}});\n' + js[m.end():]
js = js.replace("document.getElementById('loading').remove();", '')

doc = (shell +
       '<script src="vendor/three-kit.r160.min.js"></script>\n'
       '<script src="assets/board-data.js"></script>\n'
       '<script type="module">\n' + header + js.rstrip() + '\n</script>\n</body></html>\n')
open(page, 'w', encoding='utf-8').write(doc)
print(page, len(doc))
