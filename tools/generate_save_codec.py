#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate explicit ILP32 save layouts from the reviewed id Software structs.
The generated header is checked in; normal builds require no generation step.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = root / "src/doom"
types = ["thinker_t", "pspdef_t", "player_t", "mobj_t", "ceiling_t", "vldoor_t",
         "floormove_t", "plat_t", "lightflash_t", "strobe_t", "glow_t"]
headers = ["d_think.h", "p_pspr.h", "d_player.h", "p_mobj.h", "p_spec.h"]
structs = {}
for header in headers:
    text = (source / header).read_text()
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    for body, name in re.findall(r"typedef\s+struct\s*(?:\w+)?\s*\{([^{}]*)\}\s*(\w+)\s*;", text):
        fields = []
        for declaration in body.split(";"):
            declaration = " ".join(declaration.split())
            if not declaration:
                continue
            match = re.fullmatch(r"(.*?)\s*\b(\w+)(\[[^\]]+\])?", declaration)
            assert match, (name, declaration)
            fields.append(match.groups())
        structs[name] = fields

sizes = {"ticcmd_t": (8, 2), "mapthing_t": (10, 2)}
counts = dict(NUMPOWERS=6, NUMCARDS=6, MAXPLAYERS=4, NUMWEAPONS=9, NUMAMMO=4, NUMPSPRITES=2)
out = ["// SPDX-License-Identifier: GPL-2.0-only",
       "// Generated from official DOOM struct field order by tools/generate_save_codec.py.",
       "// Modified 2026-09-30: byte-compatible 32-bit disk layouts without native addresses.",
       "#pragma pack(push, 4)"]
for name in types:
    out += ["typedef struct {"]
    offset, alignment = 0, 1
    for typ, field, arr in structs[name]:
        pointer = "*" in typ or typ == "think_t"
        disk_type = "uint32_t" if pointer else ("disk_" + typ if typ in types else typ)
        size, align = (4, 4) if pointer else sizes.get(typ, (4, 4))
        offset = (offset + align - 1) // align * align
        offset += size * (counts[arr[1:-1]] if arr else 1)
        alignment = max(alignment, align)
        out += [f"    {disk_type} {field}{arr or ''};"]
    size = (offset + alignment - 1) // alignment * alignment
    sizes[name] = (size, alignment)
    out += [f"}} disk_{name};", f'_Static_assert(sizeof(disk_{name}) == {size}, "{name} original ILP32 layout");']
out += ["#pragma pack(pop)", '#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__',
        '#error "This save codec targets little-endian arm64 macOS"', '#endif']
for name in types:
    out += [f"static disk_{name} Encode_{name}(const {name} *n) {{", f"    disk_{name} d = {{0}};"]
    for typ, field, arr in structs[name]:
        if typ == "think_t":
            out += [f"    d.{field} = n->{field}.acp1 != NULL;"]
        elif "*" in typ:
            value = f"(uint32_t)(uintptr_t)n->{field}" if field in ("state", "player", "sector") else "0"
            out += [f"    d.{field} = {value};"]
        elif arr:
            if typ in types:
                out += [f"    for (int i=0; i<{arr[1:-1]}; ++i) d.{field}[i] = Encode_{typ}(&n->{field}[i]);"]
            else:
                out += [f"    memcpy(d.{field}, n->{field}, sizeof(d.{field}));"]
        elif typ in types:
            out += [f"    d.{field} = Encode_{typ}(&n->{field});"]
        else:
            out += [f"    d.{field} = n->{field};"]
    out += ["    return d;", "}", f"static void Decode_{name}({name} *n, const disk_{name} *d) {{", "    memset(n, 0, sizeof(*n));"]
    for typ, field, arr in structs[name]:
        if typ == "think_t":
            out += [f"    n->{field}.acp1 = d->{field} ? (actionf_p1)(uintptr_t)1 : NULL;"]
        elif "*" in typ:
            out += [f"    n->{field} = ({typ})(uintptr_t)d->{field};"]
        elif arr:
            if typ in types:
                out += [f"    for (int i=0; i<{arr[1:-1]}; ++i) Decode_{typ}(&n->{field}[i], &d->{field}[i]);"]
            else:
                out += [f"    memcpy(n->{field}, d->{field}, sizeof(d->{field}));"]
        elif typ in types:
            out += [f"    Decode_{typ}(&n->{field}, &d->{field});"]
        else:
            out += [f"    n->{field} = d->{field};"]
    out += ["}", f"static void SaveWrite_{name}(const {name} *n) {{", f"    disk_{name} d = Encode_{name}(n);",
            "    P_SaveCheck(sizeof(d)); memcpy(save_p, &d, sizeof(d)); save_p += sizeof(d);", "}",
            f"static void SaveRead_{name}({name} *n) {{", f"    disk_{name} d;",
            "    P_SaveCheck(sizeof(d)); memcpy(&d, save_p, sizeof(d)); save_p += sizeof(d);",
            f"    Decode_{name}(n, &d);", "}"]
(source / "p_savecodec.h").write_text("\n".join(out) + "\n")
print({name: sizes[name][0] for name in types})
