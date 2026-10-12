#!/usr/bin/env python3
"""Describes the C ABI (`FFI/src/FFI/Sturdy.h`) as machine-readable JSON.

The header is the single source of truth. This tool asks clang for its AST (so it never parses C by hand) and writes every enum,
struct, callback type and function, with its documentation, to a JSON file that the binding generators (`gen_bindings.py`) consume.
Adding an entry point to the header and re-running the tools gives every language its binding; nothing is mirrored by hand.

    describe_abi.py --header FFI/src/FFI/Sturdy.h --output FFI/api/sturdy_abi.json

The type model is deliberately small: integers and floats by name and width, `pointer`, `array`, `enum`, `struct`, `callback` (a
named function-pointer type), and `function_pointer` (an unnamed one, as in a struct field). Typedef aliases of primitives keep their
alias name (`SturdyBool` is a `uint8_t` called `SturdyBool`) so a generator can choose to honour it.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

PRIMITIVES = {
    "void": ("void", 0, False),
    "_Bool": ("bool", 1, False),
    "bool": ("bool", 1, False),
    "char": ("char", 1, True),
    "signed char": ("int", 1, True),
    "unsigned char": ("int", 1, False),
    "short": ("int", 2, True),
    "unsigned short": ("int", 2, False),
    "int": ("int", 4, True),
    "unsigned int": ("int", 4, False),
    "unsigned": ("int", 4, False),
    "long": ("int", 8, True),
    "unsigned long": ("int", 8, False),
    "long long": ("int", 8, True),
    "unsigned long long": ("int", 8, False),
    "float": ("float", 4, True),
    "double": ("float", 8, True),
    "int8_t": ("int", 1, True),
    "int16_t": ("int", 2, True),
    "int32_t": ("int", 4, True),
    "int64_t": ("int", 8, True),
    "uint8_t": ("int", 1, False),
    "uint16_t": ("int", 2, False),
    "uint32_t": ("int", 4, False),
    "uint64_t": ("int", 8, False),
    # 64-bit targets only (the ABI says so): these are pointer-sized.
    "size_t": ("int", 8, False),
    "ssize_t": ("int", 8, True),
    "intptr_t": ("int", 8, True),
    "uintptr_t": ("int", 8, False),
    "ptrdiff_t": ("int", 8, True),
}


def clang_ast(header: Path, extra_args):
    # `-x c`: the ABI is a C header, and a C parse is what other languages' generators must agree with.
    command = ["clang", "-Xclang", "-ast-dump=json", "-fsyntax-only", "-x", "c", "-Wno-pragma-once-outside-header", *extra_args, str(header)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"clang failed on {header}:\n{result.stderr}")
    return json.loads(result.stdout)


def comment_text(node):
    """The doc comment of `node` as {summary, params, returns, notes}, from clang's FullComment."""
    full = next((c for c in node.get("inner", []) if c.get("kind") == "FullComment"), None)
    if full is None:
        return None

    def text_of(comment):
        parts = []
        for child in comment.get("inner", []):
            if child.get("kind") == "TextComment":
                parts.append(child.get("text", "").strip())
            elif child.get("kind") in ("ParagraphComment", "InlineCommandComment"):
                parts.append(text_of(child))
        return " ".join(p for p in parts if p)

    summary, params, returns, notes = [], {}, "", []
    for block in full.get("inner", []):
        kind = block.get("kind")
        if kind == "ParagraphComment":
            paragraph = text_of(block)
            if paragraph:
                summary.append(paragraph)
        elif kind == "ParamCommandComment":
            params[block.get("param", "")] = text_of(block)
        elif kind == "BlockCommandComment":
            name = block.get("name", "")
            if name in ("return", "returns"):
                returns = text_of(block)
            elif name in ("note", "warning"):
                notes.append(text_of(block))
    doc = {"summary": "\n\n".join(summary)}
    if params:
        doc["params"] = params
    if returns:
        doc["returns"] = returns
    if notes:
        doc["notes"] = notes
    return doc


class Describer:
    def __init__(self, ast, source):
        self.source = source
        self.source_bytes = source.encode("utf-8")
        self.nodes = ast["inner"]
        self.by_id = {n["id"]: n for n in self.nodes if "id" in n}
        self.typedefs = {}  # name -> resolved type
        self.enums, self.structs, self.callbacks, self.functions = {}, {}, {}, {}
        self.names = {"enum": set(), "struct": set(), "callback": set()}

    # ---- type resolution ----

    def parse_type(self, spelled: str):
        s = " ".join(spelled.replace("STURDY_ABI_CALL", "").split())
        # unnamed function pointer: `SturdyBool (*)(SturdyEngine, void *)`
        match = re.match(r"^(.*?)\s*\(\s*\*\s*\)\s*\((.*)\)$", s)
        if match:
            params = self.split_params(match.group(2))
            return {"kind": "function_pointer", "ret": self.parse_type(match.group(1)), "params": [self.parse_type(p) for p in params]}
        match = re.match(r"^(.*?)\s*\[(\d+)\]$", s)
        if match:
            return {"kind": "array", "of": self.parse_type(match.group(1)), "count": int(match.group(2))}
        if s.endswith("*") or re.search(r"\*\s*const$", s):
            body = re.sub(r"\s*const$", "", s)[:-1].strip()
            to = self.parse_type(body)
            return {"kind": "pointer", "to": to, "const": bool(re.match(r"^const\b", body)) or to.get("const_pointee", False)}
        const = False
        words = [w for w in s.split() if w not in ("volatile", "restrict")]
        if words and words[0] == "const":
            const = True
            words = words[1:]
        base = " ".join(w for w in words if w not in ("struct", "enum", "union"))
        return self.named(base, const)

    def named(self, name: str, const=False):
        if name in PRIMITIVES:
            kind, width, signed = PRIMITIVES[name]
            t = {"kind": kind} if kind in ("void", "bool") else {"kind": kind, "width": width * 8, "signed": signed}
            t["name"] = name
            if const:
                t["const_pointee"] = True
            return t
        if name in self.typedefs:
            t = dict(self.typedefs[name])
            if const:
                t["const_pointee"] = True
            return t
        if name in self.names["enum"]:
            return {"kind": "enum", "name": name}
        if name in self.names["struct"]:
            return {"kind": "struct", "name": name}
        if name in self.names["callback"]:
            return {"kind": "callback", "name": name}
        raise KeyError(f"unknown type `{name}`")

    @staticmethod
    def split_params(text: str):
        params, depth, current = [], 0, ""
        for ch in text:
            if ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            if ch == "," and depth == 0:
                params.append(current.strip())
                current = ""
            else:
                current += ch
        if current.strip() and current.strip() != "void":
            params.append(current.strip())
        return params

    # ---- collection ----

    def run(self):
        # Pass 1: names, so a type can refer to one declared later.
        for node in self.nodes:
            name = node.get("name", "")
            if not name.startswith(("Sturdy", "sturdy")):
                continue
            kind = node["kind"]
            if kind == "EnumDecl":
                self.names["enum"].add(name)
            elif kind == "RecordDecl" and node.get("completeDefinition"):
                self.names["struct"].add(name)
            elif kind == "TypedefDecl":
                inner = node.get("inner", [{}])[0]
                target = self.by_id.get(inner.get("decl", {}).get("id", ""))
                if target is not None and target.get("kind") in ("EnumDecl", "RecordDecl") and not target.get("name"):
                    # typedef struct { ... } Name;  /  typedef enum { ... } Name;
                    self.names["enum" if target["kind"] == "EnumDecl" else "struct"].add(name)
        for node in self.nodes:
            name = node.get("name", "")
            if node["kind"] == "TypedefDecl" and name.startswith(("Sturdy", "sturdy")):
                spelled = node.get("type", {}).get("qualType", "")
                if "(*)" in spelled and name not in self.names["enum"] | self.names["struct"]:
                    self.names["callback"].add(name)
        # Pass 2: primitive typedefs (SturdyBool and friends).
        for node in self.nodes:
            name = node.get("name", "")
            if node["kind"] == "TypedefDecl" and name.startswith(("Sturdy", "sturdy")) and name not in self.names["enum"] | self.names["struct"] | self.names["callback"]:
                spelled = node.get("type", {}).get("qualType", "")
                try:
                    resolved = self.parse_type(spelled)
                except KeyError:
                    continue
                resolved["alias"] = name
                self.typedefs[name] = resolved
        # Pass 3: the declarations.
        for node in self.nodes:
            kind, name = node["kind"], node.get("name", "")
            if kind == "EnumDecl" and name in self.names["enum"]:
                self.add_enum(name, node)
            elif kind == "RecordDecl" and name in self.names["struct"] and node.get("completeDefinition"):
                self.add_struct(name, node)
            elif kind == "TypedefDecl" and name.startswith(("Sturdy", "sturdy")):
                inner = node.get("inner", [{}])[0]
                target = self.by_id.get(inner.get("decl", {}).get("id", ""))
                if target is not None and not target.get("name") and target["kind"] == "EnumDecl":
                    self.add_enum(name, target, node)
                elif target is not None and not target.get("name") and target["kind"] == "RecordDecl":
                    self.add_struct(name, target, node)
                elif name in self.names["callback"]:
                    self.add_callback(name, node)
            elif kind == "FunctionDecl" and name.startswith("sturdy_") and not node.get("isImplicit"):
                self.add_function(name, node)

    def add_enum(self, name, node, typedef=None):
        if name in self.enums:
            return
        values = []
        for constant in node.get("inner", []):
            if constant.get("kind") != "EnumConstantDecl":
                continue
            value = next((c.get("value") for c in constant.get("inner", []) if c.get("kind") == "ConstantExpr"), None)
            entry = {"name": constant["name"], "value": int(value) if value is not None else (values[-1]["value"] + 1 if values else 0)}
            doc = comment_text(constant)
            if doc:
                entry["doc"] = doc["summary"]
            values.append(entry)
        # `*_FORCE_U32` sentinels only exist to pin the underlying type to 32 bits in C; bindings declare that themselves.
        values = [v for v in values if not v["name"].endswith("_FORCE_U32")]
        self.enums[name] = {"name": name, "doc": (comment_text(node) or comment_text(typedef or {}) or {}).get("summary", ""), "values": values}

    def add_struct(self, name, node, typedef=None):
        if name in self.structs:
            return
        fields = []
        for field in node.get("inner", []):
            if field.get("kind") != "FieldDecl":
                continue
            spelled = field["type"]["qualType"]
            entry = {"name": field["name"], "type": self.parse_type(spelled)}
            doc = comment_text(field)
            if doc:
                entry["doc"] = doc["summary"]
            fields.append(entry)
        doc = comment_text(node) or comment_text(typedef or {}) or {}
        self.structs[name] = {"name": name, "doc": doc.get("summary", ""), "fields": fields}

    def add_callback(self, name, node):
        spelled = node["type"]["qualType"]
        fp = self.parse_type(spelled)
        if fp["kind"] == "pointer":
            fp = fp["to"]
        if fp["kind"] != "function_pointer":
            raise KeyError(f"`{name}` is not a function pointer type")
        # Parameter names are not in the type, only in the source text of the declaration.
        # `typedef R (*Name)(params);` — the parameter list follows the name, so slice the whole declaration.
        begin = node["range"]["begin"]["offset"]
        end = node["range"]["end"]["offset"] + node["range"]["end"].get("tokLen", 1)
        # Clang's offsets are in bytes; the header has non-ASCII text, so slice the encoded source, not the decoded string.
        snippet = self.source_bytes[begin:end].decode("utf-8", errors="replace")
        match = re.search(r"\(\s*(?:\w+\s+)?\*\s*" + re.escape(name) + r"\s*\)\s*\((.*)\)\s*;?\s*$", snippet, re.S)
        names = []
        if match:
            for p in self.split_params(match.group(1)):
                found = re.search(r"([A-Za-z_]\w*)\s*(?:\[\d*\])?$", p)
                names.append(found.group(1) if found else "")
        params = [{"name": names[i] if i < len(names) and names[i] else f"arg{i}", "type": t} for i, t in enumerate(fp["params"])]
        doc = comment_text(node) or {}
        self.callbacks[name] = {"name": name, "doc": doc.get("summary", ""), "ret": fp["ret"], "params": params}

    def add_function(self, name, node):
        params = []
        doc = comment_text(node) or {}
        for param in (c for c in node.get("inner", []) if c.get("kind") == "ParmVarDecl"):
            spelled = param["type"]["qualType"]
            parsed = self.parse_type(spelled)
            # `T name[]` parameters decay to pointers; clang already spells them `T *`.
            entry = {"name": param.get("name") or f"arg{len(params)}", "type": parsed}
            if entry["name"] in doc.get("params", {}):
                entry["doc"] = doc["params"][entry["name"]]
            params.append(entry)
        ret = self.parse_type(node["type"]["qualType"].split("(", 1)[0])
        entry = {"name": name, "doc": doc.get("summary", ""), "ret": ret, "params": params}
        if doc.get("returns"):
            entry["returns"] = doc["returns"]
        if doc.get("notes"):
            entry["notes"] = doc["notes"]
        self.functions[name] = entry


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("-I", dest="includes", action="append", default=[])
    args = parser.parse_args()

    source = args.header.read_text(encoding="utf-8")
    describer = Describer(clang_ast(args.header, [f"-I{i}" for i in args.includes]), source)
    describer.run()

    version = {}
    for part in ("MAJOR", "MINOR"):
        match = re.search(rf"#define\s+STURDY_ABI_VERSION_{part}\s+(\d+)u?", source)
        if not match:
            sys.exit(f"STURDY_ABI_VERSION_{part} not found in {args.header}")
        version[part.lower()] = int(match.group(1))

    description = {
        "abi_version": version,
        "source": args.header.name,
        "enums": sorted(describer.enums.values(), key=lambda e: e["name"]),
        "structs": sorted(describer.structs.values(), key=lambda s: s["name"]),
        "callbacks": sorted(describer.callbacks.values(), key=lambda c: c["name"]),
        "functions": sorted(describer.functions.values(), key=lambda f: f["name"]),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Stable output (sorted, fixed formatting, trailing newline) so a regenerated file differs only when the header does.
    args.output.write_text(json.dumps(description, indent=1, sort_keys=False) + "\n", encoding="utf-8")
    print(f"{args.output}: ABI {version['major']}.{version['minor']}, {len(description['functions'])} functions, "
          f"{len(description['structs'])} structs, {len(description['enums'])} enums, {len(description['callbacks'])} callbacks")


if __name__ == "__main__":
    main()
