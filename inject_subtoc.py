#!/usr/bin/env python3
"""Add per-page subsection entries to the mdBook sidebar.

mdBook's sidebar only lists one entry per chapter file, so numbered
subsections inside a page (e.g. 3.8.5) are not selectable. This script
post-processes ONE built edition directory: it collects every page's <h2>
headings and inserts them into every page's sidebar as a nested
<ol class="section"> of anchor links, using the exact markup mdBook itself
emits for nested chapters — so the stock book.js fold toggle and the
".chapter li:not(.expanded) + li > ol" hide rule apply natively (subsections
show for the active page, and fold open elsewhere via the ❱ toggle).

Must run right after that edition's `mdbook build`, BEFORE nested editions
are built into subdirectories (it scans *.html recursively). Idempotent via
the data-subtoc marker.
"""
import re
import sys
from pathlib import Path

MARKER = 'data-subtoc="1"'
SKIP = {"print.html", "404.html"}


def collect_headings(root):
    """rel posix path -> [(anchor_id, plain_text), ...] for pages with h2s."""
    pages = {}
    for f in sorted(root.rglob("*.html")):
        if f.name in SKIP:
            continue
        html = f.read_text(encoding="utf-8")
        m = re.search(r"<main>(.*?)</main>", html, re.S)
        if not m:
            continue
        heads = re.findall(
            r'<h2 id="([^"]+)"><a class="header" href="#[^"]*">(.*?)</a></h2>',
            m.group(1), re.S)
        entries = [(hid, re.sub(r"<[^>]+>", "", txt).strip()) for hid, txt in heads]
        if entries:
            pages[f.relative_to(root).as_posix()] = entries
    return pages


def item_html(href, text):
    m = re.match(r"^(\d+(?:\.\d+)*\.?)\s*(.*)$", text)
    if m:
        label = f'<strong aria-hidden="true">{m.group(1)}</strong> {m.group(2)}'
    else:
        label = text
    return f'<li class="chapter-item "><a href="{href}">{label}</a></li>'


def inject(root):
    pages = collect_headings(root)
    changed = 0
    for f in sorted(root.rglob("*.html")):
        if f.name in SKIP:
            continue
        html = f.read_text(encoding="utf-8")
        if MARKER in html:
            continue
        nav = re.search(r'<nav id="sidebar".*?</nav>', html, re.S)
        if not nav:
            continue
        sidebar = new_sidebar = nav.group(0)
        depth = len(f.relative_to(root).parts) - 1
        prefix = "../" * depth
        for rel, entries in pages.items():
            href = prefix + rel
            pat = re.compile(
                r'(<li class="chapter-item [^"]*"><a href="%s"[^>]*>.*?)(</li>)'
                % re.escape(href))
            m = pat.search(new_sidebar)
            if not m or 'class="toggle"' in m.group(1):
                continue
            items = "".join(item_html(f"{href}#{hid}", txt) for hid, txt in entries)
            repl = (f'{m.group(1)}<a class="toggle"><div>❱</div></a></li>'
                    f'<li {MARKER}><ol class="section">{items}</ol></li>')
            new_sidebar = new_sidebar[:m.start()] + repl + new_sidebar[m.end():]
        if new_sidebar != sidebar:
            f.write_text(html.replace(sidebar, new_sidebar), encoding="utf-8")
            changed += 1
    print(f"[subtoc] {root}: {len(pages)} pages with h2s, sidebar updated in {changed} files")


if __name__ == "__main__":
    inject(Path(sys.argv[1]))
