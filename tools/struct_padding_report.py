#!/usr/bin/env python3
"""Reports engine structs whose field order wastes memory to alignment padding.

Reads a compile_commands.json, parses every engine translation unit with libclang, and for each non-template, non-polymorphic struct or
class defined under the repository (not thirdparty/.cache/build) compares sizeof with the smallest size the same fields could reach
when sorted by descending alignment. Only structs where reordering saves bytes are listed, largest saving first.

    struct_padding_report.py --compile-commands build/dpi-glfw/compile_commands.json --output /tmp/padding.json

Skipped: unions, bitfield-bearing records, classes with bases or virtual members (their layout is not a free reordering), anything
with a dependent type. The tool only reports; whether a given struct may be reordered (ABI mirrors, aggregate initialisers, hand-declared
layouts) is a human decision.
"""
import argparse, json, os, shlex, sys
from multiprocessing import Pool
import clang.cindex as ci
from clang.cindex import CursorKind as K

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP = ("/thirdparty/", "/.cache/", "/build/", "/Rust/target/")


def ours(path):
    return path and path.startswith(ROOT) and not any(s in path for s in SKIP)


def args_for(entry):
    parts = shlex.split(entry["command"])[1:]
    out, skip = [], False
    for p in parts:
        if skip:
            skip = False
        elif p in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif p in ("-c", "-MD", "-MMD") or p == entry["file"]:
            continue
        else:
            out.append(p)
    return out


def scan(entry):
    idx = ci.Index.create()
    try:
        tu = idx.parse(entry["file"], args=args_for(entry), options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
    except Exception:
        return []
    found = []
    stack = [tu.cursor]
    while stack:
        c = stack.pop()
        for ch in c.get_children():
            f = ch.location.file
            if f is not None and not ours(f.name):
                continue
            if ch.kind in (K.STRUCT_DECL, K.CLASS_DECL) and ch.is_definition():
                rec = analyse(ch)
                if rec:
                    found.append(rec)
            if ch.kind in (K.NAMESPACE, K.STRUCT_DECL, K.CLASS_DECL, K.UNEXPOSED_DECL, K.LINKAGE_SPEC):
                stack.append(ch)
    return found


def analyse(c):
    t = c.type
    size = t.get_size()
    if size <= 0 or t.get_align() <= 0:
        return None
    fields = []
    for ch in c.get_children():
        if ch.kind in (K.CXX_BASE_SPECIFIER, K.CLASS_TEMPLATE, K.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION):
            return None
        if ch.kind == K.CXX_METHOD and ch.is_virtual_method():
            return None
        if ch.kind == K.FIELD_DECL:
            if ch.is_bitfield():
                return None
            ft = ch.type
            s, a = ft.get_size(), ft.get_align()
            if s <= 0 or a <= 0:
                return None
            fields.append((ch.spelling, s, a, ch.type.spelling))
        if ch.kind == K.UNION_DECL and not ch.spelling and False:
            return None
    if len(fields) < 2:
        return None
    total = sum(f[1] for f in fields)
    al = max(f[2] for f in fields)
    optimal = -(-total // al) * al
    if optimal >= size:
        return None
    loc = c.location
    return {"name": c.spelling or "<anon>", "qual": c.type.spelling, "file": os.path.relpath(loc.file.name, ROOT), "line": loc.line,
            "size": size, "optimal": optimal, "saves": size - optimal, "fields": fields}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compile-commands", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("-j", type=int, default=os.cpu_count())
    a = ap.parse_args()
    db = [e for e in json.load(open(a.compile_commands)) if ours(e["file"]) and e["file"].endswith((".cpp", ".cc"))]
    print(f"{len(db)} translation units", file=sys.stderr)
    seen = {}
    with Pool(a.j) as p:
        for i, recs in enumerate(p.imap_unordered(scan, db, chunksize=4)):
            for r in recs:
                seen[(r["file"], r["line"], r["name"])] = r
            if i % 100 == 0:
                print(i, len(seen), file=sys.stderr)
    out = sorted(seen.values(), key=lambda r: -r["saves"])
    json.dump(out, open(a.output, "w"), indent=1)
    print(f"{len(out)} reorderable structs, {sum(r['saves'] for r in out)} bytes total per-instance", file=sys.stderr)


if __name__ == "__main__":
    main()
