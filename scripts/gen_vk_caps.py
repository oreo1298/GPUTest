#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the Vulkan capability-explorer code (src/rhi/vulkan/vk_caps_gen.inc) from vk.xml.

Usage:
    python3 scripts/gen_vk_caps.py <path/to/vk.xml> <out.inc>

The output defines, in namespace gt::rhi::vk::gen:

    void collectFeatures(VkPhysicalDevice, std::uint32_t apiVersion,
                         const std::function<bool(const char*)>& hasExtension, std::vector<Capability>&);
    void collectProperties(VkPhysicalDevice, std::uint32_t apiVersion,
                           const std::function<bool(const char*)>& hasExtension, std::vector<Capability>&);

Each builds one pNext chain (VkPhysicalDeviceFeatures2 / VkPhysicalDeviceProperties2) holding the
VkPhysicalDeviceVulkanNN aggregates allowed by apiVersion plus every structure that extends it and is
provided by a (non-platform, enabled, device) extension the device supports, queries it once, and turns
every member into a Capability row. Rows come from generated tables (member name, offsetof, formatter)
rather than one statement per row, which keeps the two functions small and quick to optimize.

Selection rules (see also the comments in the generated file):
  * Alias types are folded into the structure they alias; a structure is chained at most once.
  * Structures provided only by core versions are covered by the VulkanNN aggregates and skipped.
  * Structures that were promoted to core (an extension provides them, usually under an alias name) are
    chained under the extension only while the aggregate covering them is not chained
    (apiVersion < the aggregate's version), so a modern device does not list them twice.
  * Extensions with a `platform` attribute, `supported` not containing "vulkan", or type="instance"
    are ignored. `<require depends="...">` expressions are honoured.
  * Members with api="vulkansc" (only), pointers, bitfields, multi-dimensional arrays and types that
    have no formatter are skipped (the generator reports the latter on stderr).
  * Enum values are named via generated switches (extension enumerants included: offset/extnumber/bitpos/
    value; aliases, platform and disabled extensions skipped; duplicate values collapsed); flags are
    listed bit by bit from the bitmask's `requires`/`bitvalues` enum, unknown bits in hex, 0 as "none".

Only the Python standard library is used and the output is deterministic for a given vk.xml.
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET

FEATURES2 = "VkPhysicalDeviceFeatures2"
PROPERTIES2 = "VkPhysicalDeviceProperties2"

INT_TYPES = {
    "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t", "int",
}
FLOAT_TYPES = {"float", "double"}

VERSION_RE = re.compile(r"^VK(?:_[A-Z]+)?_VERSION_(\d+)_(\d+)$")
AGGREGATE_RE = re.compile(r"^VkPhysicalDeviceVulkan(\d)(\d)(Features|Properties)$")


def api_ok(elem: ET.Element) -> bool:
    api = elem.get("api")
    return api is None or "vulkan" in api.split(",")


def version_tuple(number: str) -> tuple[int, int]:
    major, minor = number.split(".")
    return int(major), int(minor)


def version_macro(ver: tuple[int, int]) -> str:
    return f"VK_API_VERSION_{ver[0]}_{ver[1]}"


def cpp_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Member:
    def __init__(self, elem: ET.Element):
        name_el = elem.find("name")
        type_el = elem.find("type")
        self.name: str = name_el.text.strip() if name_el is not None and name_el.text else ""
        self.type: str = type_el.text.strip() if type_el is not None and type_el.text else ""
        self.values = elem.get("values")
        before = (elem.text or "") + ((type_el.tail or "") if type_el is not None else "")
        self.pointer = "*" in before
        after = ""
        seen_name = False
        for child in elem:
            if child is name_el:
                seen_name = True
                after += child.tail or ""
            elif seen_name and child.tag != "comment":
                after += (child.text or "") + (child.tail or "")
        self.dims = re.findall(r"\[\s*([A-Za-z0-9_]+)\s*\]", after)
        self.bitfield = ":" in after
        # VkPhysicalDeviceVulkan14Properties & co. map members back to the promoted structure.
        self.alias = name_el.get("alias") if name_el is not None else None


class Struct:
    def __init__(self, name: str, elem: ET.Element, index: int):
        self.name = name
        self.index = index
        self.extends = [s for s in (elem.get("structextends") or "").split(",") if s]
        self.members = [Member(m) for m in elem.findall("member") if api_ok(m)]
        self.stype = None
        for m in self.members:
            if m.name == "sType" and m.values:
                self.stype = m.values.split(",")[0]

    def fields(self) -> list[Member]:
        return [m for m in self.members if m.name not in ("sType", "pNext")]


class Provider:
    def __init__(self, ext: str, depends: str | None, canonical: bool, index: int):
        self.ext = ext
        self.depends = depends
        self.canonical = canonical
        self.index = index


class Registry:
    def __init__(self, root: ET.Element):
        self.alias: dict[str, str] = {}
        self.category: dict[str, str] = {}
        self.structs: dict[str, Struct] = {}
        self.struct_order: list[str] = []
        self.bitmask_bits: dict[str, str | None] = {}
        self.basetypes: dict[str, str | None] = {}
        self.enum_kind: dict[str, str] = {}
        self.enum_bitwidth: dict[str, int] = {}
        self.enumerants: dict[str, list[tuple[str, int]]] = {}
        self._enumerant_names: dict[str, set[str]] = {}
        self.header_version = None
        self.header_version_complete = None
        self.core_version: dict[str, tuple[int, int]] = {}
        self.providers: dict[str, list[Provider]] = {}
        self.warnings: list[str] = []
        self._parse_types(root)
        self._parse_enums(root)
        self._parse_features(root)
        self._parse_extensions(root)

    # --- parsing ---------------------------------------------------------------------------------------
    def _parse_types(self, root: ET.Element) -> None:
        for index, t in enumerate(root.find("types").findall("type")):
            if not api_ok(t):
                continue
            cat = t.get("category")
            name_el = t.find("name")
            name = t.get("name") or (name_el.text if name_el is not None else None)
            if not name:
                continue
            if cat == "define":
                text = "".join(t.itertext())
                if name == "VK_HEADER_VERSION":
                    m = re.search(r"VK_HEADER_VERSION\s+(\d+)", text)
                    if m:
                        self.header_version = int(m.group(1))
                elif name == "VK_HEADER_VERSION_COMPLETE":
                    m = re.search(r"\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*VK_HEADER_VERSION\s*\)", text)
                    if m:
                        self.header_version_complete = (int(m.group(2)), int(m.group(3)))
                continue
            if cat is not None:
                self.category.setdefault(name, cat)
            if t.get("alias"):
                self.alias[name] = t.get("alias")
                continue
            if cat in ("struct", "union"):
                if name not in self.structs:
                    self.structs[name] = Struct(name, t, index)
                    self.struct_order.append(name)
            elif cat == "bitmask":
                self.bitmask_bits[name] = t.get("requires") or t.get("bitvalues")
            elif cat == "basetype":
                base = t.find("type")
                text = "".join(t.itertext())
                self.basetypes[name] = base.text if base is not None and "*" not in text else None

    def canonical(self, name: str) -> str:
        seen = set()
        while name in self.alias and name not in seen:
            seen.add(name)
            name = self.alias[name]
        return name

    def _add_enumerant(self, enum: str, name: str, value: int) -> None:
        names = self._enumerant_names.setdefault(enum, set())
        if name in names:
            for n, v in self.enumerants[enum]:
                if n == name and v != value:
                    self.warnings.append(f"enumerant {name} defined with values {v} and {value}")
            return
        names.add(name)
        self.enumerants.setdefault(enum, []).append((name, value))

    @staticmethod
    def _int(text: str) -> int | None:
        try:
            return int(text, 0)
        except ValueError:
            return None

    def _parse_enums(self, root: ET.Element) -> None:
        for es in root.findall("enums"):
            kind = es.get("type")
            if kind not in ("enum", "bitmask"):
                continue
            ename = es.get("name")
            self.enum_kind[ename] = kind
            self.enum_bitwidth[ename] = int(es.get("bitwidth") or 32)
            self.enumerants.setdefault(ename, [])
            for e in es.findall("enum"):
                if not api_ok(e) or e.get("alias"):
                    continue
                value = None
                if e.get("value") is not None:
                    value = self._int(e.get("value"))
                elif e.get("bitpos") is not None:
                    value = 1 << int(e.get("bitpos"))
                if value is not None:
                    self._add_enumerant(ename, e.get("name"), value)

    def _add_extending_enum(self, e: ET.Element, default_extnumber: int | None) -> None:
        target = e.get("extends")
        if not target or not api_ok(e) or e.get("alias"):
            return
        value = None
        if e.get("offset") is not None:
            extnumber = e.get("extnumber") or default_extnumber
            if extnumber is None:
                self.warnings.append(f"no extnumber for {e.get('name')}")
                return
            value = 1000000000 + (int(extnumber) - 1) * 1000 + int(e.get("offset"))
            if e.get("dir") == "-":
                value = -value
        elif e.get("bitpos") is not None:
            value = 1 << int(e.get("bitpos"))
        elif e.get("value") is not None:
            value = self._int(e.get("value"))
        if value is not None:
            self._add_enumerant(target, e.get("name"), value)

    def _parse_features(self, root: ET.Element) -> None:
        for f in root.findall("feature"):
            if not api_ok(f):
                continue
            ver = version_tuple(f.get("number"))
            for req in f.findall("require"):
                if not api_ok(req):
                    continue
                for e in req.findall("enum"):
                    self._add_extending_enum(e, None)
                for t in req.findall("type"):
                    name = self.canonical(t.get("name"))
                    if name not in self.core_version or ver < self.core_version[name]:
                        self.core_version[name] = ver

    def _parse_extensions(self, root: ET.Element) -> None:
        index = 0
        for ext in root.find("extensions").findall("extension"):
            supported = (ext.get("supported") or "").split(",")
            if "vulkan" not in supported or ext.get("platform"):
                continue
            is_device = ext.get("type") == "device"
            number = int(ext.get("number"))
            for req in ext.findall("require"):
                if not api_ok(req):
                    continue
                for e in req.findall("enum"):
                    self._add_extending_enum(e, number)
                if not is_device:
                    continue
                for t in req.findall("type"):
                    raw = t.get("name")
                    name = self.canonical(raw)
                    providers = self.providers.setdefault(name, [])
                    if any(p.ext == ext.get("name") and p.depends == req.get("depends") for p in providers):
                        continue
                    providers.append(Provider(ext.get("name"), req.get("depends"), raw == name, index))
                    index += 1
        for providers in self.providers.values():
            providers.sort(key=lambda p: (0 if p.canonical else 1, p.index))


# --- depends="..." expressions ---------------------------------------------------------------------------
def depends_to_cpp(expr: str) -> str:
    """Translate a vk.xml dependency expression ('+' = and, ',' = or, equal precedence, left-assoc)."""
    tokens = re.findall(r"[A-Za-z0-9_:]+|[+,()]", expr)
    pos = 0

    def primary() -> str:
        nonlocal pos
        tok = tokens[pos]
        pos += 1
        if tok == "(":
            inner = expression()
            pos += 1  # ')'
            return f"({inner})"
        m = VERSION_RE.match(tok)
        if m:
            return f"apiVersion >= VK_API_VERSION_{m.group(1)}_{m.group(2)}"
        if "::" in tok:  # feature-bit dependency: not knowable before the query, assume present
            return "true"
        return f"hasExtension({cpp_string(tok)})"

    def expression() -> str:
        nonlocal pos
        result = primary()
        while pos < len(tokens) and tokens[pos] in ("+", ","):
            op = "&&" if tokens[pos] == "+" else "||"
            pos += 1
            rhs = primary()
            result = f"({result} {op} {rhs})"
        return result

    return expression()


# --- code generation ---------------------------------------------------------------------------------------
STD_TYPES = {"uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t"}


class Generator:
    """Emits row tables (name + offsetof + formatter) and the two collect functions.

    Table-driven on purpose: straight-line code with one call per row (~2500 rows) made the optimizer
    take minutes on the two huge functions; tables keep them small and fast to compile.
    """

    def __init__(self, reg: Registry):
        self.reg = reg
        self.used_enums: list[str] = []
        self.used_flags: list[str] = []
        self.used: set[str] = set()
        self.skipped_types: dict[str, int] = {}
        self.stats: dict[str, tuple[int, int, int]] = {}
        self.depends_funcs: dict[str, str] = {}

    # Formatting -----------------------------------------------------------------------------------------
    def numeric(self, t: str) -> str | None:
        """C++ spelling of a plain number type (VkBool32 / VkFlags excluded), else None."""
        if t in STD_TYPES:
            return f"std::{t}"
        if t in INT_TYPES or t in FLOAT_TYPES:
            return t
        if t in ("VkBool32", "VkFlags", "VkFlags64"):
            return None
        if self.reg.basetypes.get(t) in INT_TYPES:
            return t
        return None

    def skip(self, what: str) -> None:
        self.skipped_types[what] = self.skipped_types.get(what, 0) + 1

    def formatter(self, m: Member) -> str | None:
        """Address of the `std::string (*)(const void*)` formatting member `m`, or None to skip it."""
        reg = self.reg
        if m.pointer or m.bitfield or len(m.dims) > 1:
            return None
        t = reg.canonical(m.type)
        if len(m.dims) == 1:
            dim = m.dims[0]
            if t == "char":
                return f"&capFmtString<{dim}>"
            if t == "uint8_t":
                return f"&capFmtHex<{dim}>"
            num = self.numeric(t)
            if num is not None:
                return f"&capFmtNumArray<{num}, {dim}>"
            self.skip(f"{t}[]")
            return None
        if t == "VkBool32":
            return "&capFmtBool"
        num = self.numeric(t)
        if num is not None:
            return f"&capFmtNum<{num}>"
        if t in ("VkExtent2D", "VkExtent3D", "VkConformanceVersion"):
            self.used.add(t)
            return f"&capFmt{t[2:]}"
        if reg.category.get(t) == "enum" and reg.enumerants.get(t):
            if t not in self.used_enums:
                self.used_enums.append(t)
            return f"&capFmtEnum_{t}"
        if reg.category.get(t) == "bitmask":
            if t not in self.used_flags:
                self.used_flags.append(t)
            return f"&capFmtFlags_{t}"
        if t in ("VkFlags", "VkFlags64"):
            self.used.add("rawFlags")
            return f"&capFmtRawFlags<{t}>"
        self.skip(t)
        return None

    # Struct selection -------------------------------------------------------------------------------------
    def aggregates(self, kind: str) -> list[tuple[Struct, tuple[int, int], tuple[int, int]]]:
        """[(struct, core version it describes, version gating the chain)] for VkPhysicalDeviceVulkanNN<kind>."""
        base = FEATURES2 if kind == "Features" else PROPERTIES2
        out = []
        for name in self.reg.struct_order:
            m = AGGREGATE_RE.match(name)
            if not m or m.group(3) != kind or base not in self.reg.structs[name].extends:
                continue
            describes = (int(m.group(1)), int(m.group(2)))
            gate = max(describes, (1, 2), self.reg.core_version.get(name, (1, 2)))
            out.append((self.reg.structs[name], describes, gate))
        out.sort(key=lambda a: a[1])
        return out

    @staticmethod
    def is_feature_member(m: Member) -> bool:
        return m.type == "VkBool32" and not m.pointer and not m.dims and not m.bitfield

    def covered_by(self, s: Struct, aggregate: Struct, kind: str) -> bool:
        """True when every member of `s` also appears in the VulkanNN aggregate (by name or alias=)."""
        agg_names = [a.name.lower() for a in aggregate.fields()]
        agg_alias = {a.alias for a in aggregate.fields() if a.alias}
        names = [s.name] + sorted(k for k, v in self.reg.alias.items() if v == s.name)
        for m in s.fields():
            if (kind == "Features" and not self.is_feature_member(m)) or (kind != "Features" and m.pointer):
                continue
            if any(f"{n}::{m.name}" in agg_alias for n in names):
                continue
            if not any(a.endswith(m.name.lower()) for a in agg_names):
                return False
        return True

    def extension_structs(self, kind: str, aggregates) -> list[tuple[Struct, list[Provider], tuple | None]]:
        base = FEATURES2 if kind == "Features" else PROPERTIES2
        agg_by_version = {describes: (s, gate) for s, describes, gate in aggregates}
        agg_names = {s.name for s, _, _ in aggregates}
        result = []
        for name in self.reg.struct_order:
            s = self.reg.structs[name]
            if base not in s.extends or name in agg_names or s.stype is None:
                continue
            providers = self.reg.providers.get(name)
            core = self.reg.core_version.get(name)
            covered = core in agg_by_version and self.covered_by(s, agg_by_version[core][0], kind)
            if not providers:
                if core is not None and not covered:
                    self.reg.warnings.append(f"core-only {name} is not covered by a VulkanNN aggregate")
                continue  # core-only (covered by the aggregates), platform, disabled or instance-only
            gate = None
            if core is not None:
                if covered:
                    gate = agg_by_version[core][1]
                else:
                    self.reg.warnings.append(f"promoted {name} is not covered by a VulkanNN aggregate")
            result.append((s, providers, gate))
        result.sort(key=lambda r: (r[1][0].ext, r[0].index))
        return result

    # Emission ---------------------------------------------------------------------------------------------
    @staticmethod
    def var_name(struct: str) -> str:
        for prefix in ("VkPhysicalDevice", "Vk"):
            if struct.startswith(prefix):
                return struct[len(prefix):]
        return struct

    def table(self, kind: str, s: Struct, table_name: str, special: dict[str, str] | None = None,
              exclude: set[str] | None = None) -> list[str] | None:
        """Row table for `s`, or None when no member can be listed."""
        rows = []
        for m in s.fields():
            if exclude and m.name in exclude:
                continue
            offset = f"offsetof({s.name}, {m.name})"
            if kind == "Features":
                if self.is_feature_member(m):
                    rows.append(f'    {{"{m.name}", {offset}}},')
                continue
            fmt = (special or {}).get(m.name) or self.formatter(m)
            if fmt is not None:
                rows.append(f'    {{"{m.name}", {offset}, {fmt}}},')
        if not rows:
            return None
        row_type = "CapBoolField" if kind == "Features" else "CapField"
        return [f"const {row_type} {table_name}[] = {{"] + rows + ["};"]

    def depends_function(self, expr: str) -> str:
        """Name of the generated predicate for a `<require depends="...">` expression."""
        if expr not in self.depends_funcs:
            self.depends_funcs[expr] = f"capDepends{len(self.depends_funcs)}"
        return self.depends_funcs[expr]

    def gen_depends(self) -> list[str]:
        L: list[str] = []
        for expr, name in self.depends_funcs.items():
            body = depends_to_cpp(expr)
            api = "apiVersion" if "apiVersion" in body else "/*apiVersion*/"
            has = "hasExtension" if "hasExtension" in body else "/*hasExtension*/"
            L += ["", f"// depends=\"{expr}\"",
                  f"bool {name}(std::uint32_t {api}, const CapHasExtension& {has}) {{ return {body}; }}"]
        return L

    def gen_collect(self, kind: str, tables: list[str]) -> list[str]:
        reg = self.reg
        top = FEATURES2 if kind == "Features" else PROPERTIES2
        core_member = "features" if kind == "Features" else "properties"
        prefix = "kFeat" if kind == "Features" else "kProp"
        kind1 = kind[:-1] if kind == "Features" else "Property"  # Feature / Property
        storage = f"Cap{kind1}Storage"
        chain_table = f"kCap{kind1}Chain"
        provider_table = f"kCap{kind1}Providers"
        get2 = f"vkGetPhysicalDevice{kind}2"
        get1 = f"vkGetPhysicalDevice{kind}"

        # Row tables for the core 1.0 structures.
        core_rows: list[tuple[str, str, str]] = []  # (group, address expression, table)
        if kind == "Features":
            tables += [""] + self.table(kind, reg.structs["VkPhysicalDeviceFeatures"], f"{prefix}_Core")
            core_rows.append(("Features: Vulkan 1.0", "&top.features", f"{prefix}_Core"))
        else:
            props = reg.structs["VkPhysicalDeviceProperties"]
            nested = {m.name: m.type for m in props.fields()
                      if m.type in ("VkPhysicalDeviceLimits", "VkPhysicalDeviceSparseProperties") and not m.dims}
            self.used.add("apiVersion")
            tables += [""] + self.table(kind, props, f"{prefix}_Core", special={"apiVersion": "&capFmtApiVersion"},
                                        exclude=set(nested))
            core_rows.append(("Properties: Vulkan 1.0", "&top.properties", f"{prefix}_Core"))
            for member, struct in nested.items():
                label = "Limits" if struct == "VkPhysicalDeviceLimits" else "Sparse"
                group = "Limits" if struct == "VkPhysicalDeviceLimits" else "Sparse properties"
                tables += [""] + self.table(kind, reg.structs[struct], f"{prefix}_{label}")
                core_rows.append((group, f"&top.properties.{member}", f"{prefix}_{label}"))

        # Row tables for everything that can be chained: aggregates first, then extension structures.
        entries: list[tuple[Struct, str, tuple | None, list[Provider], tuple | None]] = []
        for s, describes, gate in self.aggregates(kind):
            t = self.table(kind, s, f"{prefix}_{self.var_name(s.name)}")
            if t:
                tables += [""] + t
                entries.append((s, f"{kind}: Vulkan {describes[0]}.{describes[1]}", gate, [], None))
        n_aggregates = len(entries)
        for s, providers, below in self.extension_structs(kind, self.aggregates(kind)):
            t = self.table(kind, s, f"{prefix}_{self.var_name(s.name)}")
            if t:
                tables += [""] + t
                entries.append((s, "", None, providers, below))
        self.stats[kind] = (n_aggregates, len(entries) - n_aggregates, sum(len(e[3]) for e in entries))

        # Storage: every candidate structure as a member of one zero-initialized local.
        tables += ["", f"// Every structure collect{kind}() may chain; one zero-initialized local holds them all.",
                   f"struct {storage} {{"]
        tables += [f"    {s.name} s{self.var_name(s.name)};" for s, *_ in entries]
        tables += ["};"]

        # Providing extensions, in priority order, referenced by index from the chain table.
        provider_rows: list[str] = []
        first: list[int] = []
        for s, group, gate, providers, below in entries:
            first.append(len(provider_rows))
            for p in providers:
                dep = self.depends_function(p.depends) if p.depends else "nullptr"
                provider_rows.append(f"    {{{cpp_string(p.ext)}, {dep}}},")
        tables += [""]
        if provider_rows:
            tables += [f"const CapProvider {provider_table}[] = {{"] + provider_rows + ["};"]
        else:
            tables += [f"const CapProvider* const {provider_table} = nullptr;"]

        row_type = "CapBoolField" if kind == "Features" else "CapField"
        tables += ["", "// offset, sType, aggregate group, min apiVersion, chained while apiVersion < (0: always),",
                   "// first provider, provider count, rows",
                   f"const CapChainEntry<{row_type}> {chain_table}[] = {{"]
        for (s, group, gate, providers, below), index in zip(entries, first):
            v = self.var_name(s.name)
            group_expr = cpp_string(group) if group else "nullptr"
            min_ver = version_macro(gate) if gate else "0"
            below_ver = version_macro(below) if below else "0"
            tables.append(f"    {{offsetof({storage}, s{v}), {s.stype}, {group_expr}, {min_ver}, {below_ver}, "
                          f"{index}, {len(providers)}, {prefix}_{v}, std::size({prefix}_{v})}},")
        tables += ["};"]

        L: list[str] = []
        L.append(f"void collect{kind}(VkPhysicalDevice pd, std::uint32_t apiVersion,")
        L.append(f"{' ' * (len(kind) + 13)}const std::function<bool(const char*)>& hasExtension, "
                 "std::vector<Capability>& out) {")
        L.append(f"    {top} top{{}};")
        L.append(f"    top.sType = {reg.structs[top].stype};")
        L.append(f"    {storage} storage{{}};")
        L.append("    std::vector<std::string> groups; // per chain entry: row group, or empty when not chained")
        L.append(f"    PFN_{get2} query = {get2} != nullptr ? {get2} : {get2}KHR;")
        L.append("    if (query != nullptr) {")
        L.append(f"        groups = capBuildChain(&top, &storage, {chain_table}, std::size({chain_table}), "
                 f"{provider_table}, apiVersion,")
        L.append(f'                               hasExtension, "{kind}: ");')
        L.append("        query(pd, &top);")
        L.append(f"    }} else if ({get1} != nullptr) {{")
        L.append(f"        {get1}(pd, &top.{core_member});")
        L.append("    }")
        for group, addr, table in core_rows:
            L.append(f'    capAddRows(out, "{group}", {addr}, {table}, std::size({table}));')
        L.append(f"    capAddChained(out, groups, &storage, {chain_table});")
        L.append("}")
        return L

    def gen_enum(self, t: str) -> list[str]:
        reg = self.reg
        wide = reg.enum_bitwidth.get(t, 32) == 64
        cast = "unsigned long long" if wide else "long long"
        L = [f"std::string capFmtEnum_{t}(const void* p) {{",
             f"    const {cast} v = static_cast<{cast}>(capLoad<{t}>(p));",
             "    switch (v) {"]
        seen: set[int] = set()
        for name, value in reg.enumerants[t]:
            if value in seen or (wide and value < 0):
                continue  # duplicate values (no duplicate case labels)
            seen.add(value)
            L.append(f"    case {name}: return {cpp_string(name)};")
        L += ["    default: break;", "    }", "    return std::to_string(v);", "}"]
        return L

    def gen_flags(self) -> list[str]:
        reg = self.reg
        L: list[str] = []
        emitted: set[str] = set()
        for flags in self.used_flags:
            bits = reg.bitmask_bits.get(flags)
            bits = reg.canonical(bits) if bits else None
            entries = []
            seen: set[int] = set()
            for name, value in sorted(reg.enumerants.get(bits, []) if bits else [], key=lambda nv: nv[1]):
                if value <= 0 or value & (value - 1) or value in seen:
                    continue  # single bits only (skips NONE / ALL / combined masks and duplicates)
                seen.add(value)
                entries.append(name)
            L.append("")
            if entries:
                table = f"kCapBits_{bits}"
                if bits not in emitted:
                    emitted.add(bits)
                    L.append(f"constexpr CapFlagBit {table}[] = {{")
                    L += [f"    {{static_cast<unsigned long long>({n}), {cpp_string(n)}}}," for n in entries]
                    L += ["};", ""]
                L.append(f"std::string capFmtFlags_{flags}(const void* p) {{")
                L.append(f"    return capFlagsString(capLoad<{flags}>(p), {table}, std::size({table}));")
            else:
                L.append(f"std::string capFmtFlags_{flags}(const void* p) {{")
                L.append(f"    return capFlagsString(capLoad<{flags}>(p), nullptr, 0);")
            L.append("}")
        return L

    def helpers(self) -> list[str]:
        L = r"""
struct CapBoolField {
    const char* name;
    std::size_t offset;
};

using CapFormatFn = std::string (*)(const void*);

struct CapField {
    const char* name;
    std::size_t offset;
    CapFormatFn format;
};

struct CapFlagBit {
    unsigned long long bit;
    const char* name;
};

using CapHasExtension = std::function<bool(const char*)>;

struct CapProvider {
    const char* extension;
    bool (*depends)(std::uint32_t apiVersion, const CapHasExtension& hasExtension); // <require depends>, or nullptr
};

template <typename Field>
struct CapChainEntry {
    std::size_t offset;            // of the structure inside the storage struct
    VkStructureType sType;
    const char* coreGroup;         // VulkanNN aggregates: row group; extension structures: nullptr
    std::uint32_t minApiVersion;   // chained only when apiVersion >= this
    std::uint32_t belowApiVersion; // ... and, when non-zero, apiVersion < this (promoted to core)
    std::size_t firstProvider;     // extension structures: providers[first, first + count) in priority order
    std::size_t providerCount;
    const Field* fields;
    std::size_t fieldCount;
};

template <typename T>
T capLoad(const void* p) {
    T v{};
    std::memcpy(&v, p, sizeof v);
    return v;
}

// Links each selected structure of `storage` into top's pNext chain (each at most once, in table order) and
// returns the row group per entry: "" when not chained.
template <typename Field>
std::vector<std::string> capBuildChain(void* top, void* storage, const CapChainEntry<Field>* entries,
                                       std::size_t count, const CapProvider* providers, std::uint32_t apiVersion,
                                       const CapHasExtension& hasExtension, const char* groupPrefix) {
    std::vector<std::string> groups(count);
    auto* tail = static_cast<VkBaseOutStructure*>(top);
    for (std::size_t i = 0; i < count; ++i) {
        const CapChainEntry<Field>& e = entries[i];
        if (apiVersion < e.minApiVersion || (e.belowApiVersion != 0 && apiVersion >= e.belowApiVersion)) continue;
        if (e.coreGroup != nullptr) {
            groups[i] = e.coreGroup;
        } else {
            for (std::size_t p = e.firstProvider; p < e.firstProvider + e.providerCount; ++p) {
                if (hasExtension(providers[p].extension) &&
                    (providers[p].depends == nullptr || providers[p].depends(apiVersion, hasExtension))) {
                    groups[i] = std::string(groupPrefix) + providers[p].extension;
                    break;
                }
            }
            if (groups[i].empty()) continue;
        }
        auto* s = reinterpret_cast<VkBaseOutStructure*>(static_cast<unsigned char*>(storage) + e.offset);
        s->sType = e.sType;
        s->pNext = nullptr;
        tail->pNext = s;
        tail = s;
    }
    return groups;
}

void capAddRows(std::vector<Capability>& out, const std::string& group, const void* s, const CapBoolField* fields,
                std::size_t count) {
    const auto* base = static_cast<const unsigned char*>(s);
    for (std::size_t i = 0; i < count; ++i) {
        const bool on = capLoad<VkBool32>(base + fields[i].offset) != VK_FALSE;
        out.push_back(Capability{group, fields[i].name, on ? "yes" : "no", on});
    }
}

void capAddRows(std::vector<Capability>& out, const std::string& group, const void* s, const CapField* fields,
                std::size_t count) {
    const auto* base = static_cast<const unsigned char*>(s);
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(Capability{group, fields[i].name, fields[i].format(base + fields[i].offset), true});
}

template <typename Field>
void capAddChained(std::vector<Capability>& out, const std::vector<std::string>& groups, const void* storage,
                   const CapChainEntry<Field>* entries) {
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].empty()) continue;
        capAddRows(out, groups[i], static_cast<const unsigned char*>(storage) + entries[i].offset, entries[i].fields,
                   entries[i].fieldCount);
    }
}

std::string capFlagsString(unsigned long long v, const CapFlagBit* bits, std::size_t count) {
    if (v == 0) return "none";
    std::string s;
    unsigned long long rest = v;
    for (std::size_t i = 0; i < count; ++i) {
        if ((v & bits[i].bit) == 0) continue;
        if (!s.empty()) s += " | ";
        s += bits[i].name;
        rest &= ~bits[i].bit;
    }
    if (rest != 0) {
        if (!s.empty()) s += " | ";
        s += std::format("0x{:x}", rest);
    }
    return s;
}

template <typename T>
std::string capNumString(T v) {
    if constexpr (std::is_floating_point_v<T>)
        return std::format("{:g}", v);
    else
        return std::format("{}", v);
}

std::string capFmtBool(const void* p) { return capLoad<VkBool32>(p) != VK_FALSE ? "yes" : "no"; }

template <typename T>
std::string capFmtNum(const void* p) {
    return capNumString(capLoad<T>(p));
}

template <typename T, std::size_t N>
std::string capFmtNumArray(const void* p) {
    std::string s;
    for (std::size_t i = 0; i < N; ++i) {
        if (i != 0) s += ", ";
        s += capNumString(capLoad<T>(static_cast<const unsigned char*>(p) + i * sizeof(T)));
    }
    return s;
}

template <std::size_t N>
std::string capFmtString(const void* p) {
    const auto* a = static_cast<const char*>(p);
    std::size_t n = 0;
    while (n < N && a[n] != '\0') ++n;
    return std::string(a, n);
}

template <std::size_t N>
std::string capFmtHex(const void* p) {
    const auto* a = static_cast<const unsigned char*>(p);
    std::string s;
    for (std::size_t i = 0; i < N; ++i) s += std::format("{:02x}", a[i]);
    return s;
}
""".strip("\n").split("\n")
        if "rawFlags" in self.used:
            L += ["", "template <typename T>", "std::string capFmtRawFlags(const void* p) {",
                  "    return capFlagsString(capLoad<T>(p), nullptr, 0);", "}"]
        if "apiVersion" in self.used:
            L += ["", "std::string capFmtApiVersion(const void* p) {",
                  "    const auto v = capLoad<std::uint32_t>(p);",
                  "    return std::format(\"{}.{}.{}\", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), "
                  "VK_API_VERSION_PATCH(v));", "}"]
        if "VkExtent2D" in self.used:
            L += ["", "std::string capFmtExtent2D(const void* p) {",
                  "    const auto e = capLoad<VkExtent2D>(p);",
                  "    return std::format(\"{}x{}\", e.width, e.height);", "}"]
        if "VkExtent3D" in self.used:
            L += ["", "std::string capFmtExtent3D(const void* p) {",
                  "    const auto e = capLoad<VkExtent3D>(p);",
                  "    return std::format(\"{}x{}x{}\", e.width, e.height, e.depth);", "}"]
        if "VkConformanceVersion" in self.used:
            L += ["", "std::string capFmtConformanceVersion(const void* p) {",
                  "    const auto c = capLoad<VkConformanceVersion>(p);",
                  "    return std::format(\"{}.{}.{}.{}\", c.major, c.minor, c.subminor, c.patch);", "}"]
        return L

    def generate(self) -> str:
        reg = self.reg
        feature_tables: list[str] = []
        property_tables: list[str] = []
        features = self.gen_collect("Features", feature_tables)
        properties = self.gen_collect("Properties", property_tables)
        enums: list[str] = []
        for t in self.used_enums:
            enums += [""] + self.gen_enum(t)
        flags = self.gen_flags()

        major, minor = reg.header_version_complete or (1, 0)
        version = f"{major}.{minor}.{reg.header_version}"
        fa, fe, fp = self.stats["Features"]
        pa, pe, pp = self.stats["Properties"]
        out: list[str] = [
            "// SPDX-License-Identifier: MIT",
            "// GENERATED FILE - DO NOT EDIT.",
            f"// Generated by scripts/gen_vk_caps.py from the Vulkan registry (vk.xml) of Vulkan-Headers {version}",
            f"// (VK_HEADER_VERSION {reg.header_version}). Regenerate with:",
            "//   python3 scripts/gen_vk_caps.py $GT_DEPS_CACHE/vulkan_headers/registry/vk.xml "
            "src/rhi/vulkan/vk_caps_gen.inc",
            "//",
            "// Include at global scope from one .cpp, after: #define VK_ENABLE_BETA_EXTENSIONS 1, <volk.h>, <string>,",
            "// <vector>, <functional>, <format>, <cstring> and \"gt/rhi/types.h\".",
            "//",
            f"// Features:   {fa} VulkanNN aggregates + {fe} extension structures ({fp} providing extensions).",
            f"// Properties: {pa} VulkanNN aggregates + {pe} extension structures ({pp} providing extensions).",
            "// Structures promoted to core are chained under their extension only while apiVersion is below the",
            "// VulkanNN aggregate that covers them. Pointer members (e.g. pCopySrcLayouts) stay null: counts only.",
            "// clang-format off",
            "",
            "#include <cstddef>",
            "#include <type_traits>",
            "",
            f"static_assert(VK_HEADER_VERSION >= {reg.header_version}, "
            f"\"vk_caps_gen.inc needs Vulkan headers >= {version}; regenerate it\");",
            "",
            "namespace gt::rhi::vk::gen {",
            "",
            "void collectFeatures(VkPhysicalDevice pd, std::uint32_t apiVersion,",
            "                     const std::function<bool(const char*)>& hasExtension, std::vector<Capability>& out);",
            "void collectProperties(VkPhysicalDevice pd, std::uint32_t apiVersion,",
            "                       const std::function<bool(const char*)>& hasExtension, "
            "std::vector<Capability>& out);",
            "",
            "namespace {",
            "",
        ]
        out += self.helpers()
        out += self.gen_depends()
        out += enums
        out += flags
        out += ["", "// --- feature tables (VkBool32 members) " + "-" * 70]
        out += feature_tables
        out += ["", "// --- property tables " + "-" * 87]
        out += property_tables
        out += ["", "} // namespace", ""]
        out += features
        out += [""]
        out += properties
        out += ["", "} // namespace gt::rhi::vk::gen", ""]
        return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <vk.xml> <out.inc>", file=sys.stderr)
        return 2
    reg = Registry(ET.parse(argv[1]).getroot())
    if reg.header_version is None:
        print("error: VK_HEADER_VERSION not found in the registry", file=sys.stderr)
        return 1
    gen = Generator(reg)
    text = gen.generate()
    with open(argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    for w in reg.warnings:
        print(f"note: {w}", file=sys.stderr)
    if gen.skipped_types:
        skipped = ", ".join(f"{t} x{n}" for t, n in sorted(gen.skipped_types.items()))
        print(f"note: skipped members of types without a formatter: {skipped}", file=sys.stderr)
    fa, fe, _ = gen.stats["Features"]
    pa, pe, _ = gen.stats["Properties"]
    print(f"wrote {argv[2]}: features {fa} aggregates + {fe} extension structs, "
          f"properties {pa} aggregates + {pe} extension structs", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
