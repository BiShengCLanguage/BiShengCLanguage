#!/usr/bin/env python3
"""Annotate preview manual pages with an INLINE diff vs the release edition.

Usage: annotate_preview_diff.py <release_src_dir> <preview_src_dir> <lang:zh|en>

For every page that exists in BOTH src trees, diff the preview page against the
release page at BLOCK granularity and rewrite the preview page so that each change
is shown ```diff'd right where it occurs — unchanged blocks render as normal
Markdown, and only the changed/added regions become a small red/green diff block in
place. Identical pages are left alone.

Why block-level (not raw line surgery): a block is an atomic unit — a whole fenced
code block, table, heading, or blank-line-delimited paragraph. Diffing the SEQUENCE
of blocks means an inline diff never splits a code fence or table mid-way; each diff
hunk sits between intact blocks. Unchanged code/tables keep rendering natively.
"""
import os, sys, difflib, re

rel_dir, prev_dir, lang = sys.argv[1], sys.argv[2], sys.argv[3]

CALLOUT = {
    "zh": ("> 🔍 **本页含预览版改动**：下文用 <span style=\"color:#22863a\">绿色 +</span> 标注新增、"
           "<span style=\"color:#b31d28\">红色 -</span> 标注删除，差异就地显示在对应位置。"),
    "en": ("> 🔍 **This page has preview-only changes.** Differences are shown inline below where they "
           "occur — <span style=\"color:#22863a\">green +</span> added, <span style=\"color:#b31d28\">red -</span> removed."),
}

def _backtick_runs(text):
    return [len(m.group(0)) for m in re.finditer(r"`+", text)]

def diff_fence(body):
    """A ```diff fence long enough that inner ``` runs in `body` can't close it."""
    longest = max(_backtick_runs(body), default=0)
    f = "`" * max(4, longest + 1)
    return f"{f}diff\n{body}\n{f}"

def split_blocks(lines):
    """Split a page into atomic blocks. A fenced code block (``` / ~~~, any length)
    is ONE block; otherwise blocks are separated by blank lines. Returns list of
    blocks, each a list of lines (no trailing blank)."""
    blocks, cur = [], []
    i, n = 0, len(lines)
    fence_re = re.compile(r"^(\s*)(`{3,}|~{3,})")
    def flush():
        nonlocal cur
        if cur:
            # drop trailing blanks inside the accumulated chunk into separate splits
            blocks.append(cur)
            cur = []
    while i < n:
        m = fence_re.match(lines[i])
        if m:
            flush()
            fence = m.group(2)[0]
            # capture the whole code block until the matching closing fence
            code = [lines[i]]; i += 1
            close = re.compile(r"^\s*" + re.escape(m.group(2)[0]) + "{%d,}\\s*$" % len(m.group(2)))
            while i < n:
                code.append(lines[i])
                if close.match(lines[i]):
                    i += 1; break
                i += 1
            blocks.append(code)
            continue
        if lines[i].strip() == "":
            flush(); i += 1; continue
        cur.append(lines[i]); i += 1
    flush()
    return blocks

def block_key(block):
    return "\n".join(block)

def hunk_diff(rel_block_lines, prev_block_lines):
    """Render a -/+ diff body between two block groups (lists of lines)."""
    out = []
    for ln in rel_block_lines:
        out.append("- " + ln)
    for ln in prev_block_lines:
        out.append("+ " + ln)
    return "\n".join(out)

def annotate(page_rel_path):
    relp = os.path.join(rel_dir, page_rel_path)
    prevp = os.path.join(prev_dir, page_rel_path)
    if not (os.path.isfile(relp) and os.path.isfile(prevp)):
        return False
    rel_raw = open(relp, encoding="utf-8").read().splitlines()
    prev_raw = open(prevp, encoding="utf-8").read().splitlines()
    if rel_raw == prev_raw:
        return False

    # Keep the preview page's own H1 as the first line; diff the rest block-wise.
    head = ""
    if prev_raw and prev_raw[0].startswith("# "):
        head = prev_raw[0]
        prev_raw = prev_raw[1:]
    if rel_raw and rel_raw[0].startswith("# "):
        rel_raw = rel_raw[1:]

    rel_blocks = split_blocks(rel_raw)
    prev_blocks = split_blocks(prev_raw)
    sm = difflib.SequenceMatcher(
        a=[block_key(b) for b in rel_blocks],
        b=[block_key(b) for b in prev_blocks], autojunk=False)

    out_parts = []
    if head:
        out_parts.append(head)
    out_parts.append(CALLOUT[lang])

    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for b in prev_blocks[j1:j2]:
                out_parts.append("\n".join(b))
        elif tag == "insert":
            body = "\n".join("+ " + ln for b in prev_blocks[j1:j2] for ln in b)
            out_parts.append(diff_fence(body))
        elif tag == "delete":
            body = "\n".join("- " + ln for b in rel_blocks[i1:i2] for ln in b)
            out_parts.append(diff_fence(body))
        else:  # replace — show removed then added, in place
            rel_lines_ = [ln for b in rel_blocks[i1:i2] for ln in b]
            prev_lines_ = [ln for b in prev_blocks[j1:j2] for ln in b]
            out_parts.append(diff_fence(hunk_diff(rel_lines_, prev_lines_)))

    open(prevp, "w", encoding="utf-8").write("\n\n".join(out_parts) + "\n")
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
