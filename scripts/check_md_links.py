#!/usr/bin/env python3
"""Check that relative links in Markdown files point to existing targets.

Scans all .md files under the repository root for inline links [text](path)
and reference links [key]: path.  For each relative link (not http(s)):
  - Checks that the file or directory exists
  - If the link has a #fragment, checks that a matching heading exists

Exit code: 0 if all links are valid, 1 otherwise.
"""

import os
import re
import sys


# Match inline links: [text](url) and images: ![alt](url)
INLINE_RE = re.compile(r'!?\[[^\]]*\]\(([^)]+)\)')

# Match reference-style link definitions: [key]: url
REF_RE = re.compile(r'^\[([^\]]+)\]:\s+(.+)$', re.MULTILINE)

# Match ATX headings: # Heading
HEADING_RE = re.compile(r'^(#{1,6})\s+(.+?)(?:\s+#*)?$', re.MULTILINE)


def strip_fenced_blocks(content):
    """Remove fenced code blocks (``` ... ```) from content.

    Returns a string with the same number of lines (fenced regions
    replaced with blank lines) so that line-number accounting is
    preserved for later regex matches.
    """
    lines = content.split('\n')
    out = []
    in_fence = False
    for line in lines:
        if not in_fence and re.match(r'^(`{3,}|~{3,})', line):
            in_fence = True
            out.append('')
        elif in_fence and re.match(r'^(`{3,}|~{3,})\s*$', line):
            in_fence = False
            out.append('')
        elif in_fence:
            out.append('')
        else:
            out.append(line)
    return '\n'.join(out)


def heading_to_anchor(text):
    """Convert a Markdown heading to a GitHub-style anchor slug."""
    # Strip inline formatting (keep underscores — they appear in anchors)
    text = re.sub(r'[*`~]', '', text)
    # Strip link syntax: [text](url) -> text
    text = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text)
    # Lowercase
    text = text.lower()
    # Replace spaces and hyphens with single hyphens
    text = re.sub(r'[\s]+', '-', text)
    # Remove non-alphanumeric except hyphens
    text = re.sub(r'[^\w-]', '', text, flags=re.ASCII)
    # Collapse multiple hyphens
    text = re.sub(r'-+', '-', text)
    return text.strip('-')


def get_headings(filepath):
    """Return the set of anchor slugs for all headings in a Markdown file."""
    try:
        with open(filepath) as f:
            content = strip_fenced_blocks(f.read())
    except (OSError, UnicodeDecodeError):
        return set()

    anchors = set()
    counts = {}
    for m in HEADING_RE.finditer(content):
        slug = heading_to_anchor(m.group(2))
        # GitHub de-duplicates: first is #foo, second is #foo-1, etc.
        if slug in counts:
            counts[slug] += 1
            anchors.add(f'{slug}-{counts[slug]}')
        else:
            counts[slug] = 0
            anchors.add(slug)
    return anchors


def strip_inline_code(content):
    """Remove inline code spans (`...`) preserving string length with spaces."""
    return re.sub(r'`[^`\n]+`', lambda m: ' ' * len(m.group()), content)


def extract_links(filepath):
    """Yield (lineno, raw_url) for all links in a Markdown file."""
    with open(filepath) as f:
        content = strip_fenced_blocks(f.read())
    content = strip_inline_code(content)

    # Track line numbers by finding match positions
    for m in INLINE_RE.finditer(content):
        url = m.group(1).strip()
        lineno = content[:m.start()].count('\n') + 1
        yield lineno, url

    for m in REF_RE.finditer(content):
        url = m.group(2).strip()
        lineno = content[:m.start()].count('\n') + 1
        yield lineno, url


def check_link(md_path, lineno, url, heading_cache):
    """Check a single link. Returns an error string or None."""
    # Skip external URLs
    if url.startswith(('http://', 'https://', 'mailto:')):
        return None

    # Skip bare fragments on same file
    if url.startswith('#'):
        fragment = url[1:]
        if md_path not in heading_cache:
            heading_cache[md_path] = get_headings(md_path)
        if fragment not in heading_cache[md_path]:
            return f'anchor not found: {url}'
        return None

    # Split path and optional fragment
    if '#' in url:
        path_part, fragment = url.split('#', 1)
    else:
        path_part, fragment = url, None

    # Resolve relative to the file's directory
    base_dir = os.path.dirname(md_path)
    target = os.path.normpath(os.path.join(base_dir, path_part))

    if not os.path.exists(target):
        return f'target not found: {url}'

    # Check fragment if present
    if fragment and os.path.isfile(target) and target.endswith('.md'):
        if target not in heading_cache:
            heading_cache[target] = get_headings(target)
        if fragment not in heading_cache[target]:
            return f'anchor not found: {url} (heading #{fragment} missing in {os.path.relpath(target)})'

    return None


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    md_files = []
    for dirpath, dirnames, filenames in os.walk('.'):
        # Skip hidden dirs, build, third_party, node_modules
        dirnames[:] = [d for d in dirnames
                       if not d.startswith('.') and d not in
                       ('build', 'third_party', 'node_modules', 'states')]
        for f in filenames:
            if f.endswith('.md'):
                md_files.append(os.path.join(dirpath, f))

    md_files.sort()
    heading_cache = {}
    errors = []

    for md_path in md_files:
        for lineno, url in extract_links(md_path):
            err = check_link(md_path, lineno, url, heading_cache)
            if err:
                errors.append((md_path, lineno, err))

    if errors:
        for path, lineno, err in errors:
            print(f'{os.path.relpath(path)}:{lineno}: {err}')
        print(f'\n{len(errors)} broken link(s) found.')
        return 1

    print(f'All links OK across {len(md_files)} Markdown files.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
