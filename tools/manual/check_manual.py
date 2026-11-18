#!/usr/bin/env python3
"""Checks the user manual (docs/manual/): the manifest, the pages, the links — and whether every
menu action and viewport-strip control the app shows is mentioned somewhere in it.

    python tools/manual/check_manual.py            # from the repository root; exit code = errors
    python tools/manual/check_manual.py --strict   # coverage warnings count as errors too

Errors (exit code): a manifest that does not parse, a page listed but missing, a page present but
unlisted, a duplicate id, a link to a page or an anchor that does not exist, an image that does
not exist. Warnings: a menu action or strip control label that no page mentions (the coverage
check reads the labels out of src/shell/menumanager.cpp and src/viewport/viewportstrip.cpp, so it
needs no build). The anchor rule mirrors HelpManual::slugFor in src/help/helpmanual.cpp.
"""
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MANUAL = os.path.join(ROOT, "docs", "manual")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")

HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
LINK = re.compile(r"(!?)\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"^\s*(```|~~~)")

# A UI label as the sources write it: a string literal, bare or inside tr() / QObject::tr() /
# QStringLiteral(), after an optional icon argument (a name or one call). The literal may carry
# escapes ("Delete Selected Object\tDel").
_ICON_ARG = r'(?:[^,()"]*(?:\([^()]*\))?\s*,\s*)?'
_WRAP = r'(?:(?:QObject::)?tr|QStringLiteral)\('
_LITERAL = r'"((?:[^"\\]|\\.)+)"'
MENU_LABEL = re.compile(r'add(?:Action|Menu)\(' + _ICON_ARG + '(?:' + _WRAP + ')?' + _LITERAL)
TOOLTIP_LABEL = re.compile(r'setToolTip\(' + _WRAP + _LITERAL + r'\)\)')


def slug_for(text):
    """HelpManual::slugFor: lower case; letters, digits, '_' and '-' kept; spaces -> '-'; the rest dropped."""
    text = text.replace("`", "").replace("*", "").strip().lower()
    out = []
    for c in text:
        if c.isalnum() or c in "_-":
            out.append(c)
        elif c.isspace():
            out.append("-")
    return "".join(out)


def headings_of(text):
    """(level, text, slug) of every ATX heading outside fenced code, slugs numbered like the app's."""
    out, seen, in_fence = [], {}, False
    for line in text.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = HEADING.match(line)
        if not m:
            continue
        slug = slug_for(m.group(2))
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        if n:
            slug = "%s-%d" % (slug, n)
        out.append((len(m.group(1)), m.group(2), slug))
    return out


def links_of(text):
    out, in_fence = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for m in LINK.finditer(line):
            out.append((m.group(1) == "!", m.group(2), m.group(3)))
    return out


def collect_pages(entries, out, errors, path="pages"):
    for i, entry in enumerate(entries):
        where = "%s[%d]" % (path, i)
        for key in ("id", "title", "file"):
            if not isinstance(entry.get(key), str) or not entry[key]:
                errors.append("%s: missing '%s'" % (where, key))
        out.append(entry)
        collect_pages(entry.get("children", []), out, errors, where + ".children")


def main():
    # The messages quote page names, headings and UI labels as they are written. Under CTest the
    # output is a pipe, which Python on Windows encodes with the system code page, strictly: one
    # character outside it (an arrow in a label) and the check died of its own warning, with a
    # traceback where the message should be. UTF-8, whatever the output is.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    strict = "--strict" in sys.argv
    errors, warnings = [], []

    # --- the manifest ---
    manifest_path = os.path.join(MANUAL, "manual.json")
    try:
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as e:
        print("ERROR: manual.json: %s" % e)
        return 1
    pages = []
    collect_pages(manifest.get("pages", []), pages, errors)
    ids = [p.get("id") for p in pages]
    for dup in sorted({i for i in ids if ids.count(i) > 1}):
        errors.append("duplicate page id '%s'" % dup)
    files = {}
    for p in pages:
        name = p.get("file", "")
        path = CHANGELOG if name == "changelog.md" else os.path.join(MANUAL, name)
        if not os.path.isfile(path):
            errors.append("page '%s' lists %s, which does not exist" % (p.get("id"), name))
            continue
        with open(path, encoding="utf-8") as f:
            files[name] = f.read()
    # every .md in the folder (README.md aside) should be listed
    for name in sorted(os.listdir(MANUAL)):
        if name.endswith(".md") and name != "README.md" and name not in files:
            errors.append("%s is in docs/manual/ but not in manual.json" % name)

    # --- the pages: one title, the links, the images ---
    anchors = {name: {h[2] for h in headings_of(text)} for name, text in files.items()}
    for name, text in files.items():
        heads = headings_of(text)
        if name != "changelog.md" and sum(1 for h in heads if h[0] == 1) != 1:
            errors.append("%s: a page needs exactly one '#' title" % name)
        for is_image, _label, target in links_of(text):
            if re.match(r"^(https?|mailto):", target):
                continue
            if target.startswith("qrc:"):
                continue  # (the app's own resources; not checkable here)
            if is_image:
                if not os.path.isfile(os.path.join(MANUAL, target)):
                    errors.append("%s: image %s does not exist" % (name, target))
                continue
            file_part, _, anchor = target.partition("#")
            page = file_part or name
            if page not in files:
                errors.append("%s: link to %s, which is not a page" % (name, target))
                continue
            if anchor and anchor not in anchors[page]:
                errors.append("%s: link to %s: no such heading (anchors: %s)" % (
                    name, target, ", ".join(sorted(anchors[page])[:12]) + ("…" if len(anchors[page]) > 12 else "")))

    # --- coverage: every menu action and strip control label appears somewhere ---
    corpus = "\n".join(text.lower() for text in files.values())
    labels = set()
    for source in (os.path.join(ROOT, "src", "shell", "menumanager.cpp"),
                   os.path.join(ROOT, "src", "viewport", "viewportstrip.cpp")):
        try:
            with open(source, encoding="utf-8") as f:
                code = f.read()
        except OSError:
            continue
        for pattern in (MENU_LABEL, TOOLTIP_LABEL):
            for m in pattern.finditer(code):
                labels.add(m.group(1).replace("\\t", "\t").replace('\\"', '"'))
    for label in sorted(labels):
        plain = label.split("\t")[0].rstrip(".").replace("...", "").strip().lower()
        plain = plain.replace("&&", "&")
        if len(plain) < 3 or plain in corpus or plain.replace("…", "") in corpus:
            continue
        warnings.append("no page mentions '%s'" % label.split("\t")[0])

    for e in errors:
        print("ERROR: " + e)
    for w in warnings:
        print("WARNING: " + w)
    print("manual: %d page(s), %d error(s), %d warning(s)" % (len(files), len(errors), len(warnings)))
    return len(errors) + (len(warnings) if strict else 0)


if __name__ == "__main__":
    sys.exit(main())
