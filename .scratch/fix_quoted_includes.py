#!/usr/bin/env python3
"""Fix quoted #include "X.hpp" lines left over from the module->header conversion that don't
resolve (the file lives under Include/<Package>/... but the quoted form assumed it sat next to
the includer). Rewrites them to the established <Package/relative/path.hpp> angle-bracket form,
preferring a match within the includer's own package when the plain quoted name is ambiguous.
"""
import re
import sys
from pathlib import Path

ROOT = Path("/mnt/E/Projects/SturdyEngine5")
EXCLUDE_DIRS = {".cache", "build", ".git", ".scratch"}
INCLUDE_RE = re.compile(r'^(\s*)#include\s+"([^"]+\.hpp)"\s*$')

PACKAGES = ["Foundation", "Async", "RHI", "Platform", "Text", "Core", "Renderer", "Engine", "graphicsPlatform"]


def build_index():
    """canonical suffix path (e.g. 'Window/Window.hpp') -> list of (package, full_canonical)"""
    index = {}
    for pkg in PACKAGES:
        inc_root = ROOT / pkg / "Include" / pkg
        if not inc_root.is_dir():
            continue
        for f in inc_root.rglob("*.hpp"):
            rel = f.relative_to(inc_root)
            canonical = f"{pkg}/{rel.as_posix()}"
            # index by every suffix (so 'Window.hpp', 'Window/Window.hpp' etc all resolve)
            parts = rel.as_posix().split("/")
            for i in range(len(parts)):
                suffix = "/".join(parts[i:])
                index.setdefault(suffix, []).append((pkg, canonical))
    return index


def package_of(path: Path):
    try:
        rel = path.relative_to(ROOT)
    except ValueError:
        return None
    return rel.parts[0]


def resolves_relatively(path: Path, quoted: str) -> bool:
    return (path.parent / quoted).is_file()


def main():
    index = build_index()
    changed = 0
    for f in ROOT.rglob("*"):
        if f.suffix not in (".hpp", ".cpp"):
            continue
        if any(part in EXCLUDE_DIRS for part in f.parts):
            continue
        text = f.read_text()
        lines = text.splitlines(keepends=True)
        new_lines = []
        file_changed = False
        own_pkg = package_of(f)
        for line in lines:
            m = INCLUDE_RE.match(line)
            if not m:
                new_lines.append(line)
                continue
            indent, quoted = m.group(1), m.group(2)
            if resolves_relatively(f, quoted):
                new_lines.append(line)
                continue
            candidates = index.get(quoted, [])
            if not candidates:
                print(f"UNRESOLVED: {f.relative_to(ROOT)}: \"{quoted}\"")
                new_lines.append(line)
                continue
            same_pkg = [c for c in candidates if c[0] == own_pkg]
            chosen = same_pkg[0] if same_pkg else candidates[0]
            if len(candidates) > 1 and not same_pkg:
                print(f"AMBIGUOUS (picked {chosen[1]}): {f.relative_to(ROOT)}: \"{quoted}\" candidates={candidates}")
            new_lines.append(f'{indent}#include <{chosen[1]}>\n')
            file_changed = True
        if file_changed:
            f.write_text("".join(new_lines))
            changed += 1
            print(f"fixed: {f.relative_to(ROOT)}")
    print(f"\n{changed} file(s) changed")


if __name__ == "__main__":
    main()
