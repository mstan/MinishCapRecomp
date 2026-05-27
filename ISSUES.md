# Minish Cap — Known Issues

Burndown chart for the recompiled Minish Cap title. Format mirrors
`gbarecomp/ISSUES.md`: ordered by severity, not chronology. Each
entry captures what the user reproduced, what we suspect, and what
the next concrete step is. Platform-level issues that happen to be
visible *through* Minish Cap stay here as long as the dominant
reproducer is the game.

Baseline as of `a4d0187` (symbols: jump tables + IWRAM-copy mode
split) on top of gbarecomp `29e81ac` (title-screen checkpoint):
new save → load → title screen → Zelda intro cutscene → Link's
bedroom → ground floor of Link's house renders cleanly. First
crash on the path out of the house through the right door.

---

## High priority

### MC-HP-000: Function-finder relies on manual hints; cheap discovery heuristics not implemented
- **Observed:** 2026-05-25, during MC-HP-001 triage. The 28
  `[[jump_table]]` entries shipped in `a4d0187` and the growing
  list of manual `[[extra_func]]` entries are all symptoms of
  the discovery pass not catching idioms it could be catching.
  MC-HP-001 (dispatch miss at `0x0804B208`) is the latest
  instance; closing it with another `extra_func` line would be
  a symptom patch.
- **Detail:** Today's finder is recursive-descent from known
  entry points + direct `BL`/`B`. It cannot follow indirect
  control flow (`BX rN`, `LDR pc, [...]`, function-pointer
  callbacks, jump tables) without an explicit hint. Every
  per-game manual entry we accumulate is evidence of a finder
  gap that, once closed, would have caught it for free — and
  for every game.
- **Heuristics to evaluate, precision-first:**
  - **Literal-pool sweeping.** ARM/THUMB code stores function
    pointers as 32-bit words in PC-relative literal pools next
    to functions, then loads them via `LDR rN, =0x08XXXXXX`
    for a later `BX`. Sweep code-section literal pools; any
    word matching `(0x08000000..0x0BFFFFFF | thumb-bit)` whose
    target disassembles to a plausible prologue is
    overwhelmingly a code pointer. Cheap, high precision,
    very high recall.
  - **Jump-table idiom recognition.** Compiler emits indirect
    dispatch in a small set of canonical shapes
    (`CMP rN,#count; BCS default; ADD pc,pc,rN,LSL #2; B…;
    B…;` or `LDR pc,[pc,rN,LSL #2]` followed by N aligned
    words). Ghidra and IDA both pattern-match these. Almost
    all 28 manual jump tables we just added would have been
    caught automatically.
  - **Function-prologue scanning in IWRAM-copied regions.**
    These regions are bounded, 100% code, and small. Linear
    sweep for `PUSH {lr, …}` / `STMFD sp!, {…}` shapes — high
    recall with negligible false-positive risk inside the
    bounded region.
  - **Value analysis (cheap case).** `LDR rN, =literal` inside
    a basic block followed by `BX rN` is trivially resolvable
    without cross-BB constant propagation. Catches the common
    one-call-site indirect.
  - **Expanded config hints.** `[[function_pointer_word]]`
    (single callback slot), `[[function_pointer_table]]`
    (region of N consecutive `void(*)()` words — Minish Cap's
    task system is full of these), `[[scan_region]]`
    (opt-in linear sweep over a known code-only PC range).
    Lower-risk than full heuristics because the user opts in
    per-region; useful as a safety valve and as a starting
    point before pattern detection lands.
- **Cross-pollination:** survey `segagenesisrecomp` (M68K),
  `psxrecomp` (MIPS R3000), `nesrecomp` (6502), and
  `snesrecomp` (65816) for technique inventory. Instruction
  sets differ but the *shape* of the discovery problem is
  identical (recursive descent + indirect-control hints +
  manual entries). Whatever has worked / failed there should
  inform implementation order here. Specifically check whether
  any of them already implement literal-pool sweep or
  jump-table idiom recognition — those are the highest-value
  imports.
- **Priority:** high — this is the proper-fix prerequisite for
  MC-HP-001 and for every future dispatch miss. Per the
  "completeness > shortcut" rule and the recompiler-discipline
  posture (fix the recompiler tool, not the symptom), the
  immediate `extra_func` unblock for MC-HP-001 is fine, but
  the long-term work has to land here. False positives are
  bad; the precision-first ordering above is meant to ensure
  every discovered entry is verifiable before it goes into the
  generated output.
- **Next step:** start with the cross-project survey to avoid
  re-inventing what already exists. Then implement in the
  precision-first order listed. Manual hints remain available
  as a safety valve for things the heuristics still can't
  reach, but the goal is for the manual-hint count to *drop*
  with each landed heuristic — a rising count is a regression
  signal.
- **Progress (2026-05-26, gbarecomp `beaab83`):** Survey done
  (jump-table recognition was the one universal technique we
  lacked). Landed automatic **abs32 jump-table detection** in
  the function finder: confirmation-gated (emit only when the
  loaded entry is branched to — `BX Rt` / `MOV pc,Rt` /
  `BL`-into-bx-veneer, incl. the THUMB BL prefix/suffix pair),
  with a function-prologue entry-validity gate (validate-and-
  stop). On Minish Cap it confirms **192** distinct table bases
  and rediscovers **18/27** manual `[[jump_table]]` entries
  (incl. the canonical `0x08100CBC`) with **zero change to
  generated output** (42991 functions == baseline; the 27 manual
  entries stay benignly suppressed by their data_ranges). Two
  precision bugs were found + fixed via the execution loop
  (eager-emit explosion → confirmation gate; weak `!is_undefined`
  gate over-counting → prologue gate). Added `GBARECOMP_NO_JT`
  toggle + a 2M-entry worklist brake (safety net).
- **Open follow-ups (lower priority):**
  1. **Computed/offset-base idiom** — the remaining 9 misses
     (the `0x080FCxxx` file-select cluster + `0x08090880`) load
     the table base several instructions + `bl` calls before the
     indexed use, so the in-block base constant doesn't survive
     to the `ADD`. Needs base-liveness tracking across calls.
     Bounded effort was spent and the limit accepted (ship 18).
  2. **Regen speed — RESOLVED (gbarecomp `09ff973`).** Discovery
     was slow because every branch target was pushed to the
     worklist and dedup'd only at visit time (~170k of mostly-
     duplicates). Dedup-at-push cut full discovery+codegen from
     **minutes to 5.5s** with byte-identical generated output.
     That was the whole bottleneck; codegen parallelization /
     content-hash caching are no longer warranted.

### MC-HP-001: Crash when Link walks through the right-side door on Link's-house ground floor
- **Observed:** 2026-05-25. After the Zelda cutscene resolves and
  Link can move, the ground-floor room (Image #4) renders 100%
  correctly. Walking through the right-side door of that room
  causes the application to terminate immediately (exit code 3).
- **Detail:** stderr trail ends with
  `runtime_arm: dispatch miss for pc=0x0804B208 (no generated
  function; not recompiled, or function-finder didn't reach it)`.
  Trace ring shows a long call chain inside `0x0804Bxxx` before
  the miss. Cart address; needs a function entry.
- **Suspected cause:** function-finder didn't reach `0x0804B208` —
  likely an indirect-call / table edge that the discovery pass
  doesn't currently follow. This is the "dispatch miss is a silent
  game-breaking bug" rule firing as designed (per
  `gbarecomp/CLAUDE.md` DISPATCH MISS RULE).
- **Priority:** high — blocks all gameplay past the opening room.
- **Proper fix gated by:** MC-HP-000. The immediate `extra_func`
  unblock is acceptable for triage; the long-term fix is for the
  function-finder to discover `0x0804B208` without a manual hint.
  Closing this with another `extra_func` line and skipping the
  finder audit would convert this entry into permanent tech debt
  and guarantee a near-identical miss next time we cross a new
  room boundary.
- **Next step:** add `extra_func 0x0804B208` (mode TBD — likely
  `thumb` given surrounding ARM/THUMB context shown in trace) to
  `symbols/minishcap.toml`, regenerate, rebuild. Then run again
  and walk the same path; check `dispatch_misses.log` for the next
  follow-on miss and iterate until the doorway transition completes.
  Once a clean traversal exists, *do not close this issue* —
  hand it to MC-HP-000 with the xref data (caller PC, branch type,
  table source if any) so the audit pass has a concrete first
  case to validate the new heuristic against.

### MC-HP-002: Long unresponsive hangs at cutscene boundaries
- **Observed:** 2026-05-25. Two distinct multi-second hangs during
  the intro path. (1) After the stained-glass cards + text
  sequence, a long black screen during which Windows flags the
  window as "Not Responding". (2) At the start of the
  Zelda-walks-into-Link's-house cutscene the same Not-Responding
  freeze recurs before the scene plays out.
- **Detail:** Game eventually unblocks on its own and continues
  correctly (Image #2 "Good morning, Master Smith" renders fine
  afterwards). Host window stops pumping messages while blocked,
  which is what triggers Windows' unresponsive flag.
- **Suspected cause:** a single recompiled function (or tight
  inner loop) running for hundreds of millions of dispatches
  without ever crossing a PPU frame boundary, so `step_frame`'s
  loop and the in-loop `pump_host_input` (every 512 dispatches in
  `step_once`) keep firing — but the per-frame pump only happens
  *between* recompiled function returns, and if a single dispatch
  runs unbounded (intro logic doing CPU-only work), the OS message
  pump starves. Could also be a busy-wait waiting on a hardware
  event that the recomp isn't ticking (timer, audio FIFO,
  serial). Need a trace at the moment of the hang to confirm.
- **Priority:** high — UX-breaking, and "spins forever" can mask
  a real divergence (e.g., waiting on an IRQ that never fires).
- **Next step:** during a hang, capture `runtime_trace` over TCP
  and look for: (a) PC stuck in a small range (busy-wait), (b) IE
  / IME / VCOUNT-style I/O reads with no progress, (c) timer
  state that should advance but isn't. Independently, give the
  host window an OS message pump on a watchdog timer so the
  "Not Responding" flag goes away even when the recomp is
  computing — but only after we know what the recomp is actually
  doing, since making the symptom invisible is the wrong fix if
  the underlying spin is a bug.

### MC-HP-003: Severe screen garbling during Zelda's room-transition
- **Observed:** 2026-05-25. During the cutscene transition where
  Zelda walks off-screen into the next room, the display garbles
  badly (Image #1: torn / scrambled tiles, palette bleed, OAM
  artifacts). The transition completes and the destination screen
  (Image #2) is correct — so the corruption is confined to the
  transition window itself. The application also hangs (see
  MC-HP-002) at the moment the garbled frame is on screen.
- **Detail:** Looks like in-flight DMA / VRAM rewrite is being
  presented mid-update — i.e. the framebuffer is sampled while
  the game is rewriting tile data + map data + palette for the
  new room, rather than presenting on the VBlank boundary the
  game expects.
- **Suspected cause:** PPU frame-latch timing relative to the
  game's room-load sequence. The new `step_frame` loop (committed
  in `29e81ac`) presents the latched framebuffer on every PPU
  frame advance — if the latch isn't gated on the same VBlank
  IRQ boundary the game uses to commit a room swap, we'll present
  the half-loaded frame. Could also be DMA timing (HBlank DMA
  used for line-by-line scroll) being executed at the wrong
  cycle phase.
- **Priority:** high — visible and persistent across every
  transition we will encounter; will get worse the deeper into
  the game we go.
- **Next step:** capture a frame-by-frame BMP scan across one
  transition (`--frames 16 --dump-bmp` family), diff against
  mGBA at the same VBlank counts, and identify the first frame
  where the corruption appears. From there determine whether the
  corruption is a presentation-timing bug (wrong frame latched
  for present) or a memory-state bug (VRAM/PAL/OAM actually
  contains garbage at that moment).

### MC-HP-004: Gameplay ran at monitor-refresh speed — RESOLVED 2026-05-26
- **Observed:** 2026-05-25. Entire run (BIOS intro included,
  best estimate) plays back at approximately double speed —
  Zelda walks too fast, dialog auto-advance feels accelerated,
  audio is shifted up.
- **Root cause (2026-05-26):** the windowed runtime had no
  wall-clock frame limiter; presentation was gated solely by the
  SDL renderer's `PRESENTVSYNC`, i.e. the *host monitor's* refresh
  rate. On the dev machine's 164 Hz panel that is 164 / 59.7275 ≈
  2.75x real-time (the "roughly 2x" the user saw). Not a mixer or
  cycle-budget bug — purely host presentation pacing. Distinct from
  `gbarecomp/ISSUES.md` LP-001 (sub-1% BIOS chime drift), which
  stands.
- **Resolution:** added `FramePacer` in
  `gbarecomp/src/runtime/host_platform.{h,cpp}` — a monotonic
  wall-clock limiter keyed to the exact GBA frame period
  (16'777'216 / 280'896 = 59.7275 Hz). Hybrid sleep-then-spin with
  a 1 ms Windows timer-resolution bump (`timeBeginPeriod`) so the
  ~16.74 ms target isn't wrecked by the default ~15.6 ms scheduler
  granularity; resyncs instead of catch-up if it falls a frame
  behind. `PRESENTVSYNC` removed from the renderer so present()
  never blocks on the display. Constructed only for windowed runs
  (headless/TCP batch stays uncapped by design). Hold **Tab** to
  uncap (fast-forward).
- **Verification:** user confirmed normal-speed gameplay; 300-frame
  timed runs land near the 59.7 Hz budget (vs ~0.9 s unpaced).
- **Note:** this also stops the audio-queue overflow/clear churn
  that the over-fast loop caused, since frames now arrive at the
  rate the SDL audio device consumes them.

### MC-HP-005: Save states — RESOLVED 2026-05-26
- **Observed:** 2026-05-25. User flagged the absence as a
  debug-loop blocker — every bug repro currently requires
  walking back through the BIOS intro + stained-glass + Zelda
  cutscene from cold start, which makes the other four
  high-priority entries each cost several minutes per attempt.
- **Resolution (2026-05-26):** Implemented a versioned binary
  snapshot (`GBAS` container, format v1) in
  `gbarecomp/src/debug/snapshot.{h,cpp}`. Captures the full
  machine at the dispatch boundary between `step_once()` calls
  (host C stack empty — the only safe boundary per
  PRINCIPLES.md): `g_cpu` + the host-side call-return stack,
  EWRAM/IWRAM/PAL/VRAM/OAM, the IO page + timer/DMA shadow
  state, the audio mixer + FIFOs + pending output ring, the
  EEPROM chip, and the PPU. Per-subsystem `serialize`/
  `deserialize` methods (value-state only; live `ppu_`/`bus_`/
  `irq_`/`audio_` pointers stay wired across a restore). The
  blob stores the ROM SHA-1; load refuses a state from a
  different ROM, a different format version, or a truncated
  file. BIOS/ROM bytes are NOT serialized — reloaded and
  hash-verified at launch.
- **Surfaces:** TCP `savestate_save`/`savestate_load {path}`
  (see `gbarecomp/TCP.md`), and host-window nine slots —
  **F1..F9** load slot 1..9, **Shift+F1..F9** save slot 1..9,
  each backed by a `<rom>.stateN` file.
- **Verification:** `MinishCapRecomp/tools/savestate_roundtrip.py`
  proves four properties against the runtime as its own oracle:
  (1) restore fidelity, (2) deterministic replay from a restored
  point, (3) byte-identical save→load→save, (4) the gate cleanly
  rejects tampered magic/version/SHA-1/truncated blobs and the
  machine stays usable afterward. All green at warmup 40 and 200.
- **Follow-up (optional, low):** snapshot size is ~553 KB
  uncompressed; if a slot count grows this could gzip. Not
  needed for the debug loop.

---

## Resolved (intentional baseline reference)

These are not issues — recording them so future regressions are
unambiguous against the 2026-05-25 baseline at
`MinishCapRecomp` `a4d0187` / `gbarecomp` `29e81ac`.

- Title screen, "new file" creation, file save to disk, and file
  load back from disk all complete cleanly.
- "Good morning, Master Smith" cutscene frame (Image #2) renders
  100% correctly.
- Link's bedroom (Image #3) renders 100% correctly.
- Link's-house ground floor (Image #4) renders 100% correctly
  and Link can walk freely inside the room.

If any of the above ever regresses, treat it as a new high-
priority entry rather than a re-open here.
