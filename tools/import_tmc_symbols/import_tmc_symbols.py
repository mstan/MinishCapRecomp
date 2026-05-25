#!/usr/bin/env python3
"""import_tmc_symbols.py — read zeldaret/tmc and emit symbol TSVs.

The recompiler needs (name, address, mode) tuples to seed its
analysis. zeldaret/tmc maintains this data in two parseable
forms:

  1. asm/**/*.s — for not-yet-decompiled functions. Each
     function starts with a `thumb_func_start NAME`,
     `arm_func_start NAME`, or
     `non_word_aligned_thumb_func_start NAME` directive,
     followed by `NAME: @ 0xADDR` as the label line.

  2. linker.ld — for data symbols at fixed addresses
     (EWRAM/IWRAM globals). Lines look like
     `. = 0xADDR; NAME = .;` inside MEMORY-typed SECTIONS.

Decompiled functions in `src/**/*.c` have NO embedded address —
their final placement is determined by the linker at build time
and only appears in the `.map`. We don't build tmc here, so
those functions are left for the recompiler to discover via
`dispatch_misses.log` iteration.

Outputs (relative to this repo's root):

  symbols/imported_symbols.tsv          functions: addr  mode  name
  symbols/function_boundaries.tsv       functions: start  end  mode  name
  symbols/imported_data_symbols.tsv     data:      addr  region  name
  ghidra/import_symbols.py              Ghidra Jython script that
                                         applies all symbols to the
                                         open program.

Usage:
  python tools/import_tmc_symbols/import_tmc_symbols.py
      [--tmc third_party/tmc]   Path to the cloned tmc repo.
      [--out symbols]           Output dir for TSVs.
      [--ghidra ghidra]         Output dir for Ghidra script.

The decomp's C source is never read. Only symbol metadata.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Iterable


ROOT = pathlib.Path(__file__).resolve().parents[2]


@dataclass
class Func:
    addr: int
    mode: str          # "arm" or "thumb"
    name: str
    source_file: str   # asm/.../foo.s
    end: int = 0       # inferred from next function in same file; 0=unknown


@dataclass
class DataSym:
    addr: int
    name: str
    region: str        # "ewram" / "iwram" / "rom" / "io" / "unknown"


# ─────────────────────────────────────────────────────────────────────
# asm parsing
# ─────────────────────────────────────────────────────────────────────

# Matches the three directive flavors.
FUNC_DIRECTIVE = re.compile(
    r"^\s*(arm_func_start|thumb_func_start|non_word_aligned_thumb_func_start)"
    r"\s+([A-Za-z_][A-Za-z0-9_]*)\s*$"
)
# Matches `NAME: @ 0xADDR` on the label line.
LABEL_LINE = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*):\s*@\s*0x([0-9A-Fa-f]+)\s*$"
)


def parse_asm_file(path: pathlib.Path) -> list[Func]:
    out: list[Func] = []
    pending_directive: tuple[str, str] | None = None
    rel = str(path).replace("\\", "/")
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"  warn: cannot read {path}: {e}", file=sys.stderr)
        return out

    for line in text.splitlines():
        m = FUNC_DIRECTIVE.match(line)
        if m:
            directive, name = m.group(1), m.group(2)
            mode = "arm" if directive == "arm_func_start" else "thumb"
            pending_directive = (mode, name)
            continue
        if pending_directive is None:
            continue
        if not line.strip():
            continue
        label = LABEL_LINE.match(line)
        if label is None:
            # First non-blank line after the directive isn't an
            # annotated label — likely a `.byte`-style guard or an
            # unlabelled body. Skip this directive.
            pending_directive = None
            continue
        label_name, addr_hex = label.group(1), int(label.group(2), 16)
        directive_mode, directive_name = pending_directive
        if label_name != directive_name:
            # Mismatch between directive name and label name —
            # unusual; prefer the label name.
            directive_name = label_name
        out.append(Func(
            addr=addr_hex,
            mode=directive_mode,
            name=directive_name,
            source_file=rel,
        ))
        pending_directive = None
    return out


def infer_boundaries(funcs: list[Func]) -> None:
    """Within each source file, set each function's end = next function's
    start - 1. The last function in a file keeps end = 0 (unknown)."""
    by_file: dict[str, list[Func]] = defaultdict(list)
    for f in funcs:
        by_file[f.source_file].append(f)
    for file_funcs in by_file.values():
        file_funcs.sort(key=lambda f: f.addr)
        for i in range(len(file_funcs) - 1):
            file_funcs[i].end = file_funcs[i + 1].addr - 1


# ─────────────────────────────────────────────────────────────────────
NM_TEXT_SYM = re.compile(
    r"^([0-9A-Fa-f]+)\s+([Tt])\s+([A-Za-z_][A-Za-z0-9_]*)\s*$"
)
OBJDUMP_TEXT_SECTION = re.compile(
    r"^\s*\d+\s+\.text\s+([0-9A-Fa-f]+)\s+"
)
LINKER_TEXT_OBJECT = re.compile(
    r"([A-Za-z0-9_./-]+\.o)\s*\(\.text\)"
)
ADDR_ANCHOR_NAME = re.compile(r"^sub_([0-9A-Fa-f]{8})$")
DEFINED_EXPR = re.compile(r"defined\(([^)]+)\)")


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def align_down(value: int, alignment: int) -> int:
    return value & ~(alignment - 1)


def eval_linker_condition(expr: str) -> bool:
    """Evaluate the small subset of linker.ld #if syntax for USA."""
    defines = {"USA"}

    def repl(match: re.Match[str]) -> str:
        return "True" if match.group(1) in defines else "False"

    py_expr = DEFINED_EXPR.sub(repl, expr)
    py_expr = py_expr.replace("||", " or ")
    py_expr = py_expr.replace("&&", " and ")
    py_expr = re.sub(r"!\s*", " not ", py_expr)
    try:
        return bool(eval(py_expr, {"__builtins__": {}}, {}))
    except Exception:
        return False


def active_linker_lines(text: str) -> Iterable[str]:
    """Yield lines active for the USA non-demo build."""
    frames: list[tuple[bool, bool]] = []
    active = True

    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#ifdef "):
            name = stripped.split(None, 1)[1]
            cond = name == "USA"
            frames.append((active, cond))
            active = active and cond
            continue
        if stripped.startswith("#if "):
            cond = eval_linker_condition(stripped[4:].strip())
            frames.append((active, cond))
            active = active and cond
            continue
        if stripped.startswith("#else"):
            if frames:
                parent_active, cond = frames[-1]
                active = parent_active and not cond
                frames[-1] = (parent_active, not cond)
            continue
        if stripped.startswith("#endif"):
            if frames:
                parent_active, _cond = frames.pop()
                active = parent_active
            continue
        if active:
            yield line


def asm_object_rel(source_file: str) -> str | None:
    source = source_file.replace("\\", "/")
    idx = source.find("asm/")
    if idx < 0 or not source.endswith(".s"):
        return None
    return source[idx:-2] + ".o"


def run_nm_text_symbols(obj: pathlib.Path,
                        nm_tool: str) -> list[tuple[int, str, str]]:
    try:
        proc = subprocess.run(
            [nm_tool, "-n", str(obj)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"  warn: cannot nm {obj}: {exc}", file=sys.stderr)
        return []

    syms: list[tuple[int, str, str]] = []
    for line in proc.stdout.splitlines():
        m = NM_TEXT_SYM.match(line)
        if m:
            syms.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return syms


def read_text_size(obj: pathlib.Path,
                   objdump_tool: str = "arm-none-eabi-objdump") -> int | None:
    try:
        proc = subprocess.run(
            [objdump_tool, "-h", str(obj)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"  warn: cannot objdump {obj}: {exc}", file=sys.stderr)
        return None

    for line in proc.stdout.splitlines():
        m = OBJDUMP_TEXT_SECTION.match(line)
        if m:
            return int(m.group(1), 16)
    return None


def parse_linker_text_order(linker_ld: pathlib.Path) -> list[str]:
    try:
        text = linker_ld.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"  warn: cannot read {linker_ld}: {exc}", file=sys.stderr)
        return []

    out: list[str] = []
    for raw_line in active_linker_lines(text):
        line = raw_line.split("/*", 1)[0]
        m = LINKER_TEXT_OBJECT.search(line)
        if not m:
            continue
        obj = m.group(1)
        if obj.startswith("*"):
            continue
        out.append(obj)
    return out


def infer_object_bases(tmc_root: pathlib.Path,
                       build_dir: pathlib.Path,
                       asm_funcs: Iterable[Func],
                       nm_tool: str = "arm-none-eabi-nm",
                       objdump_tool: str = "arm-none-eabi-objdump"
                       ) -> dict[str, int]:
    """Infer linked ROM bases for built objects.

    Decompiled C objects often have no address-bearing symbol names.
    For those, use linker.ld order plus built object .text sizes.
    A direct base is trusted when either a text symbol is named
    sub_08xxxxxx or an ASM object symbol matches an annotated ASM label.
    """
    linker_order = parse_linker_text_order(tmc_root / "linker.ld")
    if not linker_order:
        return {}

    asm_label_addrs: dict[str, dict[str, int]] = defaultdict(dict)
    for func in asm_funcs:
        obj_rel = asm_object_rel(func.source_file)
        if obj_rel is not None:
            asm_label_addrs[obj_rel][func.name] = func.addr

    obj_infos: dict[str, dict[str, object]] = {}
    for obj in build_dir.rglob("*.o"):
        try:
            obj_rel = str(obj.relative_to(build_dir)).replace("\\", "/")
        except ValueError:
            continue
        size = read_text_size(obj, objdump_tool)
        syms = run_nm_text_symbols(obj, nm_tool)
        obj_infos[obj_rel] = {
            "path": obj,
            "size": size,
            "syms": syms,
        }

    direct_bases: dict[str, int] = {}
    for obj_rel, info in obj_infos.items():
        syms = info["syms"]
        assert isinstance(syms, list)
        bases: set[int] = set()
        for offset, _sym_type, name in syms:
            anchor = ADDR_ANCHOR_NAME.match(name)
            if anchor:
                bases.add(int(anchor.group(1), 16) - offset)
            asm_addr = asm_label_addrs.get(obj_rel, {}).get(name)
            if asm_addr is not None:
                bases.add(asm_addr - offset)
        if len(bases) == 1:
            direct_bases[obj_rel] = next(iter(bases))
        elif len(bases) > 1:
            print(f"  warn: inconsistent object bases in {obj_rel}",
                  file=sys.stderr)

    inferred: dict[str, int] = {}
    current_base: int | None = None
    for obj_rel in linker_order:
        info = obj_infos.get(obj_rel)
        if info is None:
            current_base = None
            continue

        size_obj = info.get("size")
        size = size_obj if isinstance(size_obj, int) else None
        base = direct_bases.get(obj_rel)
        if base is None and current_base is not None:
            base = align(current_base, 4)
        if base is not None:
            inferred[obj_rel] = base
            if size is not None:
                current_base = base + size
            else:
                current_base = None
        else:
            current_base = None

    current_base = None
    for obj_rel in reversed(linker_order):
        info = obj_infos.get(obj_rel)
        if info is None:
            current_base = None
            continue

        size_obj = info.get("size")
        size = size_obj if isinstance(size_obj, int) else None
        if obj_rel in inferred:
            current_base = inferred[obj_rel]
            continue

        if current_base is not None and size is not None:
            base = align_down(current_base - size, 4)
            inferred[obj_rel] = base
            current_base = base
        else:
            current_base = None

    return inferred


def parse_built_c_objects(tmc_root: pathlib.Path,
                          build_dir: pathlib.Path,
                          asm_funcs: Iterable[Func],
                          nm_tool: str = "arm-none-eabi-nm") -> list[Func]:
    """Read built C object files and infer linked ROM addresses."""
    src_root = build_dir / "src"
    if not src_root.exists():
        return []

    object_bases = infer_object_bases(tmc_root, build_dir, asm_funcs,
                                      nm_tool)
    out: list[Func] = []
    for obj in sorted(src_root.rglob("*.o")):
        syms = run_nm_text_symbols(obj, nm_tool)

        bases: set[int] = set()
        for offset, _sym_type, name in syms:
            anchor = ADDR_ANCHOR_NAME.match(name)
            if anchor:
                bases.add(int(anchor.group(1), 16) - offset)
        try:
            obj_rel = str(obj.relative_to(build_dir)).replace("\\", "/")
        except ValueError:
            obj_rel = ""
        if obj_rel in object_bases:
            bases.add(object_bases[obj_rel])

        if not bases:
            continue
        if len(bases) != 1:
            print(f"  warn: inconsistent C object anchors in {obj}",
                  file=sys.stderr)
            continue
        base = next(iter(bases))

        try:
            rel = str(obj.relative_to(tmc_root)).replace("\\", "/")
        except ValueError:
            rel = str(obj).replace("\\", "/")
        obj_prefix = "_".join(obj.relative_to(src_root).with_suffix("").parts)

        for offset, sym_type, raw_name in syms:
            name = raw_name
            if sym_type == "t" and not ADDR_ANCHOR_NAME.match(raw_name):
                name = f"{obj_prefix}_{raw_name}"
            out.append(Func(
                addr=base + offset,
                mode="thumb",
                name=name,
                source_file=rel,
            ))
    return out


def dedupe_funcs(funcs: Iterable[Func]) -> list[Func]:
    out: list[Func] = []
    seen: set[tuple[int, str]] = set()
    for f in funcs:
        key = (f.addr, f.mode)
        if key in seen:
            continue
        seen.add(key)
        out.append(f)
    return out


# linker.ld parsing
# ─────────────────────────────────────────────────────────────────────

# Inside MEMORY sections of linker.ld:
#   . = 0x00000010; gUnk_02000010 = .;
# We don't track the current SECTION; the address bits tell us the
# region.
LINKER_SYM = re.compile(
    r"^\s*\.\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\.\s*;"
)
# Top-level "ABSOLUTE" symbol definitions (some appear outside
# sections): e.g., `gNumMusicPlayers = 0x20;`
ABS_SYM = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*$"
)


def region_for(addr: int) -> str:
    if 0x00000000 <= addr <= 0x00003FFF:
        return "bios"
    if 0x02000000 <= addr <= 0x0203FFFF:
        return "ewram"
    if 0x03000000 <= addr <= 0x03007FFF:
        return "iwram"
    if 0x04000000 <= addr <= 0x040003FF:
        return "io"
    if 0x05000000 <= addr <= 0x050003FF:
        return "pal"
    if 0x06000000 <= addr <= 0x06017FFF:
        return "vram"
    if 0x07000000 <= addr <= 0x070003FF:
        return "oam"
    if 0x08000000 <= addr <= 0x09FFFFFF:
        return "rom"
    if 0x0A000000 <= addr <= 0x0BFFFFFF:
        return "rom_mirror"
    if 0x0E000000 <= addr <= 0x0E00FFFF:
        return "sram"
    return "unknown"


def parse_linker_ld(path: pathlib.Path) -> list[DataSym]:
    out: list[DataSym] = []
    # Linker fragments use both forms. The MEMORY-section style is
    # contextualized (the offset is added to the section's ORIGIN).
    # For simplicity we treat the offset as an absolute address when
    # it already looks like one (>= 0x02000000); otherwise we
    # combine with the section the line is in. tmc's MEMORY layout
    # has clearly distinguishable EWRAM/IWRAM ORIGINs, but the
    # offsets inside sections are within-section, so we infer the
    # ORIGIN from the most recent section header.
    current_origin = 0
    in_section = False
    section_re = re.compile(
        r"^\s*(ewram|iwram|rom)\s*(?:\(\s*NOLOAD\s*\))?\s*:"
    )

    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"  warn: cannot read {path}: {e}", file=sys.stderr)
        return out

    origins = {
        "ewram": 0x02000000,
        "iwram": 0x03000000,
        "rom":   0x08000000,
    }
    for line in active_linker_lines(text):
        sm = section_re.match(line)
        if sm:
            current_origin = origins.get(sm.group(1), 0)
            in_section = True
            continue
        if in_section:
            m = LINKER_SYM.match(line)
            if m:
                offset = int(m.group(1), 16)
                name = m.group(2)
                addr = current_origin + offset
                out.append(DataSym(addr=addr, name=name,
                                    region=region_for(addr)))
                continue
            if "}" in line:
                in_section = False
                current_origin = 0
                continue
        # Outside-section absolute definitions (e.g. gNumMusicPlayers
        # = 0x20). These are constants, not memory addresses, so we
        # skip them — they aren't relevant to the recompiler.
    return out


# ─────────────────────────────────────────────────────────────────────
# writers
# ─────────────────────────────────────────────────────────────────────

def write_imported_symbols(path: pathlib.Path, funcs: Iterable[Func]) -> int:
    rows = sorted(funcs, key=lambda f: (f.addr, f.name))
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Tab-separated: addr\tmode\tname\n")
        fh.write("# Generated by tools/import_tmc_symbols/import_tmc_symbols.py\n")
        fh.write("# Source: zeldaret/tmc asm/**/*.s (thumb_func_start /\n")
        fh.write("#         arm_func_start / non_word_aligned_thumb_func_start)\n")
        for f in rows:
            fh.write(f"0x{f.addr:08X}\t{f.mode}\t{f.name}\n")
    return len(rows)


def write_function_boundaries(path: pathlib.Path,
                              funcs: Iterable[Func]) -> int:
    rows = sorted(funcs, key=lambda f: (f.addr, f.name))
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Tab-separated: start\tend\tmode\tname\n")
        fh.write("# Generated by tools/import_tmc_symbols/import_tmc_symbols.py\n")
        fh.write("# end=0 means \"unknown — last function in source file\".\n")
        for f in rows:
            fh.write(
                f"0x{f.addr:08X}\t0x{f.end:08X}\t{f.mode}\t{f.name}\n"
            )
    return len(rows)


def write_data_symbols(path: pathlib.Path,
                        syms: Iterable[DataSym]) -> int:
    rows = sorted(syms, key=lambda s: (s.addr, s.name))
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Tab-separated: addr\tregion\tname\n")
        fh.write("# Generated by tools/import_tmc_symbols/import_tmc_symbols.py\n")
        fh.write("# Source: zeldaret/tmc linker.ld inside-MEMORY symbol lines.\n")
        for s in rows:
            fh.write(f"0x{s.addr:08X}\t{s.region}\t{s.name}\n")
    return len(rows)


def write_recompiler_toml(path: pathlib.Path,
                          funcs: Iterable[Func],
                          rom_path: pathlib.Path,
                          data_syms: Iterable[DataSym]) -> int:
    if not rom_path.exists():
        print(f"  warn: ROM not found at {rom_path}; skipping TOML",
              file=sys.stderr)
        return 0

    rom = rom_path.read_bytes()
    sha1 = hashlib.sha1(rom).hexdigest()
    rows = sorted(funcs, key=lambda f: (f.addr, f.name))

    code_copies = [
        (0x0300404C, 0x080AF3A4, 0x00000380, "sound_main_ram",
         "M4A SoundMainRAM copied to IWRAM"),
        (0x030056F0, 0x080B197C, 0x00001280, "iwram_funcs",
         "tmc RAMFUNCS_BASE copied to IWRAM"),
    ]

    manual_entries = [
        (0x08000000, "arm", "rom_header_start_vector",
         "GBA ROM header branch to crt0"),
        (0x080000C0, "arm", "crt0_start",
         "ROM header branch target"),
        (0x03005D90, "arm", "ram_IntrMain",
         "IWRAM copy; source bytes at 0x080B201C"),
        (0x0300404C, "thumb", "ram_SoundMainRAM",
         "IWRAM copy; source bytes at 0x080AF3A4"),
        (0x080B14C6, "thumb", "BgAffineSet_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14CA, "thumb", "CpuSet_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14CE, "thumb", "Div_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14D2, "thumb", "Mod_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14DA, "thumb", "LZ77UnCompVram_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14DE, "thumb", "LZ77UnCompWram_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14E2, "thumb", "ObjAffineSet_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14E6, "thumb", "RegisterRamReset_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14EC, "thumb", "SoundBiasReset_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14F4, "thumb", "SoundBiasSet_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B14FA, "thumb", "Sqrt_swi_cont",
         "libagbsyscall SWI continuation"),
        (0x080B1500, "thumb", "VBlankIntrWait_swi_cont",
         "libagbsyscall SWI continuation"),
    ]
    manual_jump_tables = [
        (0x08100CBC, 4, 6, "abs32", "auto", "game task dispatch table"),
        (0x080FC8A4, 4, 11, "abs32", "auto", "file-select state table"),
        (0x080FC8FC, 4, 6, "abs32", "auto", "file-select menu callback table"),
        (0x080FC93C, 4, 13, "abs32", "auto", "file-select transition callback table"),
        (0x080FC9B0, 4, 20, "abs32", "auto", "file-select widget callback table"),
        (0x080FCA04, 4, 7, "abs32", "auto", "post-title task callback table"),
        (0x080FCA70, 4, 5, "abs32", "auto", "game-over callback table"),
        (0x080FCB18, 4, 8, "abs32", "auto", "staff-roll callback table"),
        (0x080FCBB4, 4, 4, "abs32", "auto", "debug callback table"),
        (0x08050C88, 4, 32, "abs32", "thumb", "slot-start cursor dispatch table"),
        (0x08051258, 4, 8, "abs32", "thumb", "name-entry action dispatch table"),
        (0x0805252C, 4, 11, "abs32", "thumb", "InitializePlayer spawn-state dispatch table"),
        (0x08052AB4, 4, 9, "abs32", "thumb", "InitRoomTransition type dispatch table"),
        (0x0805F00C, 4, 16, "abs32", "thumb", "GetCharacter top-level dispatch table"),
        (0x0805F0AC, 4, 22, "abs32", "thumb", "GetCharacter character-range dispatch table"),
        (0x0805F27C, 4, 9, "abs32", "thumb", "sub_0805F25C character dispatch table"),
        (0x0805F36C, 4, 15, "abs32", "thumb", "GetFontStrWith character dispatch table"),
        (0x0805F6C8, 4, 15, "abs32", "thumb", "sub_0805F6A4 text-width dispatch table"),
        (0x08070958, 4, 16, "abs32", "thumb", "player_PlayerNormal input dispatch table"),
        (0x0805677C, 4, 15, "abs32", "thumb", "RunTextCommand command dispatch table"),
        (0x0807BDD8, 4, 8, "abs32", "thumb", "sub_0807BDB8 direction dispatch table"),
        (0x0807C358, 4, 25, "abs32", "thumb", "LoadRoomGfx room-type dispatch table"),
        (0x08080A80, 4, 29, "abs32", "thumb", "UpdateDoorTransition direction dispatch table"),
        (0x0808EAF8, 4, 8, "abs32", "thumb", "sub_0808EABC file-screen object dispatch table"),
        (0x0808EBD8, 4, 6, "abs32", "thumb", "object_fileScreenObjects_Type16 dispatch table"),
        (0x080907E4, 4, 39, "abs32", "thumb", "FurnitureInit flags dispatch table"),
        (0x08090880, 4, 21, "abs32", "thumb", "FurnitureInit type dispatch table"),
    ]

    def mode_for_iwram_func_source(source: int) -> str:
        # RAMFUNCS_BASE starts with a small THUMB routine, then switches
        # to ARM at arm_GetTileAtEntityPos and remains ARM for the copied
        # gameplay hot-path routines in intr.s.
        if source < 0x080B19CC:
            return "thumb"
        return "arm"

    manual_keys = {(addr, mode) for addr, mode, _name, _note in manual_entries}
    for sym in sorted(data_syms, key=lambda s: (s.addr, s.name)):
        if sym.region != "iwram" or not sym.name.startswith("ram_"):
            continue
        for runtime_start, source_start, size, name, _note in code_copies:
            if name != "iwram_funcs":
                continue
            if runtime_start <= sym.addr < runtime_start + size:
                source = source_start + (sym.addr - runtime_start)
                mode = mode_for_iwram_func_source(source)
                key = (sym.addr, mode)
                if key in manual_keys:
                    break
                manual_entries.append(
                    (sym.addr, mode, sym.name,
                     f"IWRAM copy; source bytes at 0x{source:08X}"))
                manual_keys.add(key)
                break

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Generated by tools/import_tmc_symbols/import_tmc_symbols.py\n")
        fh.write("# Source: zeldaret/tmc asm labels plus ROM header metadata.\n")
        fh.write("# Do not hand-edit; rerun the importer.\n\n")

        fh.write("[program]\n")
        fh.write('name = "The Legend of Zelda: The Minish Cap (USA)"\n')
        fh.write('id = "minishcap_usa"\n')
        fh.write("load_address = 0x08000000\n")
        fh.write(f"size = 0x{len(rom):08X}\n")
        fh.write("entry_pc = 0x08000000\n\n")

        fh.write("[identity]\n")
        fh.write(f'sha1 = "{sha1}"\n\n')

        fh.write("[[data_range]]\n")
        fh.write("start = 0x08000004\n")
        fh.write("end = 0x080000C0\n")
        fh.write('note = "GBA ROM header and Nintendo logo bytes"\n\n')

        for runtime_start, source_start, size, name, note in code_copies:
            fh.write("[[code_copy]]\n")
            fh.write(f"runtime_start = 0x{runtime_start:08X}\n")
            fh.write(f"source_start = 0x{source_start:08X}\n")
            fh.write(f"size = 0x{size:08X}\n")
            fh.write(f'name = "{name}"\n')
            fh.write(f'note = "{note}"\n\n')

        for addr, mode, name, note in manual_entries:
            fh.write("[[extra_func]]\n")
            fh.write(f"addr = 0x{addr:08X}\n")
            fh.write(f'mode = "{mode}"\n')
            fh.write(f'name = "{name}"\n')
            fh.write(f'note = "{note}"\n\n')

        for addr, stride, count, fmt, entries_mode, note in manual_jump_tables:
            fh.write("[[jump_table]]\n")
            fh.write(f"addr = 0x{addr:08X}\n")
            fh.write(f"stride = {stride}\n")
            fh.write(f"count = {count}\n")
            fh.write(f'format = "{fmt}"\n')
            fh.write(f'entries_mode = "{entries_mode}"\n')
            fh.write(f'note = "{note}"\n\n')

        for f in rows:
            fh.write("[[extra_func]]\n")
            fh.write(f"addr = 0x{f.addr:08X}\n")
            fh.write(f'mode = "{f.mode}"\n')
            fh.write(f'name = "{f.name}"\n')
            fh.write(f'note = "{f.source_file}"\n\n')

    return len(rows) + len(manual_entries)


def write_ghidra_script(path: pathlib.Path,
                        funcs: list[Func],
                        data: list[DataSym]) -> None:
    """Ghidra Jython script: applies symbols to the open program.

    Usage in Ghidra: Window → Script Manager → load this file →
    Run. The script expects the cart program (loaded at 0x08000000)
    to be the current program; data symbols outside ROM are skipped
    unless the program covers them.
    """
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write(GHIDRA_SCRIPT_HEADER)
        fh.write("\n# ── Functions ───────────────────────────────\n")
        fh.write("FUNCTIONS = [\n")
        for f in sorted(funcs, key=lambda f: f.addr):
            mode = "thumb" if f.mode == "thumb" else "arm"
            fh.write(f"    (0x{f.addr:08X}, {mode!r}, {f.name!r}),\n")
        fh.write("]\n\n")
        fh.write("# ── Data symbols ────────────────────────────\n")
        fh.write("DATA_SYMS = [\n")
        for s in sorted(data, key=lambda s: s.addr):
            fh.write(f"    (0x{s.addr:08X}, {s.region!r}, {s.name!r}),\n")
        fh.write("]\n\n")
        fh.write(GHIDRA_SCRIPT_BODY)


GHIDRA_SCRIPT_HEADER = '''# import_symbols.py — Ghidra Jython script.
#
# Applies tmc-derived function and data symbols to the current
# program. Use from Ghidra's Script Manager.
#
# Generated by tools/import_tmc_symbols/import_tmc_symbols.py.
# Do not hand-edit — re-run the importer instead.

# @author tools/import_tmc_symbols
# @category Symbol Import
# @keybinding
# @menupath
# @toolbar

from ghidra.program.model.symbol import SourceType
from ghidra.program.model.address import AddressOutOfBoundsException
'''


GHIDRA_SCRIPT_BODY = '''
def _addr(value):
    return currentAddress.getAddressSpace().getAddress(value)


def _apply_function(addr_int, mode, name):
    try:
        addr = _addr(addr_int)
    except AddressOutOfBoundsException:
        return False
    if not currentProgram.getMemory().contains(addr):
        return False
    fm = currentProgram.getFunctionManager()
    existing = fm.getFunctionAt(addr)
    if existing is None:
        try:
            createFunction(addr, name)
        except Exception as exc:
            print("createFunction failed at %s (%s): %s" %
                  (addr, name, exc))
            return False
    else:
        existing.setName(name, SourceType.USER_DEFINED)
    # Mark the function's THUMB mode via the TMode context register
    # if Ghidra recognises it for this processor.
    try:
        tmode = currentProgram.getProgramContext().getRegister("TMode")
        if tmode is not None:
            currentProgram.getProgramContext().setValue(
                tmode, addr, addr,
                java.math.BigInteger.valueOf(1 if mode == "thumb" else 0))
    except Exception:
        pass
    return True


def _apply_label(addr_int, name):
    try:
        addr = _addr(addr_int)
    except AddressOutOfBoundsException:
        return False
    if not currentProgram.getMemory().contains(addr):
        return False
    st = currentProgram.getSymbolTable()
    st.createLabel(addr, name, SourceType.USER_DEFINED)
    return True


def main():
    fn_in_range = 0
    fn_skipped = 0
    for addr_int, mode, name in FUNCTIONS:
        if _apply_function(addr_int, mode, name):
            fn_in_range += 1
        else:
            fn_skipped += 1

    data_in_range = 0
    data_skipped = 0
    for addr_int, region, name in DATA_SYMS:
        if _apply_label(addr_int, name):
            data_in_range += 1
        else:
            data_skipped += 1

    print("Functions: %d applied, %d skipped (out of program range)" %
          (fn_in_range, fn_skipped))
    print("Data symbols: %d applied, %d skipped (out of program range)" %
          (data_in_range, data_skipped))


import java.math
main()
'''


# ─────────────────────────────────────────────────────────────────────
# main
# ─────────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tmc", type=pathlib.Path,
                    default=ROOT / "third_party" / "tmc",
                    help="Path to the cloned zeldaret/tmc repo.")
    ap.add_argument("--tmc-build", type=pathlib.Path,
                    default=None,
                    help="Path to a local tmc build dir with C objects.")
    ap.add_argument("--out", type=pathlib.Path,
                    default=ROOT / "symbols",
                    help="Output dir for symbol TSVs.")
    ap.add_argument("--ghidra", type=pathlib.Path,
                    default=ROOT / "ghidra",
                    help="Output dir for the Ghidra import script.")
    ap.add_argument("--rom", type=pathlib.Path,
                    default=ROOT / "roms" / "minishcap_usa.gba",
                    help="ROM path used only to hash-anchor TOML output.")
    ap.add_argument("--toml", type=pathlib.Path,
                    default=ROOT / "symbols" / "minishcap.toml",
                    help="Output path for gba_recompile TOML config.")
    args = ap.parse_args()

    if not args.tmc.exists():
        print(f"error: tmc checkout not found at {args.tmc}",
              file=sys.stderr)
        print("Clone with: git clone --depth 1 "
              "https://github.com/zeldaret/tmc <path>", file=sys.stderr)
        return 1

    asm_root = args.tmc / "asm"
    if not asm_root.exists():
        print(f"error: {asm_root} missing — wrong tmc layout?",
              file=sys.stderr)
        return 1

    print(f"==> scanning {asm_root}")
    funcs: list[Func] = []
    for asm_file in sorted(asm_root.rglob("*.s")):
        funcs.extend(parse_asm_file(asm_file))
    print(f"    found {len(funcs)} function symbols")

    tmc_build = args.tmc_build or (args.tmc / "build" / "USA")
    print(f"==> scanning built C objects in {tmc_build}")
    c_funcs = parse_built_c_objects(args.tmc, tmc_build, funcs)
    if c_funcs:
        funcs.extend(c_funcs)
        funcs = dedupe_funcs(funcs)
    print(f"    found {len(c_funcs)} C object function symbols")

    infer_boundaries(funcs)

    linker_ld = args.tmc / "linker.ld"
    print(f"==> scanning {linker_ld}")
    data_syms = parse_linker_ld(linker_ld) if linker_ld.exists() else []
    print(f"    found {len(data_syms)} data symbols")

    args.out.mkdir(parents=True, exist_ok=True)
    args.ghidra.mkdir(parents=True, exist_ok=True)

    n1 = write_imported_symbols(
        args.out / "imported_symbols.tsv", funcs)
    n2 = write_function_boundaries(
        args.out / "function_boundaries.tsv", funcs)
    n3 = write_data_symbols(
        args.out / "imported_data_symbols.tsv", data_syms)
    n4 = write_recompiler_toml(args.toml, funcs, args.rom, data_syms)
    write_ghidra_script(
        args.ghidra / "import_symbols.py", funcs, data_syms)

    print(f"==> wrote {n1} rows to {args.out / 'imported_symbols.tsv'}")
    print(f"==> wrote {n2} rows to {args.out / 'function_boundaries.tsv'}")
    print(f"==> wrote {n3} rows to {args.out / 'imported_data_symbols.tsv'}")
    if n4:
        print(f"==> wrote {n4} entries to {args.toml}")
    print(f"==> wrote Ghidra script to "
          f"{args.ghidra / 'import_symbols.py'}")

    if funcs:
        by_region: dict[str, int] = defaultdict(int)
        for f in funcs:
            by_region[region_for(f.addr)] += 1
        print("    function counts by region:")
        for region, n in sorted(by_region.items()):
            print(f"      {region:12s} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
