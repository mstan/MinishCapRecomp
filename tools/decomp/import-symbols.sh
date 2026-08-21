#!/usr/bin/env bash
# import-symbols.sh — build the pinned zeldaret/tmc decomp and import its
# symbols.
#
# Run under WSL. End to end: submodule -> byte-exact ROM -> readelf -> the
# engine's shared importer -> symbols/. Nothing here needs root.
#
# The submodule at third_party/tmc is the PROVENANCE record: it pins the
# upstream revision whose build reproduces this project's ROM. Its own
# tmc.sha1 declares b4bd50e4131b027c334547b4524e2dbbd4227130, which is
# exactly this project's [identity].sha1. It is not the build sandbox —
# agbcc installs INTO a decomp tree and building over /mnt is slow — so a
# WSL-side clone is checked out to the pinned SHA and built there. Same split
# MKSC uses (MarioKartSuperCircuitRecomp/tools/decomp/provision.sh).
#
# STATUS: unlike the Gen3 games, this path has NOT yet been run end to end
# here. Minish Cap's recompile is still driven by the importer-generated
# symbols/minishcap.toml, which also holds 27 hand-derived [[jump_table]]
# entries an import would discard — so the conversion onto game.toml plus
# layered overlays is deliberately pending. Until that lands, running this
# script will produce symbols/ artifacts that nothing consumes yet. The ROM
# hash gate below still applies, so a decomp revision that stops matching
# fails loudly rather than quietly emitting wrong symbols.
#
# Usage: tools/decomp/import-symbols.sh [--force-clone]
set -euo pipefail

DECOMP_NAME="tmc"
DECOMP_URL="https://github.com/zeldaret/tmc"
EXPECTED_SHA1="b4bd50e4131b027c334547b4524e2dbbd4227130"
PROGRAM_ID="AGBZ"
PROGRAM_NAME="The Legend of Zelda: The Minish Cap (USA)"
ROM_PATH_REL="roms/minishcap_usa.gba"

# tmc's IWRAM copies as this project already models them (see
# symbols/minishcap.toml [[code_copy]]): the M4A mixer and the RAMFUNCS block.
# Expressed as symbol pairs so the importer resolves the addresses itself; if
# a name does not resolve the importer warns and skips it rather than guessing.
CODE_COPY_PAIRS=(
  "SoundMainRAM_Buffer=SoundMainRAM:thumb"
)

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SANDBOX="${DECOMP_SANDBOX:-$HOME/${DECOMP_NAME}-build}"
AGBCC_SRC="${AGBCC_SRC:-}"
J="${J:-$(nproc)}"
IMPORTER="$REPO/gbarecomp/tools/symbol_import/import_decomp_symbols.py"

say() { printf '==> %s\n' "$*"; }

[ -f "$IMPORTER" ] || {
    echo "missing $IMPORTER — is the gbarecomp submodule checked out?" >&2
    exit 1
}

PINNED="$(git -C "$REPO" ls-tree HEAD "third_party/$DECOMP_NAME" | awk '{print $3}')"
[ -n "$PINNED" ] || { echo "third_party/$DECOMP_NAME is not a submodule" >&2; exit 1; }
say "pinned $DECOMP_NAME revision: $PINNED"

if [ "${1:-}" = "--force-clone" ]; then rm -rf "$SANDBOX"; fi
if [ ! -d "$SANDBOX/.git" ]; then
    say "cloning $DECOMP_URL into $SANDBOX"
    git clone --quiet "$DECOMP_URL" "$SANDBOX"
fi
git -C "$SANDBOX" fetch --quiet origin
git -C "$SANDBOX" checkout --quiet --detach "$PINNED"

if [ ! -x "$SANDBOX/tools/agbcc/bin/old_agbcc" ]; then
    if [ -z "$AGBCC_SRC" ]; then
        for c in "$HOME"/*/tools/agbcc; do
            [ -x "$c/bin/old_agbcc" ] && AGBCC_SRC="$c" && break
        done
    fi
    if [ -z "$AGBCC_SRC" ]; then
        say "building pret/agbcc (no existing install found)"
        rm -rf "$HOME/agbcc-src"
        git clone --quiet https://github.com/pret/agbcc "$HOME/agbcc-src"
        (cd "$HOME/agbcc-src" && ./build.sh >/dev/null && ./install.sh "$SANDBOX")
    else
        say "installing agbcc from $AGBCC_SRC"
        mkdir -p "$SANDBOX/tools/agbcc"
        cp -r "$AGBCC_SRC/." "$SANDBOX/tools/agbcc/"
    fi
fi

cd "$SANDBOX"
# tmc drives its toolchain through TOOLCHAIN_PATH (Toolchain.mk) rather than
# DEVKITARM; empty means "use whatever arm-none-eabi-* is on PATH", which is
# what a plain Ubuntu binutils install provides.
say "building $DECOMP_NAME"
nice -n 10 make -j"$J" TOOLCHAIN_PATH= >/dev/null

got="$(sha1sum tmc.gba | cut -d' ' -f1)"
if [ "$got" != "$EXPECTED_SHA1" ]; then
    echo "SHA-1 MISMATCH: built $got, expected $EXPECTED_SHA1" >&2
    echo "This decomp revision does not reproduce our ROM; refusing to" >&2
    echo "import symbols that would be wrong for it." >&2
    exit 1
fi
say "sha1 $got OK — matches this project's identity"

out="$REPO/symbols"
mkdir -p "$out"
readelf -sW tmc.elf > "$out/${DECOMP_NAME}_syms.txt"
readelf -SW tmc.elf > "$out/${DECOMP_NAME}_sections.txt"
echo "$PINNED" > "$out/${DECOMP_NAME}_revision.txt"

ccargs=()
for p in "${CODE_COPY_PAIRS[@]}"; do ccargs+=(--code-copy-pair "$p"); done

say "importing $PROGRAM_ID"
python3 "$IMPORTER" \
    --id "$PROGRAM_ID" --name "$PROGRAM_NAME" \
    --syms     "$out/${DECOMP_NAME}_syms.txt" \
    --sections "$out/${DECOMP_NAME}_sections.txt" \
    --rom      "$REPO/$ROM_PATH_REL" \
    "${ccargs[@]}" \
    --out "$out"

say "done. NOTE: the recompile still reads symbols/minishcap.toml; wiring"
say "these artifacts in is the pending overlay conversion for this game."
