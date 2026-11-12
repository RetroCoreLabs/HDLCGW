#!/usr/bin/env python3
"""Write a contents list into every Markdown file that is not tiny.

    python3 src/tools/gen_toc.py            write or refresh the lists
    python3 src/tools/gen_toc.py --check    change nothing; fail if a list is missing or stale

Which files: every Markdown file git tracks with MIN_LINES lines or more. A file under that
gets no list, and a list left in one that has shrunk is taken out again.

Where: just before the file's first "## " heading, between the two marker comments below. The
list holds the "##" and "###" headings, linked with github.com's anchors (lower case, spaces to
"-", punctuation dropped, "-1", "-2" for repeats), which the docs site uses too (mkdocs.yml:
pymdownx.slugs.slugify, case lower). Headings inside ``` fences are not headings.

The site shows its own contents list beside each page, so gen_docs.py takes this one out of
the copies it publishes (strip()).

Run from anywhere; it works on the repository this file is in.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# A file with fewer lines than this is short enough to read without a list.
MIN_LINES = 100

START = '<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->'
END = '<!-- /contents -->'
BLOCK = re.compile(re.escape(START) + r'.*?' + re.escape(END) + r'\n\n?', re.S)


def tracked():
    out = subprocess.run(['git', '-C', ROOT, 'ls-files', '*.md'], capture_output=True,
                         text=True, check=True).stdout
    return [p for p in out.splitlines() if p]


def strip(text):
    """The file without its contents list."""
    return BLOCK.sub('', text)


def plain(heading):
    """The heading as github.com renders it to text: no code ticks, links as their text."""
    t = re.sub(r'!\[([^\]]*)\]\([^)]*\)', r'\1', heading)
    t = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', t)
    t = t.replace('`', '').replace('**', '').replace('__', '')
    return t.strip()


def slug(text):
    s = text.lower()
    s = re.sub(r'[^\w\- ]', '', s)
    return s.replace(' ', '-')


def headings(text):
    """(level, text, anchor) for every ## and ### heading outside code fences."""
    seen = {}
    out = []
    fence = None
    for line in text.split('\n'):
        m = re.match(r'^\s*(```+|~~~+)', line)
        if m:
            mark = m.group(1)[0] * 3
            if fence is None:
                fence = mark
            elif mark == fence:
                fence = None
            continue
        if fence is not None:
            continue
        m = re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', line)
        if not m:
            continue
        level = len(m.group(1))
        name = plain(m.group(2))
        base = slug(name)
        n = seen.get(base, 0)
        seen[base] = n + 1
        anchor = base if n == 0 else '%s-%d' % (base, n)
        if level in (2, 3):
            out.append((level, name, anchor))
    return out


def with_list(text):
    """The file as it should be: a fresh list for a big file, none for a small one."""
    body = strip(text)
    if body.count('\n') < MIN_LINES:
        return body
    hs = headings(body)
    if len(hs) < 2:
        return body
    lines = [START, '**Contents**', '']
    for level, name, anchor in hs:
        lines.append('%s- [%s](#%s)' % ('  ' * (level - 2), name, anchor))
    lines += ['', END, '', '']
    block = '\n'.join(lines)
    pos = first_h2(body)
    return body[:pos] + block + body[pos:]


def first_h2(text):
    """Where the first "## " heading outside a code fence starts; the end when there is none."""
    pos = 0
    fence = None
    for line in text.split('\n'):
        m = re.match(r'^\s*(```+|~~~+)', line)
        if m:
            mark = m.group(1)[0] * 3
            fence = mark if fence is None else (None if mark == fence else fence)
        elif fence is None and line.startswith('## '):
            return pos
        pos += len(line) + 1
    return len(text)


def main():
    check = '--check' in sys.argv[1:]
    stale = []
    for p in tracked():
        path = os.path.join(ROOT, p)
        with open(path, encoding='utf-8', newline='') as f:
            raw = f.read()
        crlf = '\r\n' in raw
        text = raw.replace('\r\n', '\n')
        new = with_list(text)
        if new == text:
            continue
        stale.append(p)
        if not check:
            if crlf:
                new = new.replace('\n', '\r\n')
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
    for p in stale:
        print(('stale contents list: ' if check else 'contents list written: ') + p)
    if check and stale:
        sys.exit('run: python3 src/tools/gen_toc.py')


if __name__ == '__main__':
    main()
