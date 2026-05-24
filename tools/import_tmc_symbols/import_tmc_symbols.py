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
import pathlib
import re
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
    for line in text.splitlines():
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
    ap.add_argument("--out", type=pathlib.Path,
                    default=ROOT / "symbols",
                    help="Output dir for symbol TSVs.")
    ap.add_argument("--ghidra", type=pathlib.Path,
                    default=ROOT / "ghidra",
                    help="Output dir for the Ghidra import script.")
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
    write_ghidra_script(
        args.ghidra / "import_symbols.py", funcs, data_syms)

    print(f"==> wrote {n1} rows to {args.out / 'imported_symbols.tsv'}")
    print(f"==> wrote {n2} rows to {args.out / 'function_boundaries.tsv'}")
    print(f"==> wrote {n3} rows to {args.out / 'imported_data_symbols.tsv'}")
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
