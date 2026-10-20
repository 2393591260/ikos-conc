#!/usr/bin/env python3
"""Check local links and section anchors in the public concurrency guides."""

from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]


def anchors(path):
    text = path.read_text()
    explicit = re.findall(r'<a id="([^"]+)"', text)
    if len(explicit) != len(set(explicit)):
        raise ValueError(f"{path}: duplicate explicit anchor")
    result = set(explicit)
    for heading in re.findall(r"^#{1,6} (.+)$", text, re.MULTILINE):
        result.add(re.sub(r"[^a-z0-9 -]", "", heading.lower()).replace(" ", "-"))
    return result


def main():
    pages = [ROOT / "doc/CONCURRENCY.md"]
    pages.extend(sorted((ROOT / "doc/concurrency").rglob("*.md")))
    failures = []
    for page in pages:
        text = page.read_text()
        if text.count("```") % 2:
            failures.append(f"{page}: unclosed fenced block")
        anchors(page)
        for target in re.findall(r"\]\(([^)]+)\)", text):
            link = urlsplit(target)
            if link.scheme or link.netloc:
                continue
            destination = page.parent / unquote(link.path) if link.path else page
            if not destination.exists():
                failures.append(f"{page}: missing target {target}")
            elif link.fragment and destination.suffix == ".md":
                if unquote(link.fragment) not in anchors(destination):
                    failures.append(f"{page}: missing section {target}")
        if "/home/ruan/" in text:
            failures.append(f"{page}: public guide contains a private absolute path")
    if failures:
        raise SystemExit("\n".join(failures))
    print(f"Checked {len(pages)} public concurrency documentation pages.")


if __name__ == "__main__":
    main()
