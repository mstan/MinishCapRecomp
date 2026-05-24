// import_tmc_symbols — STUB.
//
// Eventually: read symbol output from a local checkout of
// `zeldaret/tmc` (typically a `.map` / `.sym` / generated metadata
// file the decomp produces) and write normalized TSVs into
// `symbols/imported_symbols.tsv` and `symbols/function_boundaries.tsv`.
//
// The decomp's C source is *not* read by this tool. Only metadata.

#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
    std::printf("import_tmc_symbols: stub. See symbols/README.md.\n");
    return 0;
}
