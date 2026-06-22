#!/usr/bin/env python3
"""Annotate preview manual pages with an inline diff vs the release edition.

Usage: annotate_preview_diff.py <release_src_dir> <preview_src_dir> <lang:zh|en>

For every page that exists in BOTH src trees, compute a line-level diff of the
preview page against the release page. If they differ, prepend a small callout and
render the changed lines as a ```diff fenced block at the TOP of the page (mdBook's
highlighter colours `-` red and `+` green), leaving the real page content untouched
below. Pages that are identical are left alone.

Why a diff block at the top rather than inline surgery: injecting -/+ markers inside
the live body would corrupt code fences, tables, and headings. A self-contained
```diff summary is always valid Markdown and never breaks the page that follows.
"""
import os, sys, difflib

rel_dir, prev_dir, lang = sys.argv[1], sys.argv[2], sys.argv[3]

CALLOUT = {
    "zh": ("> 🔍 **本页含预览版改动**：以下为相对正式版的差异（<span style=\"color:#22863a\">绿色 +</span> 为新增，"
           "<span style=\"color:#b31d28\">红色 -</span> 为删除）。完整正文见下方。"),
    "en": ("> 🔍 **This page has preview-only changes.** The diff vs the release edition is shown below "
           "(<span style=\"color:#22863a\">green +</span> added, <span style=\"color:#b31d28\">red -</span> removed). "
           "Full content follows."),
}

import re
def _backtick_runs(text):
    """Lengths of every run of backticks in text (to pick a safe outer fence)."""
    return [len(m.group(0)) for m in re.finditer(r"`+", text)]

def rel_lines(p):
    return open(p, encoding="utf-8").read().splitlines()

def make_diff_block(rel, prev):
    """Return a compact unified-ish diff body (changed hunks only), or '' if identical."""
    sm = difflib.SequenceMatcher(a=rel, b=prev, autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        # a few lines of leading context help orient the reader
        ctx_start = max(0, i1 - 2)
        for k in range(ctx_start, i1):
            out.append("  " + rel[k])
        if tag in ("replace", "delete"):
            for k in range(i1, i2):
                out.append("- " + rel[k])
        if tag in ("replace", "insert"):
            for k in range(j1, j2):
                out.append("+ " + prev[k])
        out.append("")  # blank between hunks
    return "\n".join(out).rstrip()

def annotate(page_rel_path):
    relp = os.path.join(rel_dir, page_rel_path)
    prevp = os.path.join(prev_dir, page_rel_path)
    if not (os.path.isfile(relp) and os.path.isfile(prevp)):
        return False
    rel, prev = rel_lines(relp), rel_lines(prevp)
    if rel == prev:
        return False
    body = make_diff_block(rel, prev)
    if not body:
        return False
    # keep the page's own H1 first, then the callout + diff, then the rest
    content = open(prevp, encoding="utf-8").read()
    lines = content.splitlines()
    head, rest = "", content
    if lines and lines[0].startswith("# "):
        head = lines[0]
        rest = "\n".join(lines[1:]).lstrip("\n")
    # The diff body can itself contain ``` fences (copied from the manual's code
    # blocks). Wrap in a LONGER fence so those inner backticks don't close it early.
    longest = max(_backtick_runs(body), default=0)
    fence = "`" * max(4, longest + 1)
    block = f"{fence}diff\n{body}\n{fence}"
    new = (f"{head}\n\n{CALLOUT[lang]}\n\n"
           f"<details>\n<summary>{'查看改动' if lang=='zh' else 'View changes'}</summary>\n\n"
           f"{block}\n\n</details>\n\n{rest}\n")
    open(prevp, "w", encoding="utf-8").write(new)
    return True

def page_title(path):
    """First '# ' heading of a page, for the changed-sections list."""
    try:
        for ln in open(path, encoding="utf-8"):
            if ln.startswith("# "):
                return ln[2:].strip()
    except OSError:
        pass
    return path

# Walk preview pages, annotate the ones that differ, and collect the changed list.
changed = []   # (rel_path, title)
for root, _, files in os.walk(prev_dir):
    for f in files:
        if not f.endswith(".md") or f == "SUMMARY.md":
            continue
        rel_path = os.path.relpath(os.path.join(root, f), prev_dir)
        if annotate(rel_path):
            title = page_title(os.path.join(prev_dir, rel_path))
            changed.append((rel_path, title))
            print(f"  [diff] {rel_path}")

# Header summary on the preview intro page: list which sections differ from release.
changed.sort()
readme = os.path.join(prev_dir, "README.md")
if changed and os.path.isfile(readme):
    if lang == "zh":
        hdr = ["", "## 📋 本预览版与正式版的差异",
               "", "以下章节包含预览版改动（点击查看，页内有红绿差异标注）：", ""]
    else:
        hdr = ["", "## 📋 What differs from the release edition",
               "", "These sections have preview-only changes (each page shows a red/green diff):", ""]
    for rel_path, title in changed:
        link = rel_path[:-3] + ".html"   # mdBook renders .md -> .html
        hdr.append(f"- [{title}](./{link})")
    hdr.append("")
    content = open(readme, encoding="utf-8").read()
    # insert the summary right after the page's H1 (keep any existing banners below it intact)
    lines = content.splitlines()
    if lines and lines[0].startswith("# "):
        new = lines[0] + "\n" + "\n".join(hdr) + "\n" + "\n".join(lines[1:]) + "\n"
    else:
        new = "\n".join(hdr) + "\n" + content
    open(readme, "w", encoding="utf-8").write(new)
    print(f"[annotate] wrote changed-sections summary to preview README ({len(changed)} sections)")

print(f"[annotate] pages with preview diff: {len(changed)}")
