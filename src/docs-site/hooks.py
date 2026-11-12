"""MkDocs hook for the HDLCGW site: links to files that are not pages go to GitHub.

The hand-written pages (the README, the guides, BUGS.md) link to source files, folders,
LICENSE and the .github documents with paths relative to the repository. On github.com those
work; on the site they would be dead. This hook rewrites each relative link whose target is
not a page of the site to the same file on GitHub (blob/master/<path>, or tree/master/<path> for
a folder), and logs a WARNING for a target that exists in neither place, so `--strict` fails.
The README is the site's index.md, so a link to README.md goes there.
"""
import logging
import os
import posixpath
import re

REPO = 'https://github.com/RetroCoreLabs/HDLCGW/'
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
log = logging.getLogger('mkdocs.hooks.hdlcgw')

LINK = re.compile(r'(\]\()([^)\s]+)(\))')


def repo_path(src_uri):
    return 'README.md' if src_uri == 'index.md' else src_uri


def on_page_markdown(markdown, page, config, files):
    here = posixpath.dirname(repo_path(page.file.src_uri))
    pages = {f.src_uri for f in files}

    def fix(m):
        target = m.group(2)
        if re.match(r'^[a-z]+:', target) or target.startswith(('#', '/')):
            return m.group(0)
        path, _, anchor = target.partition('#')
        if not path:
            return m.group(0)
        full = posixpath.normpath(posixpath.join(here, path))
        site = 'index.md' if full == 'README.md' else full
        if site in pages:
            rel = posixpath.relpath(site, posixpath.dirname(page.file.src_uri))
            return m.group(1) + rel + ('#' + anchor if anchor else '') + m.group(3)
        on_disk = os.path.join(ROOT, *full.split('/'))
        if os.path.isdir(on_disk):
            url = REPO + 'tree/master/' + full
        elif os.path.isfile(on_disk):
            url = REPO + 'blob/master/' + full
        else:
            log.warning('%s: link to %s, which is neither a page nor a file in the repository',
                        page.file.src_uri, target)
            return m.group(0)
        return m.group(1) + url + ('#' + anchor if anchor else '') + m.group(3)

    return LINK.sub(fix, markdown)
