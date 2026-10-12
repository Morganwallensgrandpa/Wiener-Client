#!/usr/bin/env python3
"""Look up Fabric intermediary names from Yarn (named) names.

The client talks to the game through JNI, so it must use the *intermediary* names
the production jar actually contains (net/minecraft/class_310, method_1551, ...).
Reasoning about those from memory is how a module ends up silently doing nothing,
so this resolves them from the authoritative mapping file instead.

Input is the tiny v2 file inside a Yarn "mergedv2" jar, which carries three
namespaces per entry: official, intermediary, named (Yarn). Descriptors in that
file are written in the official namespace, so they are remapped here into
intermediary before being handed out -- that is exactly what JNI needs.

CLI:
    python tools/yarn_map.py <mappings.tiny> Class#member [Class#member ...]
    python tools/yarn_map.py <mappings.tiny> --class net/minecraft/client/MinecraftClient
"""

import re
import sys
from pathlib import Path

# --- tiny v2 ---------------------------------------------------------------

class Member:
    __slots__ = ("kind", "official", "intermediary", "named", "official_desc")

    def __init__(self, kind, official, intermediary, named, official_desc):
        self.kind = kind
        self.official = official
        self.intermediary = intermediary
        self.named = named
        self.official_desc = official_desc


class Mapping:
    def __init__(self, text: str):
        self.official_to_inter: dict[str, str] = {}
        self.named_to_inter: dict[str, str] = {}
        self.inter_to_named: dict[str, str] = {}
        self.members: dict[str, list[Member]] = {}

        cur_named = None
        for line in text.splitlines():
            if line.startswith("tiny\t"):
                continue
            if line.startswith("c\t"):
                _, official, intermediary, named = (line.split("\t") + ["", ""])[:4]
                self.official_to_inter[official] = intermediary or official
                self.named_to_inter[named or official] = intermediary or official
                self.inter_to_named[intermediary or official] = named or official
                cur_named = named or official
                self.members.setdefault(cur_named, [])
                continue
            if line.startswith("\tf\t") or line.startswith("\tm\t"):
                parts = line.split("\t")
                kind = parts[1]
                desc, official, intermediary, named = (parts[2:6] + ["", ""])[:4]
                self.members.setdefault(cur_named, []).append(
                    Member(kind, official, intermediary or official, named or official, desc))
                continue
            # parameter entries and anything else are not needed here
        self._desc_cache: dict[str, str] = {}

    def named_of_member(self, inter_cls: str, inter_member: str) -> str | None:
        """intermediary class + member -> Yarn name (for bytecode -> name lookup)."""
        named_cls = self.inter_to_named.get(inter_cls)
        if not named_cls:
            return None
        for m in self.members.get(named_cls, []):
            if m.intermediary == inter_member:
                return m.named
        return None

    def remap_desc(self, desc: str) -> str:
        """official descriptor -> intermediary descriptor."""
        if desc in self._desc_cache:
            return self._desc_cache[desc]

        def sub(m: re.Match) -> str:
            cls = m.group(1)
            return "L" + self.official_to_inter.get(cls, cls) + ";"

        out = re.sub(r"L([^;]+);", sub, desc)
        self._desc_cache[desc] = out
        return out

    def find_class(self, named: str) -> str | None:
        """named class (slashed) -> intermediary class."""
        return self.named_to_inter.get(named)

    def find(self, named_cls: str, named_member: str, kind: str | None = None):
        """named class+member -> list of Member (with descriptors remapped)."""
        out = []
        for m in self.members.get(named_cls, []):
            if kind and m.kind != kind:
                continue
            if m.named == named_member or m.official == named_member:
                out.append(m)
        return out

    def describe(self, named_cls: str, named_member: str) -> list[str]:
        lines = []
        for m in self.find(named_cls, named_member):
            lines.append(f"{named_cls.split('/')[-1]}#{named_member} -> "
                         f"{m.intermediary} {self.remap_desc(m.official_desc)} "
                         f"({'method' if m.kind == 'm' else 'field'})")
        return lines


def load(path: str) -> Mapping:
    return Mapping(Path(path).read_text(encoding="utf-8", errors="replace"))


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    m = load(sys.argv[1])

    if sys.argv[2] == "--class":
        named = sys.argv[3]
        inter = m.find_class(named)
        print(f"{named} -> {inter}")
        for mem in sorted(m.members.get(named, []), key=lambda x: x.named):
            print(f"  {'m' if mem.kind == 'm' else 'f'} {mem.named} "
                  f"-> {mem.intermediary} {m.remap_desc(mem.official_desc)}")
        return 0

    bad = 0
    for spec in sys.argv[2:]:
        if "#" in spec:
            cls, member = spec.split("#", 1)
        else:
            cls, member = spec, None
        cls = cls.replace(".", "/")
        if cls.startswith("inter:"):
            inter_cls = cls[len("inter:"):]
            if "/" not in inter_cls:
                inter_cls = "net/minecraft/" + inter_cls
            named = m.named_of_member(inter_cls, member)
            print(f"{inter_cls}#{member} -> {named or 'NOT FOUND'}")
            continue
        if not cls.startswith("net/minecraft/") and "/" not in cls:
            cls = "net/minecraft/" + cls
        if cls.startswith("net/minecraft/") and cls.count("/") == 1:
            # short form like net/minecraft/Entity -> not resolvable; require full
            pass
        if member is None:
            print(f"{cls} -> {m.find_class(cls)}")
            continue
        lines = m.describe(cls, member)
        if not lines:
            print(f"NOT FOUND: {cls}#{member}")
            bad += 1
        for line in lines:
            print(line)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
