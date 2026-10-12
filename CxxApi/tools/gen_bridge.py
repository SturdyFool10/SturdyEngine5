#!/usr/bin/env python3
"""Builds a throwaway `cxx` bridge from CxxApi's headers, used only by check_bridge.py as a bindability lint (nothing is committed;
the Rust repo writes its own `#[cxx::bridge]`).

    gen_bridge.py --compile-commands build/.../compile_commands.json --output /tmp/cxxapi_bridge.rs

CxxApi is written to a short list of rules (plans/cxx-api.md) so that `cxx` can bind it without hand-written C++. This tool reads the
real headers through clang's AST and writes the bridge declaring every function, so a function added to CxxApi appears in the Rust
bridge after re-running it, and a signature that breaks a rule is rejected here with the function's name instead of surfacing later as
an opaque `cxx` error.

Rust names for the plain-data types come from bindgen run on `CxxApi/Types.hpp` with C++ namespaces enabled; the bridge refers to them
as `<types-path>::SFT::CxxApi::Name` (default `crate::types::root`) and declares each as a trivially relocatable extern type.
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

RUST_SCALARS = {
    "bool": "bool", "float": "f32", "double": "f64", "std::size_t": "usize", "size_t": "usize",
    "std::int8_t": "i8", "std::int16_t": "i16", "std::int32_t": "i32", "std::int64_t": "i64",
    "std::uint8_t": "u8", "std::uint16_t": "u16", "std::uint32_t": "u32", "std::uint64_t": "u64",
}


class RuleViolation(Exception):
    pass


def first_template_arg(spelled: str) -> str:
    """`std::unique_ptr<A<B>, D>` -> `A<B>`; the empty string when there is no template argument list."""
    start = spelled.find("<")
    if start < 0:
        return ""
    depth, begin = 0, start + 1
    for i in range(start, len(spelled)):
        c = spelled[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return spelled[begin:i].strip()
        elif c == "," and depth == 1:
            return spelled[begin:i].strip()
    return ""


def compile_flags(compile_commands: Path):
    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    entry = next((e for e in entries if e["file"].endswith("CxxApi/src/CxxApi/Scene.cpp")), None)
    if entry is None:
        sys.exit(f"{compile_commands}: no compile command for CxxApi/src/CxxApi/Scene.cpp (build CxxApi first)")
    words = shlex.split(entry.get("command") or " ".join(entry["arguments"]))
    flags, i = [], 0
    while i < len(words):
        w = words[i]
        if w == "-isystem" and i + 1 < len(words):
            flags += [w, words[i + 1]]
            i += 2
            continue
        if w.startswith(("-I", "-D", "-std=")):
            flags.append(w)
        i += 1
    return flags


def namespace_nodes(header: Path, flags):
    command = [os.environ.get("CLANGXX", "clang++"), "-x", "c++", "-fsyntax-only", *flags, "-Wno-pragma-once-outside-header", "-Xclang", "-ast-dump=json", "-Xclang",
               "-ast-dump-filter=CxxApi", str(header)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"clang failed on {header}:\n{result.stderr[-4000:]}")
    decoder, text, i, nodes = json.JSONDecoder(), result.stdout, 0, []
    while i < len(text):
        while i < len(text) and text[i].isspace():
            i += 1
        if i >= len(text):
            break
        node, i = decoder.raw_decode(text, i)
        nodes.append(node)
    # Clang's JSON writes a `file` only where it changes from the previous location, so the file of a declaration is whatever the walk
    # (in document order) last saw. Record it on each node as `_file`.
    current = {"file": ""}

    def walk(node):
        if not isinstance(node, dict):
            return
        loc = node.get("loc")
        if isinstance(loc, dict):
            current["file"] = loc.get("file") or loc.get("spellingLoc", {}).get("file") or current["file"]
        node["_file"] = current["file"]
        rng = node.get("range")
        if isinstance(rng, dict):
            for edge in ("begin", "end"):
                point = rng.get(edge, {})
                current["file"] = point.get("file") or point.get("spellingLoc", {}).get("file") or current["file"]
        for child in node.get("inner", []):
            walk(child)

    for node in nodes:
        walk(node)
    return nodes


def decl_file(node):
    return node.get("_file", "")


class Bridge:
    def __init__(self, nodes, types_path):
        self.types_path = types_path
        self.pods = {}      # name -> qualified C++ namespace ("SFT::CxxApi" or "SFT::RenderSettings")
        self.enums = {}
        self.classes = {}   # opaque types: written name -> (cxx namespace, rust name)
        self.declared_classes = set()  # classes declared in the CxxApi headers themselves (namespace SFT::CxxApi)
        self.functions = []
        self.vector_elements = set()
        self.raw_pointer_functions = set()
        self.used_pods = set()
        self.aliases = {}   # FrameSettings -> ::SFT::RenderSettings::FrameSettings
        decls = [d for n in nodes for d in n.get("inner", [])]
        for d in decls:
            kind, name = d["kind"], d.get("name", "")
            if kind == "TypeAliasDecl":
                target = d.get("type", {}).get("qualType", "").lstrip(":")
                self.aliases[name] = target
            elif kind == "EnumDecl" and name:
                self.enums[name] = "SFT::CxxApi"
            elif kind == "CXXRecordDecl" and d.get("completeDefinition") and name:
                path = decl_file(d) or ""
                if "/Types/" in path or "/Types.hpp" in path:
                    self.pods[name] = "SFT::CxxApi"
                else:
                    self.declared_classes.add(name)
                    self.classes[name] = ("SFT::CxxApi", name)
        for alias, target in self.aliases.items():
            # `using FrameSettings = ::SFT::RenderSettings::FrameSettings;` — a RenderSettings record bound as it is.
            if target.startswith("SFT::RenderSettings::"):
                self.pods[alias] = "SFT::RenderSettings"
                self.pod_rust_name = getattr(self, "pod_rust_name", {})
                self.pod_rust_name[alias] = target.split("::")[-1]
        header_files = {"Error.hpp", "Conversions.hpp"}
        for d in decls:
            if d["kind"] != "FunctionDecl":
                continue
            path = decl_file(d) or ""
            if not path or Path(path).name in header_files or "/Types/" in path:
                continue
            if "storageClass" in d and d["storageClass"] == "static":
                continue
            self.functions.append(d)

    # ---- type mapping ----

    def opaque(self, written: str, desugared: str):
        """Registers (and returns the Rust name of) a class used by reference or pointer."""
        qualified = (desugared or written).lstrip(":")
        qualified = re.sub(r"^(const\s+)", "", qualified).split("<")[0].strip()
        qualified = qualified.rstrip("&* ").strip()
        if not qualified.startswith("SFT::"):
            # A class declared in a CxxApi header lives in SFT::CxxApi; anything else named relative to SFT (`Engine::Engine`,
            # `Ecs::World`) is in SFT.
            qualified = "SFT::CxxApi::" + qualified if qualified in self.declared_classes else "SFT::" + qualified
        namespace, _, rust = qualified.rpartition("::")
        return namespace, rust

    def map_type(self, written: str, desugared: str, role: str, fn: str):
        """The Rust spelling of a C++ type as a parameter ("param") or return value ("ret"), by the plan's rules."""
        w = " ".join(written.split())
        const = bool(re.match(r"^const\b", w)) if w else False
        w = re.sub(r"^const\s+", "", w)
        if w == "void":
            return ""
        # std::unique_ptr<X>
        m = re.match(r"^std::unique_ptr<(.+)>$", w)
        if m:
            # The desugared spelling is `std::unique_ptr<SFT::X, std::default_delete<SFT::X>>`: its first template argument is the
            # fully qualified element type, which is what tells us the element's namespace.
            inner = self.map_inner(m.group(1), first_template_arg(desugared), fn, owned=True)
            return f"UniquePtr<{inner}>"
        # reference
        if w.endswith("&"):
            base = w[:-1].strip()
            inner = self.map_inner(base, desugared, fn, owned=False)
            kind = self.kind_of(base)
            if const:
                return f"&{inner}"
            if kind in ("scalar", "pod", "enum"):
                return f"&mut {inner}"
            return f"Pin<&mut {inner}>"
        # raw pointer (bulk bytes)
        if w.endswith("*"):
            base = w[:-1].strip()
            if base in ("std::uint8_t", "uint8_t"):
                self.raw_pointer_functions.add(fn)
                return f"*const u8" if const else "*mut u8"
            raise RuleViolation(f"{fn}: raw pointer parameter `{written}` (only `[const] std::uint8_t *` is allowed, as bulk bytes)")
        return self.map_inner(w, desugared, fn, owned=True)

    def kind_of(self, base: str):
        base = re.sub(r"^const\s+", "", base).strip()
        if base in RUST_SCALARS or re.match(r"^std::array<", base):
            return "scalar"
        if base in self.enums:
            return "enum"
        if base in self.pods or base in self.aliases:
            return "pod"
        return "other"

    def map_inner(self, base: str, desugared: str, fn: str, owned: bool):
        base = re.sub(r"^const\s+", "", base.strip()).strip()
        if base in RUST_SCALARS:
            return RUST_SCALARS[base]
        m = re.match(r"^std::array<float,\s*(\d+)>$", base)
        if m:
            return f"[f32; {m.group(1)}]"
        if base == "std::string":
            return "CxxString"
        m = re.match(r"^std::vector<(.+)>$", base)
        if m:
            element = self.map_inner(m.group(1), "", fn, owned=True)
            if m.group(1).strip() in self.pods:
                self.vector_elements.add(m.group(1).strip())
            return f"CxxVector<{element}>"
        if base in self.enums or base in self.pods or base in self.aliases:
            self.used_pods.add(base)
            return base if base not in getattr(self, "pod_rust_name", {}) else base
        if re.match(r"^[A-Za-z_][\w:]*$", base):
            namespace, rust = self.opaque(base, desugared)
            self.classes[base] = (namespace, rust)
            return rust
        raise RuleViolation(f"{fn}: type `{base}` is not allowed in a CxxApi signature (see plans/cxx-api.md)")

    # ---- emission ----

    def emit(self):
        problems, lines = [], []
        sigs = []
        for d in self.functions:
            name = d["name"]
            qual = d["type"]["qualType"]
            noexcept = "noexcept" in qual
            params = []
            try:
                for p in (c for c in d.get("inner", []) if c["kind"] == "ParmVarDecl"):
                    t = p["type"]
                    params.append((p.get("name") or f"arg{len(params)}", self.map_type(t["qualType"], t.get("desugaredQualType", ""), "param", name)))
                ret_spelled = qual.split("(")[0].strip()
                ret = self.map_type(ret_spelled, "", "ret", name)
            except RuleViolation as error:
                problems.append(str(error))
                continue
            sigs.append((name, params, ret, noexcept))
        if len({s[0] for s in sigs}) != len(sigs):
            seen, dup = set(), []
            for s in sigs:
                (dup if s[0] in seen else seen).add(s[0])
            problems.append(f"overloaded or duplicate function names (each name maps to one bridge line): {sorted(dup)}")
        if problems:
            raise RuleViolation("\n".join(problems))

        pods = sorted(self.used_pods)
        rust_pod = getattr(self, "pod_rust_name", {})
        lines += [
            "// Generated by CxxApi/tools/gen_bridge.py from the CxxApi headers. Do not edit: change the headers and re-run the tool.",
            "//",
            f"// The plain-data types are mirrored by bindgen (`bindgen CxxApi/Types.hpp --enable-cxx-namespaces`) into `{self.types_path}`.",
            "use cxx::{type_id, CxxString, CxxVector, ExternType, UniquePtr};",
            "use std::pin::Pin;",
            "",
        ]
        for name in pods:
            ns = self.pods.get(name) or self.enums.get(name) or "SFT::CxxApi"
            cxx_name = rust_pod.get(name, name)
            lines.append(f"unsafe impl ExternType for {self.types_path}::{ns}::{cxx_name} {{")
            lines.append(f'    type Id = type_id!("{ns}::{cxx_name}");')
            lines.append("    type Kind = cxx::kind::Trivial;")
            lines.append("}")
        lines += ["", "#[cxx::bridge]", "pub mod ffi {"]
        for name in pods:
            ns = self.pods.get(name) or self.enums.get(name) or "SFT::CxxApi"
            cxx_name = rust_pod.get(name, name)
            lines += ['    extern "C++" {', f'        #[namespace = "{ns}"]', f"        type {name} = {self.types_path}::{ns}::{cxx_name};", "    }"]
        lines += ['    unsafe extern "C++" {', '        include!("CxxApi/CxxApi.hpp");']
        rust_seen = {}
        for written, (namespace, rust) in sorted(self.classes.items(), key=lambda kv: kv[1][1]):
            if written in self.pods or written in self.enums:
                continue
            if rust in rust_seen and rust_seen[rust] != namespace:
                raise RuleViolation(f"two opaque types named `{rust}` in different namespaces ({rust_seen[rust]}, {namespace})")
            if rust in rust_seen:
                continue
            rust_seen[rust] = namespace
            lines += [f'        #[namespace = "{namespace}"]', f"        type {rust};"]
        lines += ["    }", '    #[namespace = "SFT::CxxApi"]', '    unsafe extern "C++" {', '        include!("CxxApi/CxxApi.hpp");']
        for name, params, ret, noexcept in sigs:
            args = ", ".join(f"{n}: {t}" for n, t in params)
            result = ret if noexcept else (f"Result<{ret}>" if ret else "Result<()>")
            unsafe = "unsafe " if name in self.raw_pointer_functions else ""
            lines.append(f"        {unsafe}fn {name}({args})" + (f" -> {result}" if result else "") + ";")
        lines.append("    }")
        for element in sorted(self.vector_elements):
            lines.append(f"    impl CxxVector<{element}> {{}}")
        lines.append("}")
        return "\n".join(lines) + "\n", len(sigs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--header", type=Path, default=None, help="default: CxxApi/src/CxxApi/CxxApi.hpp beside this tool")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--types-path", default="crate::types::root")
    args = parser.parse_args()
    header = args.header or Path(__file__).resolve().parent.parent / "src" / "CxxApi" / "CxxApi.hpp"

    bridge = Bridge(namespace_nodes(header, compile_flags(args.compile_commands)), args.types_path)
    try:
        text, count = bridge.emit()
    except RuleViolation as error:
        sys.exit(f"CxxApi breaks its binding rules:\n{error}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(f"{args.output}: {count} functions")


if __name__ == "__main__":
    main()
