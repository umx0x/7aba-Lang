#!/usr/bin/env python3
"""Assemble and validate the static GitHub Pages site."""

from __future__ import annotations

import argparse
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlparse
import shutil
import sys


ROOT = Path(__file__).resolve().parent.parent
PAGES = ("index.html", "docs.html", "download.html", "learn.html")
RELEASES = (
    "7aba-windows.exe",
    "7aba-windows-x86.zip",
    "7aba-windows-x64.exe",
    "7aba-windows-x64.zip",
    "7aba-source-unix.zip",
    "SHA256SUMS.txt",
)


class Links(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: set[str] = set()
        self.targets: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        element_id = values.get("id")
        if element_id:
            self.ids.add(element_id)
        attribute = "href" if tag in ("a", "link") else "src" if tag in ("img", "script") else None
        if attribute and values.get(attribute):
            self.targets.append(values[attribute] or "")


def parse_links(path: Path) -> Links:
    parser = Links()
    parser.feed(path.read_text(encoding="utf-8"))
    parser.close()
    return parser


def validate_site(site: Path) -> None:
    parsed_pages = {name: parse_links(site / name) for name in PAGES}
    for name, page in parsed_pages.items():
        for target in page.targets:
            url = urlparse(target)
            if url.scheme or target.startswith("//"):
                continue
            destination = (site / unquote(url.path)).resolve() if url.path else (site / name).resolve()
            if not destination.is_relative_to(site.resolve()) or not destination.is_file():
                raise ValueError(f"{name}: missing local target {target}")
            if url.fragment and destination.suffix == ".html":
                linked_page = parsed_pages.get(destination.name)
                if linked_page is None:
                    linked_page = parse_links(destination)
                if unquote(url.fragment) not in linked_page.ids:
                    raise ValueError(f"{name}: missing fragment {target}")


def build_site(output: Path) -> None:
    if output.exists():
        raise FileExistsError(f"Output directory already exists: {output}")
    output.mkdir(parents=True)
    for name in PAGES:
        source = ROOT / "website" / name
        text = source.read_text(encoding="utf-8")
        text = text.replace("../releases/", "releases/")
        text = text.replace("../tests/", "tests/")
        text = text.replace("../LICENSE", "LICENSE")
        text = text.replace("../pro.7ba", "pro.7ba")
        (output / name).write_text(text, encoding="utf-8", newline="\n")

    release_dir = output / "releases"
    release_dir.mkdir()
    for name in RELEASES:
        shutil.copy2(ROOT / "releases" / name, release_dir / name)
    (output / "LICENSE").write_bytes((ROOT / "LICENSE").read_bytes())
    (output / "pro.7ba").write_bytes((ROOT / "pro.7ba").read_bytes())
    example_dir = output / "tests"
    example_dir.mkdir()
    shutil.copy2(ROOT / "tests" / "comprehensive_showcase.7ba", example_dir)
    (output / ".nojekyll").write_text("", encoding="ascii")
    validate_site(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="new directory for the assembled site")
    args = parser.parse_args()
    try:
        build_site(args.output.resolve())
    except (OSError, ValueError) as error:
        print(f"site build failed: {error}", file=sys.stderr)
        return 1
    print(f"site built and validated: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
