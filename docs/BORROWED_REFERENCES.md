# Borrowed references

A precise list of what we pull from external Minish Cap projects and
*how* we pull it. The boundary between "reference" and "execution
truth" must stay sharp.

## zeldaret/tmc (decomp)

| Item                          | How it enters this repo                          | Treat as |
|-------------------------------|--------------------------------------------------|----------|
| Function names / addresses    | `tools/import_tmc_symbols/` → TSV                | Reference |
| Function boundaries            | `tools/import_tmc_symbols/` → TSV                | Reference |
| Section / ROM-layout notes    | Free-form notes in `docs/MINISH_CAP_NOTES.md`    | Reference |
| Decompiled C source            | **Never read or copied**                         | — |
| PC-port renderer / audio / input | **Never read or copied**                       | — |
| HLE helpers                    | **Never read or copied**                         | — |
| Annotated hardware behavior   | Cross-checked against GBATEK + oracle, then maybe added to `gbarecomp/src/gba` with citation | Hypothesis |

## tmc disassembly projects (if useful)

Same rules apply: symbol / address / boundary metadata is fine;
behavior interpretation is a hypothesis to test against the oracle.

## Hardware references

See `../../gbarecomp/docs/GBA_REFERENCE_NOTES.md`. Primary sources
are GBATEK, Tonc, CowBite, plus mGBA + NanoBoyAdvance as oracles.

## License posture

- Imported symbol metadata is treated as a derived list; we don't
  redistribute the decomp's source.
- The user is expected to clone `zeldaret/tmc` themselves and point
  the importer at it. If the decomp's license terms change such that
  metadata extraction becomes restricted, we stop importing.
- ROM hashes are public facts, not redistributable ROM content.
