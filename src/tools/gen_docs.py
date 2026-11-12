#!/usr/bin/env python3
"""Write the HDLCGW documentation site's pages from the repository.

    python3 src/tools/gen_docs.py [OUT]          (OUT defaults to build-site)
    mkdocs build --strict -f build-site/mkdocs.yml -d <a folder outside the checkout>

What it writes (OUT is wiped first; it is build output, never committed):
  OUT/docs/index.md             the README
  OUT/docs/<path>.md            the hand-written guides and BUGS.md, at their own paths, so the
                                relative links between them keep working
  OUT/docs/api/index.md         the API index: every header, every call
  OUT/docs/api/<path>.md        one page per public header, from its comments
  OUT/docs/assets/              the icon and the site's CSS
  OUT/mkdocs.yml                the generated menu; the settings come from
                                src/docs-site/mkdocs.yml (INHERIT)

Only files git tracks are used, so the site never shows a file a clean checkout lacks.
The text of a reference page is the header's own comments: the banner at the top and the
/** ... */ block before each declaration. A declaration without one gets an empty
description, never an invented one; the fix is a comment in the header.
"""
import html
import os
import re
import shutil
import subprocess
import sys

import gen_toc   # same folder; Python puts the script's folder on the path

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
REPO = 'https://github.com/RetroCoreLabs/HDLCGW'
BLOB = REPO + '/blob/master/'

# The libraries and what the reader calls them, in menu order.
LIBRARIES = [
    ('src/HDLCGW', 'HDLCGW'),
    ('src/XMSGGW', 'XMSGGW'),
]

# The hand-written pages, in menu order, with their menu titles. Left out on purpose:
# src/HDLCGW/CLAUDE.md (notes for the AI assistant, not for readers) and kicad/todo.md (a
# work list). BUGS.md goes in as "Known defects".
GUIDES = [
    ('src/HDLCGW/README.md', 'HDLCGW firmware'),
    ('src/HDLCGW/LOOPBACK-TEST.md', 'HDLCGW loopback test'),
    ('src/XMSGGW/README.md', 'XMSGGW firmware'),
    ('docs/README.md', 'Hardware notes'),
    ('docs/HDLC-3015-vs-MEGALINK-3023.md', 'HDLC 3015 and MegaLink 3023'),
    ('docs/HDLC-Cable-Measurements.md', 'Cable measurements'),
    ('kicad/README.md', 'Circuit board'),
]
BUGS = 'src/HDLCGW/BUGS.md'

# Headers of the two sketches only; the libraries under src/external have their own site.
def headers_of_sketches():
    return [p for folder, _ in LIBRARIES for p in tracked(folder + '/*.h')]


def tracked(pattern):
    out = subprocess.run(['git', '-C', ROOT, 'ls-files', '--', pattern], check=True,
                         capture_output=True, text=True).stdout
    return sorted(p for p in out.splitlines() if p)


# ------------------------------------------------------------------------------------------
# Reading a header
# ------------------------------------------------------------------------------------------

def comment_text(raw):
    """The words of a /* */ block: no delimiters, no leading ' * '."""
    body = raw[3:] if raw.startswith('/**') else raw[2:]
    body = body[:-2] if body.endswith('*/') else body
    lines = body.split('\n')
    out = []
    for ln in lines:
        s = ln.rstrip()
        if re.match(r'^\s*\*+\s*$', s):
            out.append('')  # a box edge (hot_path.h draws its banner in a box of stars)
            continue
        s = re.sub(r'\s*\*\*+$', '', s)  # the right edge of a box
        m = re.match(r'^\s*\*+ ?(.*)$', s)
        out.append(m.group(1) if m else s.strip())
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


def scan(text):
    """Top-level items of a header: (kind, name, code, doc_lines, line) in order."""
    items = []
    includes = []
    doc = None
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if text.startswith('/**', i) and not text.startswith('/**<', i):
            j = text.index('*/', i + 3) + 2
            doc = comment_text(text[i:j])
            i = j
            continue
        if text.startswith('/*', i):
            i = text.index('*/', i + 2) + 2
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '#':
            j = i
            while True:
                k = text.find('\n', j)
                if k < 0:
                    k = n
                    break
                if text[k - 1] == '\\':
                    j = k + 1
                    continue
                # A trailing comment that carries on to the next line belongs to this line.
                tail = text[i:k]
                if tail.rfind('/*') > tail.rfind('*/'):
                    j = text.index('*/', k) + 2
                    continue
                break
            line = text[i:k]
            m = re.match(r'#\s*include\s+"([^"]+)"', line)
            if m:
                includes.append(m.group(1))
            m = re.match(r'#\s*define\s+(\w+)(\([^)]*\))?\s*(.*)$', line, re.S)
            if m and not m.group(1).endswith('_H') and m.group(1) != 'XMSG_LOG':
                value = m.group(3)
                trail = None
                t = re.search(r'/\*\*?<?\s*(.*?)\s*\*/\s*$', value, re.S)
                if t:
                    trail = [t.group(1)]
                    value = value[:t.start()].rstrip()
                items.append(('macro', m.group(1), (m.group(2) or '') + ' ' + value.strip(),
                              doc or trail, line_of(text, i)))
            doc = None
            i = k
            continue
        m = re.compile(r'extern\s+"C"\s*\{').match(text, i)
        if m:
            i = m.end()
            continue
        if c == '}':
            i += 1
            continue
        # A statement: to ';' at depth 0, or to the '}' that ends an inline function body.
        start = i
        depth = 0
        while i < n:
            if text.startswith('/*', i):
                i = text.index('*/', i + 2) + 2
                continue
            if text.startswith('//', i):
                j = text.find('\n', i)
                i = n if j < 0 else j
                continue
            ch = text[i]
            if ch in '{(':
                depth += 1
            elif ch in '})':
                depth -= 1
                if depth == 0 and ch == '}':
                    head = text[start:i]
                    first = head.split('{')[0]
                    if '(' in first and not re.match(r'\s*(typedef|struct|enum|union)\b', first):
                        i += 1
                        break
            elif ch == ';' and depth == 0:
                i += 1
                break
            i += 1
        code = text[start:i].strip()
        item = classify(code)
        if item:
            items.append((item[0], item[1], code, doc, line_of(text, start)))
        doc = None
    return items, includes


def merge(items):
    """One entry per name. A type declared ahead and defined later, or a call with one body
    per platform, shows every piece of its code under one heading, with the first doc block."""
    out = []
    seen = {}
    for it in items:
        kind, name, code, doc, line = it
        if name in seen:
            k = seen[name]
            old = out[k]
            out[k] = (old[0], name, old[2] + '\n\n' + code, old[3] or doc, old[4])
            continue
        seen[name] = len(out)
        out.append(it)
    return out


def classify(code):
    flat = re.sub(r'/\*.*?\*/', ' ', code, flags=re.S)
    flat = re.sub(r'\s+', ' ', flat).strip()
    if flat.startswith('typedef'):
        m = re.search(r'\(\s*\*\s*(\w+)\s*\)', flat)
        if m and '{' not in flat:
            return ('type', m.group(1))
        m = re.search(r'(\w+)\s*(\[[^\]]*\])?\s*;\s*$', flat)
        return ('type', m.group(1)) if m else None
    m = re.match(r'(struct|enum|union)\s+(\w+)\s*\{', flat)
    if m:
        return ('type', m.group(2))
    if '(' in flat:
        for m in re.finditer(r'(\w+)\s*\(', flat):
            if m.group(1) not in ('__attribute__', 'format', 'sizeof'):
                return ('function', m.group(1))
        return None
    m = re.search(r'(\w+)\s*(\[[^\]]*\])?\s*;\s*$', flat)
    return ('variable', m.group(1)) if m else None


# ------------------------------------------------------------------------------------------
# Writing Markdown
# ------------------------------------------------------------------------------------------

def esc(text):
    """Comment text for Markdown: code spans kept, anything that Markdown or HTML would take
    as markup outside them made literal."""
    parts = text.split('`')
    for k in range(0, len(parts), 2):
        p = html.escape(parts[k], quote=False)
        p = re.sub(r'([*_\[\]\\])', r'\\\1', p)
        parts[k] = p
    if len(parts) % 2 == 0:  # an unpaired backtick: keep it literal
        last = parts.pop()
        return '`'.join(parts) + '\\`' + html.escape(last, quote=False)
    return '`'.join(parts)


def prose(lines):
    """Comment lines as Markdown. A paragraph that is a list or a laid-out table (lines starting
    with '-', or indented further than its first line) keeps its line breaks in a text block;
    plain text is joined into paragraphs."""
    out = []
    para = []

    def flush():
        if not para:
            return
        laid_out = any(re.match(r'^\s*(-|\d+[.)]?\s)', p) for p in para) or \
            any(len(p) - len(p.lstrip()) > len(para[0]) - len(para[0].lstrip()) + 1
                for p in para[1:])
        if laid_out:
            out.append('```text\n' + '\n'.join(para) + '\n```')
        else:
            out.append(esc(' '.join(p.strip() for p in para)))
        para.clear()

    for ln in lines:
        if not ln.strip():
            flush()
        else:
            para.append(ln.rstrip())
    flush()
    return '\n\n'.join(out)


def split_doc(lines):
    """@brief / @param / @return of a doc block; everything else is the description."""
    brief = []
    desc = []
    params = []
    ret = []
    cur = desc
    for ln in lines or []:
        s = ln.strip()
        m = re.match(r'@brief\s*(.*)$', s)
        if m:
            brief.append(m.group(1))
            cur = brief
            continue
        m = re.match(r'@param(\[[a-z,]+\])?\s+(\w+)\s*(.*)$', s)
        if m:
            params.append([m.group(2), [m.group(3)]])
            cur = params[-1][1]
            continue
        m = re.match(r'@returns?\s*(.*)$', s)
        if m:
            ret.append(m.group(1))
            cur = ret
            continue
        if cur is brief and not s:
            cur = desc
        if cur is brief or cur is ret or (params and cur is params[-1][1]):
            if s:
                cur.append(s)
            else:
                cur = desc
            continue
        desc.append(ln)
    if not brief and desc:
        # No @brief: the first sentence-paragraph is the summary.
        first = []
        while desc and desc[0].strip():
            first.append(desc.pop(0).strip())
        brief = first
    return brief, desc, params, ret


def summary(lines):
    brief, _, _, _ = split_doc(lines)
    return esc(' '.join(brief))


def cell(text):
    return text.replace('|', '\\|').replace('\n', ' ')


def page_for(path):
    rel = path[len('src/'):]
    return 'api/' + rel[:-2] + '.md'


def link_between(src_page, dst_page):
    return os.path.relpath(dst_page, os.path.dirname(src_page)).replace(os.sep, '/')


def write_header_page(path, text, items, includes, used_by, out_docs):
    page = page_for(path)
    name = os.path.basename(path)
    banner_m = re.match(r'\s*/\*.*?\*/', text, re.S)
    banner = comment_text(banner_m.group(0)) if banner_m else []
    purpose = ''
    if banner:
        first = []
        while banner and banner[0].strip():
            first.append(banner.pop(0).strip())
        purpose = ' '.join(first)
        m = re.match(r'[\w.]+\s+-\s+(.*)$', purpose)
        if m:
            purpose = m.group(1)
        purpose = purpose[:1].upper() + purpose[1:]
    banner = [b for b in banner if not re.match(r'\s*(SPDX-License-Identifier|Copyright)', b)]
    md = ['<!-- generated by src/tools/gen_docs.py from %s; edit the comments there -->' % path,
          '# %s' % name, '']
    if purpose:
        md += [esc(purpose), '']
    md += ['Source: [`%s`](%s%s)' % (path, BLOB, path), '']
    body = prose(banner)
    if body:
        md += [body, '']

    funcs = [it for it in items if it[0] == 'function']
    types = [it for it in items if it[0] == 'type']
    macros = [it for it in items if it[0] == 'macro']
    vars_ = [it for it in items if it[0] == 'variable']

    if funcs:
        md += ['## Calls', '', '| Call | What it does |', '|---|---|']
        for kind, nm, code, doc, ln in funcs:
            md.append('| [`%s`](#%s) | %s |' % (nm, nm.lower(), cell(summary(doc))))
        md.append('')
    if includes or used_by:
        md += ['## Uses and used by', '']
        if includes:
            md.append('Uses: ' + ', '.join(
                '[`%s`](%s)' % (os.path.basename(i), link_between(page, page_for(os.path.dirname(path) + '/' + i)))
                if os.path.exists(os.path.join(ROOT, os.path.dirname(path), i)) else '`%s`' % i
                for i in includes))
            md.append('')
        if used_by:
            md.append('Used by: ' + ', '.join(
                '[`%s`](%s)' % (os.path.basename(u), link_between(page, page_for(u)))
                for u in used_by))
            md.append('')

    def entry(kind, nm, code, doc, ln):
        brief, desc, params, ret = split_doc(doc)
        e = ['### `%s` { #%s }' % (nm, nm.lower()), '',
             '```c', code, '```', '']
        if brief:
            e += [esc(' '.join(brief)), '']
        d = prose(desc)
        if d:
            e += [d, '']
        if params:
            e += ['| Parameter | |', '|---|---|']
            for pn, pt in params:
                e.append('| `%s` | %s |' % (pn, cell(esc(' '.join(pt)))))
            e.append('')
        if ret:
            e += ['**Returns:** ' + esc(' '.join(ret)), '']
        e += ['<small>[%s line %d](%s%s#L%d)</small>' % (name, ln, BLOB, path, ln), '']
        return e

    if funcs:
        md += ['## Call reference', '']
        for it in funcs:
            md += entry(*it)
    if types:
        md += ['## Types', '']
        for it in types:
            md += entry(*it)
    if vars_:
        md += ['## Variables', '']
        for it in vars_:
            md += entry(*it)
    if macros:
        md += ['## Constants and macros', '', '| Name | Value | What it is |', '|---|---|---|']
        for kind, nm, code, doc, ln in macros:
            md.append('| `%s` | `%s` | %s |' % (nm, cell(code.strip()) or ' ',
                                               cell(esc(' '.join(d.strip() for d in (doc or []))))))
        md.append('')

    md += ['??? note "The whole header"', '',
           '    ```c']
    md += ['    ' + ln if ln else '' for ln in text.rstrip('\n').split('\n')]
    md += ['    ```', '']
    dest = os.path.join(out_docs, page)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(md))
    return purpose, funcs


def copy_page(src, dest):
    """Copy one hand-written page into the site, without gen_toc.py's contents list."""
    with open(os.path.join(ROOT, src), encoding='utf-8') as f:
        text = f.read()
    with open(dest, 'w', encoding='utf-8', newline='\n') as f:
        f.write(gen_toc.strip(text))


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build-site'))
    # On Windows the output may be on another drive than the checkout, and commonpath then
    # raises; another drive is outside the checkout by definition.
    inside = os.path.splitdrive(out)[0].lower() == os.path.splitdrive(ROOT)[0].lower() \
        and os.path.commonpath([out, ROOT]) == ROOT
    if inside and not os.path.basename(out).startswith('build'):
        sys.exit('the output folder inside the checkout must be a build* folder (gitignored)')
    if os.path.exists(out):
        shutil.rmtree(out)
    docs = os.path.join(out, 'docs')
    os.makedirs(docs)

    # Hand-written pages, at their own paths.
    # The contents list gen_toc.py writes into each big file is left out: the site shows its
    # own beside every page, and two lists on one page are noise.
    copy_page('README.md', os.path.join(docs, 'index.md'))
    known = set(tracked('*.md'))
    guides = [p for p, _ in GUIDES if p in known] + ([BUGS] if BUGS in known else [])
    for p in guides:
        dest = os.path.join(docs, p)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        copy_page(p, dest)
    os.makedirs(os.path.join(docs, 'assets'))
    shutil.copyfile(os.path.join(ROOT, 'src/assets/hdlcgw.svg'),
                    os.path.join(docs, 'assets/hdlcgw.svg'))
    # The pictures the pages show, at the path the README uses.
    for p in tracked('docs/*.svg'):
        dest = os.path.join(docs, p)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, p), dest)
    for f in ('extra.css', 'mermaid-init.js'):
        shutil.copyfile(os.path.join(ROOT, 'src/docs-site', f), os.path.join(docs, 'assets', f))

    # Reference pages.
    headers = headers_of_sketches()
    parsed = {}
    for p in headers:
        with open(os.path.join(ROOT, p), encoding='utf-8') as f:
            text = f.read()
        items, includes = scan(text)
        parsed[p] = (text, merge(items), includes)
    used_by = {p: [] for p in headers}
    for p, (_, _, includes) in parsed.items():
        for i in includes:
            dep = os.path.dirname(p) + '/' + i
            if dep in used_by:
                used_by[dep].append(p)

    index_rows = {}
    all_calls = []
    for p in headers:
        text, items, includes = parsed[p]
        purpose, funcs = write_header_page(p, text, items, includes, used_by[p], docs)
        index_rows[p] = (purpose, len(funcs))
        for it in funcs:
            all_calls.append((it[1], p, summary(it[3])))

    nav_api = []
    md = ['<!-- generated by src/tools/gen_docs.py; edit the header comments and regenerate -->',
          '# API index', '',
          'Every header of the two firmware sketches, and every call in them. Each page is '
          'made from the comments in the header itself.', '']
    for folder, label in LIBRARIES:
        mine = [p for p in headers if p.startswith(folder + '/')]
        if not mine:
            continue
        md += ['## %s' % label, '', '| Header | What it is | Calls |', '|---|---|---|']
        group = []
        for p in mine:
            purpose, count = index_rows[p]
            md.append('| [`%s`](%s) | %s | %s |' % (p[len(folder) + 1:],
                                                    link_between('api/index.md', page_for(p)),
                                                    cell(esc(purpose)), count or ''))
            group.append((p[len(folder) + 1:], page_for(p)))
        md.append('')
        nav_api.append((label, group))
    others = [p for p in headers if not any(p.startswith(f + '/') for f, _ in LIBRARIES)]
    if others:
        md += ['## Shared', '', '| Header | What it is |', '|---|---|']
        for p in others:
            md.append('| [`%s`](%s) | %s |' % (p[len('src/'):],
                                              link_between('api/index.md', page_for(p)),
                                              cell(esc(index_rows[p][0]))))
        md.append('')
        nav_api.append(('Shared', [(p[len('src/'):], page_for(p)) for p in others]))
    md += ['## All calls', '', '| Call | Header | What it does |', '|---|---|---|']
    for nm, p, s in sorted(all_calls):
        md.append('| [`%s`](%s#%s) | `%s` | %s |' % (nm, link_between('api/index.md', page_for(p)),
                                                     nm.lower(), os.path.basename(p), cell(s)))
    md.append('')
    with open(os.path.join(docs, 'api/index.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(md))

    # The menu.
    y = ['# generated by src/tools/gen_docs.py; the settings are in the inherited file',
         'INHERIT: %s' % os.path.relpath(os.path.join(ROOT, 'src/docs-site/mkdocs.yml'),
                                         out).replace(os.sep, '/'),
         'docs_dir: docs',
         'hooks:',
         '  - %s' % os.path.relpath(os.path.join(ROOT, 'src/docs-site/hooks.py'),
                                    out).replace(os.sep, '/'),
         'nav:',
         '  - Home: index.md',
         '  - API index: api/index.md']
    guide_pages = [(p, t) for p, t in GUIDES if p in guides]
    if guide_pages:
        y.append('  - Guides:')
        for p, t in guide_pages:
            y.append('      - "%s": %s' % (t, p))
    y.append('  - API:')
    for label, group in nav_api:
        y.append('      - %s:' % label)
        for title, pg in group:
            y.append('          - "%s": %s' % (title, pg))
    if BUGS in guides:
        y.append('  - Known defects: %s' % BUGS)
    with open(os.path.join(out, 'mkdocs.yml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(y) + '\n')
    print('%d header pages, %d calls, %d guide pages -> %s' %
          (len(headers), len(all_calls), len(guides), out))


if __name__ == '__main__':
    main()
