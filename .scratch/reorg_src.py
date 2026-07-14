#!/usr/bin/env python3
"""Move each package's Include/<Package>/**/*.hpp and scattered *.cpp files into a unified
src/<Package>/... tree, mirroring their existing relative subdirectory so a header and its
paired implementation file end up sitting next to each other. Uses `git mv` throughout.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/mnt/E/Projects/SturdyEngine5")
EXCLUDE_DIRS = {".cache", "build", ".git", ".scratch", "Include", "src"}
PACKAGES = ["RHI", "Platform", "Text", "Core", "Renderer", "Engine"]


def is_tracked(path: Path) -> bool:
    result = subprocess.run(["git", "ls-files", "--error-unmatch", str(path)],
                             cwd=ROOT, capture_output=True)
    return result.returncode == 0


def git_mv(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if is_tracked(src):
        subprocess.run(["git", "mv", str(src), str(dst)], check=True, cwd=ROOT)
    else:
        shutil.move(str(src), str(dst))


def reorg_package(pkg: str):
    pkg_dir = ROOT / pkg
    src_root = pkg_dir / "src" / pkg
    moved = 0

    # 1. Headers: Include/<Package>/** -> src/<Package>/**
    include_root = pkg_dir / "Include" / pkg
    if include_root.is_dir():
        for f in sorted(include_root.rglob("*.hpp")):
            rel = f.relative_to(include_root)
            dst = src_root / rel
            git_mv(f, dst)
            moved += 1

    # 2. Impl .cpp files scattered directly under the package dir (not under Include/ or src/)
    for f in sorted(pkg_dir.rglob("*.cpp")):
        if any(part in EXCLUDE_DIRS for part in f.relative_to(pkg_dir).parts[:-1]):
            continue
        rel = f.relative_to(pkg_dir)
        dst = src_root / rel
        git_mv(f, dst)
        moved += 1

    # 3. Remove now-empty directories left behind (Include/ tree and any emptied source dirs).
    for d in sorted(pkg_dir.rglob("*"), key=lambda p: -len(p.parts)):
        if d.is_dir() and d != src_root and src_root not in d.parents and not any(d.iterdir()):
            d.rmdir()

    print(f"{pkg}: moved {moved} file(s)")


def main():
    for pkg in PACKAGES:
        reorg_package(pkg)


if __name__ == "__main__":
    main()
