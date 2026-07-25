#!/usr/bin/env python3
"""
将 BiShengCLanguageUserManual.md 拆分为 src/ 中的多个子文件。

用法:
    python split_manual.py <source_file> <output_dir>

SUMMARY.md 和各章 README 的目录来自文档中实际出现的章节（按文档顺序），
所以 release / preview 两个版本可以有不同的章节集合。SECTION_MAP 只负责
给章节一个稳定的 URL slug；文档里出现了 map 之外的新章节时会打印警告并
使用 "<章>-<节>" 形式的回退文件名 —— 看到警告就该给 SECTION_MAP 补一行。
"""

from __future__ import annotations

import re
import os
import argparse
from collections import OrderedDict

CHAPTER_DIR_MAP = {
    "1": "chapter-1-getting-started",
    "2": "chapter-2-development-efficiency",
    "3": "chapter-3-memory-safety",
    "4": "chapter-4-concurrency",
    "5": "chapter-5-toolchain",
    "6": "chapter-6-standard-library",
    "附录": "appendix",
}

# Maps section identifiers (from ### X.Y. Title) to output files.
# Naming only — presence/order in the nav comes from the document itself.
SECTION_MAP = OrderedDict({
    "1.1": (CHAPTER_DIR_MAP["1"], "1-build"),
    "1.2": (CHAPTER_DIR_MAP["1"], "2-hello-bsc"),
    "2.1": (CHAPTER_DIR_MAP["2"], "1-member-functions"),
    "2.2": (CHAPTER_DIR_MAP["2"], "2-generic"),
    "2.3": (CHAPTER_DIR_MAP["2"], "3-constexpr"),
    "2.4": (CHAPTER_DIR_MAP["2"], "4-trait"),
    "2.5": (CHAPTER_DIR_MAP["2"], "5-operator-overloading"),
    "3.1": (CHAPTER_DIR_MAP["3"], "1-ownership"),
    "3.2": (CHAPTER_DIR_MAP["3"], "2-borrowing"),
    "3.3": (CHAPTER_DIR_MAP["3"], "3-arrays-safe-pointers"),
    "3.4": (CHAPTER_DIR_MAP["3"], "4-nonnull-pointer"),
    "3.5": (CHAPTER_DIR_MAP["3"], "5-owned-struct"),
    "3.6": (CHAPTER_DIR_MAP["3"], "6-safe-zone"),
    "3.7": (CHAPTER_DIR_MAP["3"], "7-initial-analysis"),
    "3.8": (CHAPTER_DIR_MAP["3"], "8-type-compat"),
    "4.1": (CHAPTER_DIR_MAP["4"], "1-stackless-coroutine"),
    "5.1": (CHAPTER_DIR_MAP["5"], "1-bsc2c"),
    "5.2": (CHAPTER_DIR_MAP["5"], "2-debugging"),
    "5.3": (CHAPTER_DIR_MAP["5"], "3-ide"),
    "6.1": (CHAPTER_DIR_MAP["6"], "1-safe-api"),
    "6.2": (CHAPTER_DIR_MAP["6"], "2-safe-container"),
    "6.3": (CHAPTER_DIR_MAP["6"], "3-smart-pointer"),
    "6.4": (CHAPTER_DIR_MAP["6"], "4-coroutine-scheduler"),
    "6.5": (CHAPTER_DIR_MAP["6"], "5-network-library"),
})

APPENDIX_MAP = OrderedDict({
    "附录A": (CHAPTER_DIR_MAP["附录"], "01-keywords"),
    "附录B": (CHAPTER_DIR_MAP["附录"], "02-grammar"),
})

CHAPTER_HEADING_RE = re.compile(r'##\s+(\d+)\.\s+(.+)')
SECTION_HEADING_RE = re.compile(r'###\s+(\d+\.\d+|附录[ABCDEF])\.\s+(.+)')
SUBHEADING_RE = re.compile(r'^(#{4,6})\s+(?:([\dA-Z]+(?:\.[\dA-Z]+)*)\.\s+)?(.+)')


def extract_section_id(heading: str) -> str | None:
    m = SECTION_HEADING_RE.match(heading)
    return m.group(1) if m else None


def extract_section_title(heading: str) -> str | None:
    """Extract the display title from a ### heading.
    For regular sections returns the title after the number.
    For appendix sections returns '{letter} - {title}' like 'A - 关键字'.
    """
    m = SECTION_HEADING_RE.match(heading)
    if not m:
        return None
    sec_id = m.group(1)
    title = m.group(2)
    if sec_id.startswith('附录'):
        letter = sec_id[2:]
        return f'{letter} - {title}'
    return title


def extract_chapter_id(heading: str) -> str | None:
    if heading == '## 附录':
        return '附录'
    m = CHAPTER_HEADING_RE.match(heading)
    return m.group(1) if m else None


def convert_section_heading(heading: str) -> str:
    return SECTION_HEADING_RE.sub(r'# \2', heading)


def convert_chapter_heading(heading: str) -> str:
    if heading == '## 附录':
        return '# 附录'
    return CHAPTER_HEADING_RE.sub(r'# \2', heading)


def extract_chapter_title(heading: str) -> str:
    if heading == '## 附录':
        return '附录'
    m = CHAPTER_HEADING_RE.match(heading)
    return m.group(2) if m else heading.removeprefix('## ')


def process_subheadings(lines: list[str]) -> list[str]:
    result = []
    for line in lines:
        m = SUBHEADING_RE.match(line)
        if m:
            hashes = m.group(1)
            number = m.group(2)
            title = m.group(3)
            new_level = max(1, len(hashes) - 2)
            prefix = f'{number}. ' if number else ''
            result.append(f'{"#" * new_level} {prefix}{title}\n')
        else:
            result.append(line)
    return result


def chapter_dir(chapter_id: str) -> str:
    return CHAPTER_DIR_MAP.get(chapter_id, f'chapter-{chapter_id}')


def section_chapter(sec_id: str) -> str:
    return '附录' if sec_id.startswith('附录') else sec_id.split('.')[0]


def resolve_target(sec_id: str) -> tuple[str, str]:
    """(dir, file-slug) for a section id, with a loud fallback for unmapped ids."""
    if sec_id in SECTION_MAP:
        return SECTION_MAP[sec_id]
    if sec_id in APPENDIX_MAP:
        return APPENDIX_MAP[sec_id]
    d = chapter_dir(section_chapter(sec_id))
    f = ('x-' + sec_id[2:].lower()) if sec_id.startswith('附录') \
        else sec_id.replace('.', '-')
    print(f"Warning: section '{sec_id}' missing from SECTION_MAP — "
          f"using fallback '{d}/{f}.md' (please add it to split_manual.py)")
    return (d, f)


def main(source_file: str, output_dir: str):
    with open(source_file, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    os.makedirs(output_dir, exist_ok=True)

    # First pass: collect display titles for all sections and chapters
    section_titles = {}
    chapter_titles = {}
    for line in lines:
        stripped = line.strip()
        if stripped.startswith('## ') and stripped != '## 简介':
            ch_id = extract_chapter_id(stripped)
            if ch_id:
                chapter_titles[ch_id] = extract_chapter_title(stripped)
        elif stripped.startswith('### '):
            sec_id = extract_section_id(stripped)
            title = extract_section_title(stripped)
            if sec_id and title:
                section_titles[sec_id] = title

    # Second pass: split into sections
    sections = []
    current_start = None
    current_id = None

    for i, line in enumerate(lines):
        stripped = line.strip()

        if stripped == '## 简介':
            if current_start is not None and current_id is not None:
                sections.append((current_start, i, current_id))
            current_start = i
            current_id = 'intro'

        elif stripped.startswith('## ') and stripped != '## 简介':
            if current_start is not None and current_id is not None:
                sections.append((current_start, i, current_id))
            chapter_id = extract_chapter_id(stripped)
            current_start = i
            current_id = f'ch:{chapter_id}' if chapter_id else None

        elif stripped.startswith('### '):
            if current_start is not None and current_id is not None:
                sections.append((current_start, i, current_id))
            current_start = i
            current_id = extract_section_id(stripped)

    if current_start is not None and current_id is not None:
        sections.append((current_start, len(lines), current_id))

    # Index what the DOCUMENT actually contains, in document order. The nav is
    # built from this — not from the static maps — so editions with different
    # section sets (e.g. a release-only section) each get a correct TOC.
    doc_chapters: list[str] = []
    doc_sections: dict[str, list[str]] = {}
    targets: dict[str, tuple[str, str]] = {}
    for _start, _end, sid in sections:
        if sid == 'intro':
            continue
        if sid.startswith('ch:'):
            ch = sid[3:]
        else:
            ch = section_chapter(sid)
            doc_sections.setdefault(ch, []).append(sid)
            targets[sid] = resolve_target(sid)
        if ch not in doc_chapters:
            doc_chapters.append(ch)
            doc_sections.setdefault(ch, [])

    # Third pass: Generate SUMMARY.md
    summary_lines = ['# Summary\n', '\n', '[简介](README.md)\n\n']
    for ch in doc_chapters:
        dir_name = chapter_dir(ch)
        ch_title = chapter_titles.get(ch, dir_name)
        summary_lines.append(f'- [{ch_title}](./{dir_name}/README.md)\n')
        for sec_id in doc_sections[ch]:
            sec_title = section_titles.get(sec_id, sec_id)
            _d, file_name = targets[sec_id]
            summary_lines.append(f'  - [{sec_title}](./{dir_name}/{file_name}.md)\n')

    summary_path = os.path.join(output_dir, 'SUMMARY.md')
    with open(summary_path, 'w', encoding='utf-8') as f:
        f.writelines(summary_lines)
    print(f"Written: {summary_path}")

    # Fourth pass: write files
    for start, end, section_id in sections:
        content_lines = lines[start:end]
        heading = content_lines[0].strip()
        body_lines = process_subheadings(content_lines[1:])

        if section_id == 'intro':
            combined = ['# 简介\n'] + body_lines
            output_path = os.path.join(output_dir, 'README.md')

        elif section_id.startswith('ch:'):
            chapter_id = section_id[3:]
            dir_name = chapter_dir(chapter_id)

            # Build link list from the chapter's sub-sections present in THIS document
            link_lines = []
            for sec_id in doc_sections.get(chapter_id, []):
                title = section_titles.get(sec_id, sec_id)
                _d, file_name = targets[sec_id]
                link_lines.append(f'- [{title}](./{file_name}.md)\n')

            # Insert link list after heading, before body
            combined = [convert_chapter_heading(heading) + '\n']
            if link_lines:
                combined.append('\n')
                combined.extend(link_lines)
                combined.append('\n')
            combined.extend(body_lines)
            output_path = os.path.join(output_dir, dir_name, 'README.md')

        else:
            dir_name, file_name = targets[section_id]
            combined = [convert_section_heading(heading) + '\n'] + body_lines
            output_path = os.path.join(output_dir, dir_name, file_name + '.md')

        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, 'w', encoding='utf-8') as f:
            f.writelines(combined)
        print(f"Written: {output_path}")

    print(f"Done! Total files written: {len(sections) + 1}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Split a manual into chapters.")
    parser.add_argument("source_file", type=str, help="Path to the source Markdown file.")
    parser.add_argument("output_dir", type=str, help="Path to the output directory.")
    args = parser.parse_args()
    main(args.source_file, args.output_dir)
