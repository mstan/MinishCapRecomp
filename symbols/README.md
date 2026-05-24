# symbols/

Imported symbol map for Minish Cap. Files in here are produced by
`tools/import_tmc_symbols/`; do not hand-edit unless explicitly
adding a symbol the decomp doesn't yet have.

| File                          | What it is                                        |
|-------------------------------|---------------------------------------------------|
| `imported_symbols.tsv`        | Tab-separated: `addr  mode  name`                  |
| `function_boundaries.tsv`     | Tab-separated: `start  end  mode  name`            |

Schema:

- `addr`, `start`, `end` are hex with `0x` prefix.
- `mode` is `arm` or `thumb`.
- `name` is the decomp's symbol name verbatim. The recompiler emits
  C identifiers derived from it but the TSV preserves the original
  for round-tripping.

## Where the data comes from

The decomp at `zeldaret/tmc` maintains symbol output in a form we
can parse mechanically. The importer reads it and writes the TSVs
above. The decomp's C source code is **never** read or copied —
only the symbol metadata.

If a function is missing here, it will show up in
`dispatch_misses.log` after a run, and gets added to
`game.toml [functions]`.
