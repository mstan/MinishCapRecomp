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
- **HONEST STATUS — the detector does NOT yet fix any real miss
  (HIGH priority for next session).** Two things were initially
  over-stated and are corrected here after verification:
  1. **"27" is a tiny subset, not the population.** The detector
     CONFIRMED **192** distinct table bases; only 18 overlap the
     27 manual `[[jump_table]]` hints. So the manual list captured
     ~10% of even the *confirmed* tables, and the true universe is
     larger still (computed-base tables are uncounted). "18/27" is
     recall on a small labeled sample, NOT coverage of the game.
  2. **The detector EMITS 0 on Minish Cap → output-neutral → it
     prevents ZERO dispatch misses today.** Of the 192 confirmed:
     27 are suppressed by manual data_ranges (benign), and the
     other ~165 fail emit's gate so seed nothing. Generated output
     is byte-identical to before the detector. Its value is purely
     latent/future right now.
  - **Why emit contributes nothing — the prologue gate is too
    strict.** The validate-and-stop gate requires each entry to
    point at a `push`/`stmfd sp!` prologue. That stopped the
    explosion, but it REJECTS real jump tables whose targets are
    not push-prologues (switch-case labels, handlers that open with
    e.g. `add r0,r4,#0`). This directly blocks the MC-HP-001 door
    crash: its table is at `0x0804B1D0`, target `0x0804B208` opens
    with `add r0,r4,#0` (not a push) → rejected. **Next session:
    replace the prologue gate with a better count-terminator** —
    prefer the dispatcher's `CMP index,#N; BHI/BCS default` bound
    when present (exact count, no guessing), else a code-vs-data
    discriminator that accepts non-prologue code but still rejects
    data pointers.
  - **Open idiom gaps:** (a) computed/offset base loaded several
    instructions + `bl` calls before the indexed use (in-block base
    const doesn't survive) — the `0x080FCxxx` file-select cluster +
    `0x08090880`; (b) IWRAM-copied dispatchers, where PC-relative
    literal-pool base resolution is wrong because the finder walks
    ROM source bytes (`0x080B…`) but runtime PC is IWRAM (`0x0300…`)
    — suspected for MC-HP-001 (see user recollection there).
- **Regen speed — RESOLVED (gbarecomp `09ff973`).** Discovery was
  slow because every branch target was pushed to the worklist and
  dedup'd only at visit time (~170k of mostly-duplicates).
  Dedup-at-push cut full discovery+codegen from **minutes to 5.5s**
  with byte-identical generated output. That was the whole
  bottleneck; codegen parallelization / caching are not warranted.
- **Progress (2026-05-26b) — the detector now EMITS real coverage;
  emit went 0 → 162 tables.** The honest-status blockers above are
  fixed in the function finder (`emit_jump_table` + the index trackers).
  Two coupled root causes, both found with the Ghidra literal oracle
  against MC-HP-001's table:
  1. **Mode bug.** A `MOV pc,Rt` dispatcher is a *non-interworking*
     PC write — it keeps the dispatcher's current mode, so the table
     words carry no thumb bit and every target inherits the
     dispatcher's mode. The old emit always derived mode from `raw&1`,
     so it mis-seeded MC-HP-001's even THUMB targets (e.g. `0x0804B208`)
     as ARM. Confirmation now records the dispatch kind (`BX`/`bx`-veneer
     → per-entry bit0; `MOV pc` → inherit `entry_mode`) and emit honors
     it.
  2. **Count terminator.** The strict "stop at first non-`push`
     prologue" gate rejected every switch table whose cases aren't
     function entries. Replaced with: (primary) an EXACT entry count
     from the dispatcher's `CMP index,#N; B{hi,cs} default` bound,
     captured at the compare and carried through the
     scale→add→indexed-load chain (`reg_bound`→`reg_scaled`→
     `reg_table`→`pend.count`); (fallback, unbounded) a code-vs-data
     discriminator that accepts non-prologue code (alignment + in-ROM +
     not-in-data + a strict multi-instruction defined-decode) but still
     rejects data words.
- **Measured (gbarecomp build + regen, deterministic):** 194 distinct
  confirmed table bases → **162 emitted (2182 targets)**, 31 benign
  overlaps with the manual `[[jump_table]]` hints (their bytes are
  already a data_range, so the walk yields nothing — this is the
  detector independently rediscovering the hand-annotated tables, with
  matching counts), **1** unsized reject, **0** genuine bound
  mismatches. Total emitted functions **42,991 → 44,376** (+1,385 real
  functions previously unreachable by discovery) — bounded growth, no
  explosion, discovery converged. `GBARECOMP_JT_REPORT=1` dumps the
  per-table decision to stderr; the summary now prints
  `jt_confirm_events` + the distinct emitted/overlap/rejected split.
- **The 31 overlaps mean the manual hint list is now largely
  redundant** and can shrink (feedback_toml_is_supplement: hint count
  should drop as heuristics land). Removing the auto-rediscovered
  `[[jump_table]]` entries is a safe follow-up — do it one at a time,
  re-measuring that each removed base still emits from auto-detection
  (a few, e.g. the computed-base `0x080FCxxx` cluster, the detector
  does NOT yet catch — those must stay; see the open idiom gaps).
- **Progress (2026-05-26c) — inverted `BLS`/`BCC` guard + cross-block
  bound; emit 162 → 163, unsized rejects 1 → 0.** Resolved the lone
  unsized reject (table `0x08018798`), which had surfaced as a NEW
  dispatch-miss crash on first exit from Link's house (miss at
  `pc=0x080189A4`; trace showed a `MOV pc` dispatch off base
  `0x08018798`). Two compounding causes, both fixed in the finder:
  1. **Inverted guard.** Its dispatcher uses
     `cmp r0,#0x1c; bls <dispatch>; b <default>` — the in-range branch
     goes TO the dispatch (taken), not away to the default. The
     CMP-bound capture only recognized `B{hi,cs}` (taken→default). Now
     it also handles `B{ls,cc}`: HI/LS bound at N, CS/CC at N-1,
     regardless of branch direction.
  2. **Cross-block bound.** Because the `bls` target is the dispatch,
     the finder walks it as its own seed with fresh tracker state, so
     the bound recovered at the compare never reached the `mov pc`
     walk (hence `bound=no` → unsized → rejected). Added
     `branch_target_bounds_`: for the inverted form the bound is parked
     keyed by the dispatch-target PC and re-seeded into `reg_bound` when
     that seed is walked. `0x08018798` now emits `bound=yes want=29
     got=29`; `pc=0x080189A4` is a generated function. Functions
     44,376 → 44,424. (gbarecomp build clean; boot smoke clean.)
- **Remaining work (next):** (a) the **computed/offset-base idiom**
  (`0x080FCxxx` file-select cluster + `0x08090880`) where the base
  const is loaded several instructions + `bl` calls before the indexed
  use, so the in-block base doesn't survive — needs base-liveness
  across calls. (b) genuinely IWRAM-copied dispatchers (none on the
  house-intro path so far). (c) shrink the now-redundant manual
  `[[jump_table]]` hints (31 overlap auto-detection), one at a time,
  re-measuring each still emits.

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
- **RESOLVED 2026-05-26c — user-confirmed.** The MC-HP-000 rework
  emits the table and `0x0804B208` is a generated dispatchable
  function; the user walked Link through the right door and the side
  room + full scene now play with no `exit 3`. (Superseded the earlier
  "STILL OPEN / detector output-neutral" note.) A follow-on dispatch
  miss on the *first exit from the house itself* (`0x080189A4`) was
  also fixed — see MC-HP-000 progress 2026-05-26c. A further miss on a
  later transition (`0x08062922`) is tracked as MC-HP-006.
- **Investigated 2026-05-26 (Ghidra):** `0x0804B208` is referenced
  as a DATA word from `0x0804B1D0` → it is an entry in a
  code-pointer **table at `0x0804B1D0`** (a jump/function-pointer
  table). Bytes at `0x0804B208` = `20 1c 00 f0 47 f8` =
  `add r0,r4,#0 ; bl …` — real THUMB code, but it does **not** open
  with a `push` prologue. So the detector's prologue entry-gate
  would reject this table even if the dispatcher were walked. This
  table is NOT one of the 27 manual entries.
- **User recollection (lead to verify):** a prior session indicated
  the relevant function is "loaded from in-RAM." Fits an
  **IWRAM-copied dispatcher** (cf. the `iwram_funcs` `[[code_copy]]`,
  ROM `0x080B197C` → IWRAM `0x030056F0`): such code runs at an IWRAM
  PC while the finder walks ROM source bytes, so PC-relative
  literal-pool base resolution (how the dispatcher loads the table
  base) computes the wrong address → detection fails. NEXT SESSION:
  confirm via runtime trace whether the door dispatcher is an
  IWRAM-copied function and whether its table-base load is
  PC-relative.
- **Priority:** high — blocks all gameplay past the opening room,
  and is the concrete proof case that MC-HP-000's detector does not
  yet prevent real misses (prologue gate + IWRAM-PC issue).
- **ROOT-CAUSE FIXED via MC-HP-000 (2026-05-26b) — NO manual hint
  used.** Ghidra ground truth corrected the table geometry: the base
  is **`0x0804B1CC`** (the `0x0804B1D0` in earlier notes is entry[1]);
  the dispatcher at `0x0804B1B6` is
  `ldrb r0,[r4]; sub r0,#1; cmp r0,#0xc; bhi 0x0804B252;
  lsl r0,#2; ldr r1,[=0x0804B1CC]; add r0,r0,r1; ldr r0,[r0];
  mov pc,r0` — a **13-entry `MOV pc` switch table**, all targets THUMB
  (even, mode inherited from the dispatcher; `0x0804B208` opens
  `add r0,r4,#0`). This was NOT an IWRAM case: the dispatcher lives at
  `0x0804B1xx`, well below the `iwram_funcs` code_copy window
  (`0x080B197C+`). The two MC-HP-000 fixes (mov-pc mode inheritance +
  CMP-bound terminator) make this table emit: report line
  `base=0x0804B1CC … MOVpc bound=yes want=13 got=13 -> EMIT`.
- **Verified — the exact crash PC is now dispatchable.** After regen,
  all 10 distinct targets (`0x0804B200,B208,B210,B218,B22C,B234,B23C,
  B244,B24C,B252`) have entries in `generated/dispatch_table.cpp`;
  `0x0804B208` — the literal PC in the dispatch-miss abort — is emitted
  as `autojt_0804B1CC_01`. The dispatch miss for that PC is
  structurally impossible now.
- **Runtime validation done:** cold-boot 600 frames clean
  (`dispatch_misses.log` empty); from the user's cutscene save state,
  drove the full intro conversation to controllable gameplay (Link
  moves freely in all directions) — ~7,000 frames, **zero dispatch
  misses, zero crashes**, vs the old build's `exit 3` on a room
  transition. A `state_postcut` checkpoint was saved
  (`roms/minishcap_usa.state_postcut`) so the final step is cheap.
- **Remaining (manual, ~seconds):** the *exact* right-door-of-ground-
  floor traversal hasn't been walked end-to-end (it sits behind the
  long intro cutscene + pixel-precise stair navigation that blind TCP
  scripting couldn't reach efficiently). Given the exact crash PC is
  now generated and room transitions run miss-free, this is a
  confirmation, not a risk: walk Link out the right door from
  `state_postcut` (or a fresh play) and confirm no abort. Then close.

### MC-HP-006: Dispatch miss at 0x08062922 on a later screen transition
- **Observed:** 2026-05-26c. After the MC-HP-001 + house-exit fixes,
  the user played well past the opening (frame ~38,550, new save
  states made) and hit `runtime_arm: dispatch miss for pc=0x08062922`
  (`exit 3`) on a later screen transition.
- **Trace (always-on ring):** the miss is reached via an indirect
  dispatch out of the function at `0x08062834`
  (`push {r4-r7,lr}; ... ldrb r4,[r5,#0xa]; lsl r0,r4,#2; ...`),
  landing on `0x08062922` with `r0=0x08062922` (a computed PC). Ghidra
  shows `0x08062900` as DATA (no instructions) — i.e. there is a
  jump/pointer table around there. So this is almost certainly the
  **same MC-HP-000 class** (a computed/indirect dispatch the finder
  hasn't sized/emitted yet), NOT a new failure mode.
- **Likely the computed/offset-base idiom** flagged in MC-HP-000's
  open gaps: `0x08062834` scales `r4` (a struct field) and adds a
  literal-pool base several instructions before the indexed use, so
  the in-block base const may not survive to the indexed load.
- **Status: DEFERRED (per user direction 2026-05-26c).** We are
  pivoting to the pervasive transition **hangs (MC-HP-002)** and
  **garbling (MC-HP-003)** first — they affect *every* transition and
  are general-purpose framework issues expected to pay dividends on
  these later PC misses (the transition machinery is the common
  thread). Note the discipline tension: DEBUG.md RULE 0a says clear
  dispatch misses before other debugging; this is a deliberate,
  user-directed exception with the miss documented for return.
- **Next step (when resumed):** Ghidra-map the `0x08062834` dispatcher
  + its table; extend the finder's base tracking to survive the
  multi-instruction / cross-call base load (base-liveness), regen,
  confirm `0x08062922` emits. Reuses the MC-HP-000 machinery.
- **FOLLOW-UP IDEA (user, 2026-05-27): seed transition misses
  programmatically, not crash-and-capture one-by-one.** Screen
  transitions are the dominant source of these computed-dispatch misses
  (room-state machines indexing jump/pointer tables). Rather than play
  to each crash, capture the PC, add a hint, repeat — investigate a
  systematic pass: e.g. (a) a finder sweep that recognizes the
  transition-dispatcher idiom family and emits all reachable tables up
  front; (b) an offline harness that drives the game through many
  transitions under the runtime's dispatch-miss logger (collecting all
  miss PCs in one pass, then bulk-resolving them in the finder — NOT as
  permanent TOML hints, but as proof cases to harden the heuristics);
  (c) cross-reference the decomp's known dispatcher/table symbols to
  pre-validate finder coverage. Goal: drive the transition-miss count to
  zero by improving discovery, so no per-crash whack-a-mole. Tie to
  MC-HP-000's open computed/offset-base idiom gap.

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
- **▶ SESSION 4 (2026-05-28) — fresh-boot instruction-level diff; first real
  recomp bug FOUND+FIXED (banked-SP reset), but it is NOT this hang. Read this
  first; it reframes everything below.** Built a cycle-indexed per-instruction
  fingerprint diff in gbarecomp: `g_runtime_cycles` clock (also fixes the
  session-3 "cycles incomparable" red herring), `runtime_insn_fp` ring + codegen
  emit in every instruction prologue, a bios_smoke mirror, and
  `oracle/diff_cycle_trace.py` (free-run + architectural anchor → first divergent
  instruction). Per direction, switched to **FRESH RUNS, no savestates** (state3
  was masking/contaminating the comparison). First divergence on fresh boot:
  **cyc 16, BIOS pc=0x90 `msr cpsr,#0x1f`** banked in SP=0 vs the canonical
  `0x03007F00` — `reset_recomp_cpu` set only the active SVC SP and left the
  User/IRQ banked SPs zeroed. **FIXED** in gbarecomp (seed banked SPs to the
  canonical GBA reset values, matching mGBA/interp/hardware); after the fix
  recomp==interp **bit-for-bit** through the whole comparable window. **But the
  intro hang STILL reproduces (~f3656, `oracle/check_hang.py`)** — the banked-SP
  bug was real but not the cause. Also CONFIRMED *with data* that the recomp's
  TCP `step` (step_frame) is FUNCTION-GRANULAR and OVERSHOOTS frame boundaries on
  long internal-`goto` loops (e.g. the BIOS VRAM-clear at 0x00000C04 = one
  `runtime_dispatch` spanning ~7 frames) — so frame-granular memory diffs (incl.
  the session-3 ones below) show PHANTOM divergence: the recomp is merely AHEAD,
  not wrong. NEXT: catch the first REAL IWRAM divergence on fresh boot via the
  fingerprint ring (the method that nailed the banked-SP bug) — no heavy
  pause/step lockstep with the interp.
- **▶ SESSION 3 (2026-05-28) — TWO leading hypotheses REFUTED; root narrowed
  to a real ~1-game-frame lead whose cause is NOT IRQ-delivery phase. Read this
  before re-chasing either dead lead.**
  - **(1) "Miscompiled cmp/branch/flag/carry in the m4a chain" (the handoff's
    step-1 lead) — REFUTED by a full static audit.** Hand-decoded every THUMB
    halfword from the ROM for `MPlayMain`(0x080AF908), `tfunc_080AF912/_924/
    _93C/_948/_950/_958/_960/_96A/_976`, `sub_080AFB74`, and `FadeOutBody`
    (0x080B0874) entry, and compared against the generated C's per-instruction
    decode comments: **every instruction matches** (cmp/beq/bge/bne targets,
    the `bl 0x080AFB74` `+0x250`, the `subs r0,#1; strb r0,[r4,#0x10]` countdown
    decrement — all faithful). The flag helpers in `runtime_arm.cpp` are correct
    (`arm_set_nzcv_sub` C=`a>=b`, V=`((a^b)&(a^r))&msb`; `arm_set_nzcv_add`
    C=`r<a`; `arm_cond_passes` every code incl. GE=`n==v`), and `codegen_tests`
    is green. A systematic flag/branch miscompile would also have broken the
    BIOS intro and every frame before f40. → No instruction-level bug in the
    audited m4a path.
  - **Structure identified (so the chain is no longer mysterious):** the
    MPlayMain guard const `0x080AFB80 = 0x68736D53 = 'Smsh'` is the MP2K
    `ID_NUMBER` re-entrancy magic — `MPlayMain` is the standard
    re-entrancy-locked player (`if (ident!='Smsh') return; ident++; …;
    ident='Smsh'`), and `sub_080AFB74` is `bx r3` — an *indirect* callback
    dispatch `((fn)player[0x38])(player[0x3c])`, NOT a fixed recurse. So an
    "extra sequencer pass" in re-entrancy-locked code is a timing/control-flow
    symptom, not a bad compare.
  - **(2) "Wake-from-HALT IRQ-delivery phase (recomp omits `kGbaIrqDelayCycles`
    that the interpreter applies)" (the 2026-05-28d lead) — IMPLEMENTED and
    REFUTED.** Found the concrete discrepancy: `bios_smoke` pumps +7 cycles
    before vectoring out of HALT (`main.cpp:389`); the recomp's `runtime_tick`
    (`runtime_bus_bridge.cpp`) took the IRQ immediately. **Fixed** (kept — it is
    a correct standalone parity improvement): added shared
    `gba::kIrqWakeDelayCycles` (gba_irq.h), refactored `runtime_tick` into
    `tick_devices` + an IRQ-take that pumps the wake latency on the halt path,
    and routed `bios_smoke` through the same constant. 12/12 ctest green,
    `MinishCapRecomp.exe` relinked. **But `track_bytes.py --from 34 --to 55
    --hold up` is byte-for-byte IDENTICAL to before the fix** (channel 0x03004470:
    d=0 thru f39, d=−1 f40 → d=−6 f46, recomp SPUN f47). Byte-identical (not
    "diverges differently") means the halt-wake path isn't even exercised at
    this transition — during the busy scroll the game is NOT idle-halting, so
    the VBlank IRQ is taken on the *non-halt* path, where BOTH engines already
    deliver immediately (the interpreter adds the +7 ONLY on halt-wake). So IRQ
    *phase* is not the cause.
  - **What survives — ONE root, two faces.** The diff_anim "entity freed ~1
    frame early" and the track_bytes "m4a channel decremented one extra time"
    are the SAME phenomenon: the recomp is ~1 *game-logic* frame ahead through
    the transition (game logic advances 1 step per VBlankIntrWait release = 1
    per VBlank IRQ; being 1 ahead force-animates a not-yet-priority-paused /
    freshly-freed slot → the 0x08004286 anim-walker spin). The d=−1 *onset* at
    f40 has a phase-sampling component (recomp parks post-VBlank-handler / after
    the tick, interp pre-handler / before), but the *growth* d→−6 is a real
    extra decrement/frame = a real 1-frame lead, not pure artifact.
  - **NARROWED next experiment (since IRQ phase is ruled out):** the lead must
    come from the OTHER 2026-05-28d candidate — frame-boundary / cycle pacing /
    VBlank-IRQ **count**. Decisive test: count VBlank-IRQ entries (BIOS vec 0x18
    / game handler 0x08016BDC) recomp vs interp across f1..f47 — does the recomp
    deliver an EXTRA or EARLY VBlank IRQ (spurious double-fire of
    `events.vblank_started`, or an `IF` bit not cleared and re-firing), or does
    it simply fit one more game-logic frame per PPU frame because per-instruction
    cycle accounting lets it? Also re-audit whether `g_runtime_vblank_starts`
    increments 1:1 with the interpreter's frame boundary near the transition
    (residual counting skew would manufacture the lead). Tooling to add: a TCP
    stat exposing `irq_entries`/`g_runtime_vblank_starts`, or count vec-0x18
    dispatches in the always-on `g_trace` ring over the window (ring-respecting).
  - **▶▶ BREAKTHROUGH (same session) — the recomp OVER-DELIVERS IRQs from f40;
    this is the root layer, m4a + entity-free are downstream.** Wired an
    authoritative IRQ-entry counter at the recomp's actual delivery site
    (`g_runtime_irq_entries`, incremented in `runtime_irq`, runtime_arm.cpp;
    the run-loop local never incremented because IRQs are taken in
    `runtime_tick`, a different TU — the old `counters` reported 0 for the
    recomp). New probe `oracle/diff_counters.py` compares per-frame
    hardware-event-counter DELTAS (phase-robust) recomp vs interp from state3+Up:
    ```
    f 1..39  irq: R=2/I=2 every frame   ← IDENTICAL delivery
    f40      irq: R=3 / I=1
    f41      irq: R=4 / I=2
    f42      irq: R=4 / I=2
    f43      irq: R=3 / I=2
    f44-45   irq: R=2 / I=2
    f46      irq: R=13 / I=2            ← IRQ storm
    f48      recomp SPUN
    ```
    Through f39 BOTH engines vector exactly 2 IRQs/frame; **starting exactly at
    f40 the recomp vectors EXTRA IRQs, escalating to 13 in one frame just before
    the spin.** One mechanism explains every prior symptom: each extra IRQ runs
    the VBlank handler again → an extra m4a sequencer tick (the 0x03004470
    countdown's extra decrement) AND advances game logic an extra step (entity
    freed ~1 frame early) → eventually a storm → the 0x08004286 anim-walker spin.
    The m4a "f40 divergence" and the "1-frame lead" are BOTH downstream of IRQ
    over-delivery — NOT an m4a miscompile (audit clean) and NOT IRQ-delivery
    *phase* (the kGbaIrqDelayCycles fix was inert because the bug is IRQ
    *count/acknowledgment*). `cycles_elapsed` in the probe is a red herring: the
    recomp's counter tallies only halt-path cycles (pump_idle) while the interp
    pumps a fixed 280896-cycle quantum — incomparable by construction.
  - **What changes at f40:** `FadeOutBody` first runs (a fade/song event), which
    brings a new interrupt source live (MP2K programs Timer + sound-FIFO DMA;
    candidates: a timer-overflow IRQ or a DMA IRQ). The recomp evidently fails to
    *acknowledge/clear* that source's `IF` bit (or mishandles a handler that
    re-enables IRQs for nesting — MP2K's sound IRQ handler does re-enable), so it
    re-fires. The interpreter (`enter_irq` returns to the main step loop, which
    re-checks `irq_pending` per instruction with `IF` already write-1-cleared)
    does not. The recomp vectors `runtime_irq → runtime_dispatch(0x18)` which
    runs the WHOLE handler chain *recursively* to completion before returning;
    the interaction of that recursive delivery with `IF` acknowledgment / a
    handler that re-enables IRQs is the prime suspect.
  - **NEXT (pin the source):** identify WHICH `IF` bit runs away — break the
    recomp's IRQ count down by source (log `IF`/`IE` at each `runtime_irq`, or
    dump RUNTIME_TRACE_IRQ events from the always-on ring across f40–f46), then
    compare recomp vs interp `IF` acknowledgment for that source. Fix is in the
    runtime IRQ-delivery / `IF`-clear path (or the IO write-1-to-clear for `IF`),
    NOT in generated code. Validate: `diff_counters.py` must show R=I IRQ deltas
    past f40 and `track_bytes` must stay d=0 / not spin.
  - **▶▶▶ SOURCE PINNED — the storm is nested VCount re-delivery.** Wired
    `runtime_irq` to record the active source `(IE & IF)` in the trace addr
    field; raised the TCP `runtime_trace` cap 512→4096; `oracle/irq_sources.py`
    histograms IRQ events from the ring. `oracle/diff_io.py` confirms
    **IE = 0x2005 (VBlank+VCount+GamePak) is IDENTICAL on both engines all the
    way through** — no new source enabled at f40; DISPSTAT enables VCount IRQ at
    LYC=0x50 (scanline 80). Baseline f30/f38/f39 (game HALTING): exactly
    1 VBlank + 1 VCount/frame, both interrupting the BIOS halt-wait at
    `ret=0x348` (System mode) — clean, fully serialized (handler completes,
    game re-halts, next IRQ from the halt loop). Storm f40+ (game BUSY through
    the transition, halt_steps→0): captured IRQ events are **VCount, nested,
    interrupting game code in System mode (`cpsr&0x1f==0x1f`)**.
  - **MECHANISM (why only transitions, why only busy frames):** while halting
    (f1–39) IRQs are serialized — each handler finishes and the game re-halts
    before the next IRQ, so no nesting. At a transition the game does too much
    work to idle-wait, so it runs continuously (busy); the long VBlank/m4a
    handler switches to System mode and RE-ENABLES IRQs (MP2K does this for
    sound), so **VCount nests inside it**. The recomp delivers IRQs
    *recursively* — `runtime_irq → runtime_dispatch(0x18)` runs the ENTIRE
    handler synchronously on the host C stack, re-checking `irq_pending` after
    every `runtime_tick` — and **storms on the nested VCount** (3→4→…→13/frame
    → spin). The interpreter delivers *flat*: `enter_irq` only sets up the entry
    and returns to the main step loop; nesting happens via guest CPSR/SPSR
    banking + the IRQ stack exactly like hardware, so it does NOT storm. This is
    the general transition-hang root (explains MC-HP-002 across every transition,
    and MC-HP-003 garble as its downstream presentation artifact).
  - **FIX DIRECTION (next, core runtime — NOT generated code):** make the
    recomp's nested-IRQ delivery match hardware/the interpreter. Candidates, to
    be chosen after checking mGBA's IRQ model: (a) don't re-vector the same
    unacked source — gate re-delivery so a pending bit isn't taken again until
    the handler acks it / the source re-asserts (proper edge vs the recursive
    per-tick re-check); (b) restructure delivery so a nested IRQ doesn't grow the
    host C stack unbounded (flat trampoline / bounded nest depth). MUST re-verify
    the BIOS intro gate after (the change touches the IRQ-timing model). Validate
    with `diff_counters.py` (R=I IRQ deltas past f40), `track_bytes` (no extra
    decrement, no spin), and a full transition play-through.
  - **▶▶▶▶ CORRECTION (same session) — the "recursive IRQ storm / nesting"
    framing above is WRONG; IRQ over-delivery is a SYMPTOM of over-long game
    code, not the cause. Read this; it supersedes the SOURCE-PINNED/MECHANISM/
    FIX-DIRECTION bullets' nesting claim.** Added live IRQ nesting-depth tracking
    (`g_irq_nest_depth` ++/-- around the handler in `runtime_irq`, depth in the
    trace `aux`) and an env gate `GBARECOMP_ABORT_ON_IRQ_DEPTH=N` (dumps the ring
    + aborts when depth reaches N). With the gate at **2 AND 3, it never fired
    through f47** — IRQ nesting depth stays at **1** (no IRQ is ever taken while
    a handler runs). So there is NO recursive nesting and NO re-entrant storm;
    the recursive-delivery hypothesis is refuted, and the IRQ-delivery-model
    "fix direction" above is moot.
  - **What the 13 IRQs/frame ACTUALLY are — a step-semantics artifact of
    over-long game code.** The recomp's TCP `step` (step_frame) is
    FUNCTION-GRANULAR: `step_once` runs one whole `runtime_dispatch` and only
    then checks `g_runtime_vblank_starts`. So if a single dispatched game
    function over-iterates for ~6–7 PPU-frames, the PPU advances ~6–7 frames
    *inside that one step_once* and delivers ~13 IRQs (each depth-1, flat) before
    step_once returns. The interpreter's `step` is CYCLE-CAPPED (pumps a fixed
    280896-cycle quantum and can stop mid-function), so it never overshoots and
    reports ~2 IRQ/frame. The growth f40(3)→f46(13) is a game function iterating
    progressively LONGER each frame — the runaway building toward the f48
    infinite loop (the anim/m4a walker on diverged data). The IRQ over-delivery,
    the m4a-countdown "extra decrement", and the "1-frame lead" are ALL downstream
    readouts of this over-iteration + the function-granular-vs-cycle-capped step
    mismatch — NOT independent bugs.
  - **CORRECTED ROOT (still open):** a game function over-iterates on the recomp
    starting ~f40 and grows to an infinite loop by ~f48 (interp never reaches
    that state — diff_anim: recomp entity animIdx→0 at f39, interp stays 323), so
    the recomp genuinely diverges in DATA/control-flow before the walker. NOT an
    m4a-chain miscompile (audited clean), NOT IRQ phase, NOT IRQ nesting. The
    frame-granularity oracle diffs (track_bytes/diff_iwram/diff_counters) are
    CONFOUNDED once functions run long, by recomp(function-granular) vs
    interp(cycle-capped) `step`.
  - **GENUINE NEXT STEP:** add a recomp CYCLE-CAPPED step mode (yield via the
    existing per-instruction `runtime_should_yield` when a cycle budget is spent
    — feasible since break_pc already yields mid-function) so the recomp can be
    compared to the interpreter at IDENTICAL cycle/event points; then event-align
    both at the f40 m4a/VBlank handler entry (a hardware event, identical state)
    and find the FIRST real register/memory divergence (the function whose loop
    over-iterates / the data feeding it). That first divergence — not the IRQ
    counts — is the root. Re-audit THAT function vs ROM.
  - **Tooling landed this session (kept):** `gba::kIrqWakeDelayCycles` shared
    constant + `runtime_tick`/`tick_devices` refactor (correct parity, inert on
    this bug); `g_runtime_irq_entries` wired through TCP `counters`;
    `oracle/diff_counters.py`; the m4a static-audit notes above.
- **★ CORRECTED AGAIN 2026-05-28 (session 2) — supersedes BOTH the
  "1-frame-ahead / entity-animation" and the older "M4A" writeups below.
  The real earliest divergence is in the M4A SOUND engine at frame 40; the
  animation-walker spin is a DOWNSTREAM symptom.**
  - **A harness bug invalidated the prior analysis.** The recomp's TCP `step`
    (runtime.cpp `step_frame`) stopped at scanline-WRAP (`ppu.frame_count()`
    change, 227→0) while bios_smoke `step_one_frame` and mGBA `runFrame` stop
    at VBlank-START (159→160) — 68 scanlines apart. So at the same step index
    the recomp had already run that frame's VBlank IRQ + user VBlank handler
    and the interp had not → the "recomp runs ~1 frame ahead" was an artifact.
    `oracle/diff_anim.py` only watched `entity+0x12`/`gPriorityHandler`, which
    miss this entirely. FIX (gbarecomp): added `g_runtime_vblank_starts`
    (runtime_bus_bridge.cpp, incremented on `events.vblank_started`);
    `step_frame` stops on its increment → recomp now parks at VBlank-start like
    both oracles (verified: both vcount=160, identical frame#). 12/12 ctests
    green (codegen `tests/codegen/stubs.cpp` also defines the counter).
  - **Phase-aligned full-IWRAM+EWRAM diff (new `oracle/diff_iwram.py`):** game
    state is BYTE-IDENTICAL on recomp vs interp through frame 39 (the only diff
    is a constant ~16-byte VBlank-handler "baseline" = recomp samples after the
    user handler `tfunc_08016B92`/`DispCtrlSet`+DMA+gMain+0; interp at IRQ-entry
    pc=0x128 — NOT real divergence). **At frame 40 the game state really
    diverges**, and the first divergent writes are in **M4A**: a frame-gated
    watchpoint (new `GBARECOMP_ABORT_ON_MEM_WRITE_MIN_FRAME`) on the
    sound-channel field `0x03004470` caught `tfunc_080AF976+0x10` (0x080AF986)
    inside `FadeOutBody`(0x080B0874) → `tfunc_080AF924`(recursive) →
    `TrkVolPitSet`/`ClearChain`, with the EWRAM divergent addr `0x020381A0`
    live in r5. Off-by-one signature: recomp 0x0b vs interp 0x0c at channel+0x10
    and at EWRAM 0x020381a0. The sound work area 0x03004460+ was identical
    f1..f39, so this is a genuine f40 m4a divergence, not the VBlank sample
    offset. The anim-walker spin at 0x08004286 (~f46) is the downstream result.
  - **It IS a recompiler bug:** bios_smoke runs 150 frames hold-Up from the SAME
    state3 with NO spin. So the prior "entity-animation not M4A" correction was
    itself wrong — the M4A engine (frame 40) is upstream of the animation hang.
  - **OPEN:** pin the exact m4a defect — compare recomp vs interp at the m4a
    function entry in frame 40 with identical inputs (recomp `set_break_pc` +
    interp `step_inst`-to-PC; `runtime_dispatch` is function-granular so no
    instruction lockstep). Likely an off-by-one shift/carry/round in an m4a
    mixer/fade calc. New tools: oracle/diff_iwram.py, phase_probe.py,
    watch_addrs.py. (gbarecomp memory: project-mc-hp-002-not-cycle-undercount,
    reference-diff-iwram-phase.)
- **⚠️ CORRECTION 2026-05-28 — this is the ENTITY-ANIMATION system, NOT
  the M4A sound engine. The 2026-05-27 "M4A / null song 0" diagnosis below
  was a MISDIAGNOSIS** (the prior session pattern-matched the table-indexing
  idiom to an MP2K song table and built diff_m4a.py + the whole sound
  narrative on it). After landing decomp-symbol names into the runtime
  trace (new permanent fixture — see below), the named call chain at the
  spin reads:
  `HandlePostScriptActions → ExecuteScriptForEntity → HandleEntity0x82Actions
  → sub_0807DE80 → InitAnimationForceUpdate → (0x08004260 resolver) →
  UpdateAnimationVariableFrames (spin PC 0x08004286)`. The struct at
  `0x030018D0` is an ENTITY, not an M4A track; `[+0x12]` is its **animation
  index**; the table at `*(0x0800439c)=0x080029B4` is the **animation table**
  (`table[0]=NULL`, `table[1..]=0x08007498` = real animation data; cf. the
  `gBgAnimations` symbol); `[+0x5c]` is the **animation frame-data pointer**.
  The mechanism shape from 2026-05-27 still holds (index 0 → null table
  entry → garbage frame pointer → the variable-frame walker accumulates
  duration bytes from random memory until it goes positive → spin), but
  everything labeled "song"/"track"/"M4A"/"sound" should read
  "animation id"/"entity"/"animation". `diff_m4a.py` is built on the wrong
  premise; keep it only as a generic state-injection harness.
- **NEW open question (corrected):** why does the entity-script system run
  `InitAnimationForceUpdate` on entity `0x030018D0` with animation index 0
  (an invalid/uninitialised animation) at this transition?
- **FULL CAUSAL CHAIN 2026-05-28d — localized to a ~1-FRAME TIMING SKEW that
  exposes a priority-paused cutscene object to a force-animate.** Deep named
  trace (GBARECOMP_TRACE_DUMP_DEPTH=4000) gives the path:
  `ObjectUpdate(0x080174A4) → EntityDisabled(0x0805E3B0) → [guard] → table[id]
  handler via _call_via_r1 → CutsceneOrchestrator(0x08094A0C) →
  InitScriptForNPC(0x0807DD50) → ExecuteScriptAndHandleAnimation → … →
  InitAnimationForceUpdate → UpdateAnimationVariableFrames (spin)`. The
  spinning entity is a **priority-1 cutscene object (id 0x69)**. `ObjectUpdate`
  guards the handler call with `EntityDisabled`: it returns "disabled" when the
  global **`gPriorityHandler`** (0x03003DC0, via literal 0x0805E3DC; plus a
  `gMessage`@0x02000050 clamp) exceeds the entity's priority level
  (`entity[0x11]&0xF` = 1) — i.e. a cutscene/transition raises `gPriorityHandler`
  to PAUSE low-priority background objects. `diff_anim.py` (recomp vs interp
  from state3+Up) shows the smoking gun: the recomp runs **~1 frame AHEAD** —
  it frees the old entity at f39 (interp f40) and raises `gPriorityHandler`
  0→0x07 at f40 (interp f41). That 1-frame phase offset lets the recomp update
  the cutscene object in a frame/order where `gPriorityHandler` is still low
  (object NOT yet paused) **and** its animation isn't set (animIdx 0) → force-
  animate → spin; the interpreter, a frame behind, has the object paused by the
  time it would animate, so it never force-animates it. This matches the prior
  session's deprioritized "recomp frame_count ahead of interp" observation —
  so the symptom-level FIX is in the runtime TIMING model (why the recomp
  advances ~1 frame ahead through a transition): wake-from-halt IRQ delivery
  phase (bios_smoke applies kGbaIrqDelayCycles=7; the recomp delivers
  immediately) and/or frame-boundary/cycle pacing are the prime candidates,
  now promoted from "parity nicety" to "the cause of this hang". NEXT: confirm
  the skew is causal by aligning the recomp's IRQ-delivery/frame phase to the
  interpreter and re-running diff_anim (expect the spin to vanish), then fix
  the timing source in the runtime.
- **ROOT CLASS FOUND 2026-05-28c (interpreter oracle) — the recomp
  FORCE-ANIMATES A FREED entity that the interpreter correctly SKIPS. It is a
  recompiler control-flow divergence, NOT a missing anim-id write.** Built an
  interpreter-as-oracle path: `bios_smoke` now restores a runtime GBAS
  savestate into the interpreter (maps the recomp `g_cpu` → interpreter
  `CPUState`, drops the recomp host call-stack, skips the ROM-SHA gate). New
  `oracle/diff_anim.py` runs the recomp (19842) and the interpreter (19844)
  from the same `state3`, holds Up, and diffs `gEntities[0x030018D0]+0x12`
  each frame. Result:
  ```
  @load  both = {animIdx=323, kind=6}  IDENTICAL
  f1-38  both animIdx=323 (entity alive, animating normally)
  f39    recomp -> {animIdx=0, kind=0, ALL-ZERO}   interp -> still {323,kind6}  DIVERGE
  f40    interp also zeroes (entity freed one frame LATER)
  f47    recomp SPUN                                 interp runs fine to f54+
  ```
  So the entity is deleted/zeroed (kind=0) on BOTH — but the recomp (a) frees
  it ~1 frame EARLY and (b) then runs the animation path on the dead kind=0
  slot (`animIdx=0` -> `animTable[0]`=NULL -> spin), while the interpreter
  NEVER animates the freed slot. Since both execute the same ARM, the recomp
  diverges in control flow: it processes an entity through
  `ExecuteScriptAndHandleAnimation` (0x0807DD94, called from ~20 per-entity
  update sites) that the interpreter's iteration skips. The dead-entity
  skip-guard lives in the higher entity main-loop (iterates `gEntities`,
  dispatches per-entity updates). NEXT: localize the exact divergent branch —
  either a mis-translated kind==0/active guard, or an ordering/timing skew
  (the ~1-frame-early free is consistent with the known recomp frame-count
  skew) that exposes the slot to animation in the same frame it is freed.
  This supersedes the "missing +0x12 write" framing below (the write isn't
  missing — the entity is simply dead when animated).
- **NARROWED 2026-05-28b — the slot is FREED + REALLOCATED during the room
  transition, and the new entity's anim index `[+0x12]` is never set.**
  `0x030018D0` is an entry in `gEntities` (0x030015A0); `gRoomTransition`
  (0x030010A0) is active. Watching the word-aligned `0x030018E0` (covers
  `+0x10..+0x13`) fires at `<tfunc_0805E818+0x6>` writing `+0x10=1` (a byte),
  immediately after `zFree` (0x0801DA0C) with `r4=0x030018D0`. That PC is
  inside **`DeleteEntity`** (0x0805E7BC-0x0805E84B): the slot is being DELETED
  (`+0x10=1` = deleted flag, then `zFree`) at trace seq ~#265071, yet the SAME
  slot is force-animated via cmd 0x82 (`HandleEntity0x82Actions`) ~300k events
  later at the spin (#565696). The `+0x12` anim-index halfword is NEVER written
  by a traced store in the 46f window (both the `0x18E2` and the first `0x18E0`
  watchpoints confirm), yet it reads 323 at load and 0 at the spin: zeroed by
  the delete/free block-init and then NOT re-populated before the
  force-animate. Smells like a **use-after-delete / slot-reuse ordering
  divergence** — a deleted (or freshly re-created) entity is run through the
  script/animation path with a stale/zero anim id. Lifecycle funcs are now all
  named: GetEmptyEntity, DeleteEntity, DeleteThisEntity,
  ClearAllDeletedEntities, ExecuteScriptForEntity, HandleEntity0x82Actions. Since the resolver `0x08004260` has no
  null-index guard, hardware cannot be reaching it with index 0 (the game
  runs) — so the recomp DIVERGES: it force-animates a freshly-realloc'd
  entity whose anim id is still 0. Next: identify the `0x0805E8xx`
  alloc/init function + the cmd-0x82 (`HandleEntity0x82Actions`) path; find
  where a valid anim id should be written to `[+0x12]` and why the recomp
  leaves it 0 (a skipped/mis-ordered init, or a wrong script pointer feeding
  cmd 0x82). The interpreter/mGBA oracle at this point would confirm the
  intended `[+0x12]`.
- **PERMANENT FIXTURE LANDED 2026-05-28 — runtime decomp-symbol names.** This
  bug is the proof case for it: a whole prior session was lost to a wrong
  "sound" narrative for want of names. The recompiler now emits
  `generated/symbol_map.cpp` (sorted address→name for all ~44k functions,
  decomp names where seeded; `gba_recompile --no-symbol-map` to omit), which
  self-registers into `src/armv4t/symbol_lookup.cpp` (`gba_symbol_lookup`).
  Every PC in `runtime_trace` dumps, dispatch-miss reports, and the watchpoint
  abort now prints `<Name+0xoff>`; new TCP `symbol {addr}` command resolves on
  demand. MinGW-safe (static-init registration, not weak externs). BIOS map
  (`<0x4000`, separate binary) is a documented follow-up.
- **ROOT-CAUSED 2026-05-27 — a busy-loop in the M4A SOUND ENGINE, i.e.
  a sound data/state DIVERGENCE, NOT raw recompiler slowness.** A
  guest-PC sampling profiler (background thread sampling `g_cpu.R[15]`,
  added as `GBARECOMP_SAMPLE` tooling in `runtime_bus_bridge.cpp`) on an
  overworld screen-scroll transition (state3 + Up) showed **~95% of the
  ~90s is spent at PCs `0x08004286` / `0x0800428C`** — a tiny loop. The
  enclosing routine at `0x08004260` is M4A track setup (stores a track
  id at `[r0+0x58]`, indexes the song table at literal `0x0800439C` by
  `[r0+0x12]`, stores the resulting stream pointer to `[r0+0x5c]`); the
  loop at `0x08004284` is the **sequence/duration walker** — reads bytes
  from the track stream `[r0+0x5c]`, accumulates `r2`, advances `r1+=4`,
  loops on a bit7 continue/wrap flag, exits when `r2 > 0`. It iterates
  ~tens of millions of times where a sound update is a few hundred → the
  walker is reading wrong/garbage track data (bad song index `[r0+0x12]`
  or bad stream pointer), so it over-walks until `r2` accidentally goes
  positive. The single sound call doesn't return, so the PPU clock
  advances ~445 frames "inside" it (per-instruction ticks) and the host
  pump starves → "Not Responding"; it "recovers" when the walk finally
  exits. This is the same class as the intro/Zelda-entry freezes (all at
  music/sound transitions).
- **Every performance hypothesis was WRONG (disproven by measurement,
  each ~88s, unchanged):** -O0 vs `-O3 -DNDEBUG`; the dispatch-table
  binary search (→ O(1) hash index); per-instruction device ticking (→
  lazy/batched `runtime_tick`); the always-on trace ring
  (`GBARECOMP_NOTRACE`); and the per-instruction call "barriers" (→
  inlined `runtime_tick`/`runtime_should_yield`). None moved the needle
  because the cost is **iteration count in a divergent loop**, not
  per-op speed. The sampler is what finally localized it — keep it.
- **Likely tie-in: the IWRAM-copied sound engine** (`sound_main_ram`
  `[[code_copy]]`). If the sound driver code or its state/data in IWRAM
  isn't set up correctly (wrong copy, wrong song-table base, stale
  pointer at `[r0+0x5c]`/`[r0+0x12]`), the walker reads garbage. The
  M4A song table is at literal `0x0800439C`.
- **CONFIRMED 2026-05-27 — the corrupt value is the M4A track stream
  pointer.** Added a TCP `set_break_pc` command (runtime_should_yield
  unwinds the dispatch when guest PC == target; one-shot, since the
  post-break PC is mid-function and re-dispatch would miss). Broke at
  `0x08004286` during the transition: sound track struct at IWRAM
  **`0x030018D0`**, `songidx[+0x12]=0`, `[+0x58]=0`, `[+0x59]=0`, and
  **`trackptr[+0x5c]=0x00000004`** (garbage — points into the BIOS/null
  region). So `r1=0x4`, the walker reads bytes from `0x5…` and
  accumulates garbage → spins. The corruption is `[0x030018D0+0x5c]`.
- **TRACED 2026-05-27 — the engine is processing song 0 (a NULL song)
  and walking its garbage track.** Caught the writer with
  `GBARECOMP_ABORT_ON_MEM_WRITE_ADDR=0x0300192C`: it's `pc=0x08004270`
  (`str r1,[r0,#0x5c]`) inside the M4A track setup at `0x08004260`, fed
  by `ldr r2,[songtable,songidx<<4]` at `0x0800426A` returning **r2=0**
  for **songidx=0**. The song table base is `0x080029B4` (from literal
  `0x0800439C`), and **ROM `[0x080029B4]` is genuinely all-zero** (the
  first 16 bytes are 0; real song entries begin at `0x080029C4`). So
  song 0 = "no song"/null and the recomp read the table CORRECTLY — it
  is NOT a memory/codegen read divergence. The bug is upstream control
  flow: the engine runs track-setup for song 0 and then walks
  `[null track]` → reads address ~0 (BIOS) as sequence data → spins.
- **Open question for the fix (needs the mGBA oracle):** on hardware,
  does the M4A engine reach this setup/walker for song 0 / an inactive
  channel at all? Either (a) the channel/track should be marked inactive
  and skipped (a flag in the `0x030018D0` struct the recomp sets wrong),
  or (b) `songidx`/the song-load on transition should be a real song
  number, not 0. Next: diff the M4A SoundInfo/track state
  (`0x030018D0…`) and the control path into `0x08004260` against mGBA at
  the same transition; trace how this channel got marked active with a
  null song.
- **Tooling added (kept, gated/zero-overhead):** `GBARECOMP_SAMPLE`
  guest-PC sampler + a TCP `set_break_pc` command (one-shot unwind at a
  guest PC, since the spin lives inside one `runtime_dispatch`).
- **Re-confirmed 2026-05-28 — root UNCHANGED after this session's
  per-instruction-cycle regen; the spin is byte-for-byte the same.**
  Added `MinishCapRecomp/tools/repro_hang.py` — a from-reset TCP driver
  that (a) navigates title→Start→A→A→idle with screenshots, or (b) with
  `--state <p> --hold up` loads a savestate and holds a key, detecting the
  spin by `step` wall-clock timeout (the single-threaded server blocks for
  the spin's duration), or (c) `--inspect` arms `break_pc` at the walker
  and dumps the M4A struct at the song-start park. Fast dev repro:
  `python tools/repro_hang.py --state roms/minishcap_usa.state3 --hold up`
  → `state3` is the overworld; holding Up scrolls a screen → song-start →
  **HANG at frame 46.** `--inspect` parks at `0x08004286` frame 46 with
  `sel@12(songidx)=0, f58=0, f59=0, cmdptr@5c=0x00000004, flags@0=0x10`,
  regs `r0=r4=0x030018D0 (track), r1=0x4 (walked ptr), r2=0xffffffff
  (accumulator), r5=0`. Identical to the 2026-05-27 finding. NOTE: blind
  menu-timing navigation is fragile (the title only accepts input during
  the blinking "PRESS START" window + an attract cycle); use the savestate
  repro for the dev loop. The canonical user path is still
  title→Start→A→A→idle, which hits the same spin at the intro→overworld
  load and at the Zelda-enters-house transition.
- **MECHANISM fully disassembled 2026-05-28 (Ghidra, cart).** `0x08004260`
  is a per-track command-pointer resolver + duration walker (THUMB):
  ```
  08004260: strb r1,[r0,#0x58]      ; track[0x58] = r1
  08004264: ldrh r3,[r0,#0x12]      ; r3 = track[0x12]  ← THE INDEX
  08004266: lsl  r3,r3,#4           ; ×16  (16-byte table stride)
  08004268: ldr  r2,[0x0800439c]    ; r2 = *(0x0800439c) = table base
  0800426a: ldr  r2,[r2,r3]         ; r2 = table[index]   (table[0] = NULL)
  0800426e: ldr  r1,[r2,r1<<2]      ; r1 = *(NULL + r1*4) = garbage
  08004270: str  r1,[r0,#0x5c]      ; track[0x5c] = garbage cmd ptr
  ...walker 08004284..0800429a (spin PC 08004286):
  08004286: ldrb r3,[r1,#0x1]; add r2,r2,r3; bgt exit; ldrb r3,[r1,#3];
  add r1,#4; lsr r3,#8 (carry=bit7); bcc loop; else wrap r1-=[r1]<<2.
  ```
  So the **root variable is `track[0x12]`** (the per-track song/voicegroup
  index): it reads **323 at `state3` load, 0 at the spin**. `table[0]` is
  NULL, so `track[0x5c]` becomes a ~null pointer and the walker accumulates
  `r2` from random bytes until it goes positive → tens of millions of
  iterations.
- **The zeroing of `track[0x12]` is NOT a halfword store** — watchpoint
  `GBARECOMP_ABORT_ON_MEM_WRITE_ADDR=0x030018E2` (=track+0x12) NEVER fires
  during the scroll, while the cmdptr watchpoint `0x0300192C` (=track+0x5c)
  fires at `0x08004270` as expected. So `track[0x12]` was zeroed by a wider
  / block (re)initialization of the track struct (word store to an aligned
  base, DMA, or BIOS CpuSet), i.e. **the engine re-initialized this track**
  rather than a stray halfword poke.
- **Call chain at the spin (trace ring):** the resolver is reached entirely
  from INSIDE the M4A engine's per-track processor —
  `0x0807DE80 → 0x080042AC(push lr; bl 0x08004260)` — operating on track
  `0x030018D0` with `r1=0`. It is NOT a game-level "play song 0" call; the
  engine itself is processing this track with index 0. Outer frames sit in
  `0x0807Cxxx`/`0x0807Dxxx`/`0x0807Exxx` (the sound engine in ROM).
- **SHARPENED open question (needs oracle or decomp-symbol mapping):** why
  does the engine process track `0x030018D0` with `track[0x12]==0` here?
  (a) the track should be marked inactive/skipped (a status/enable flag the
  recomp sets wrong → engine walks a track it shouldn't), or (b) `track[0x12]`
  should hold a real index and an upstream divergence zeroed it. Resolving
  this needs either the mGBA oracle at the same transition (does hardware's
  engine touch this track / with what index?) or mapping `0x08004260` /
  `0x0807DE80` and the `track[0x12]` field via the zeldaret/tmc decomp
  (MP2K). Decomp symbol names are NOT currently in the Ghidra DB.
- **Disposition of the perf changes (uncommitted):** O(1) dispatch
  index, lazy `runtime_tick`, inlined hooks + halt mirror, guest-PC
  sampler. They are correct *general* improvements but do NOT fix this
  issue and the lazy-tick/inline-hooks touch the timing model (BIOS-gate
  risk, unverified). Decide whether to keep (with BIOS-intro re-verify)
  or revert to keep the tree clean while the real sound-engine fix is
  pursued. The sampler is worth keeping as gated tooling.
- **Priority:** high — UX-breaking on every music/sound transition.

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
- **REFRAMED 2026-05-28 — almost certainly a DOWNSTREAM CONSEQUENCE of
  MC-HP-002, not an independent PPU bug (user's own read, now supported
  by the mechanism).** The garble appears *while the MC-HP-002 hang frame
  is on screen* (ISSUES already noted "The application also hangs at the
  moment the garbled frame is on screen"). During the M4A null-song spin
  the CPU is frozen inside ONE `runtime_dispatch` (the song-start), yet
  per-instruction `runtime_tick`s keep advancing the PPU clock ~445
  frames "inside" that single call — so the PPU keeps latching/presenting
  while the game's room-load code (the VRAM/tilemap/PAL/OAM rewrite + its
  DMAs) is BLOCKED mid-sequence. Result: we present the half-written VRAM
  (Image: shredded tiles, magenta/green blocks) until the spin finally
  exits and the load completes (destination renders cleanly). **Predicted:
  fixing the MC-HP-002 spin (the animation walker — see the 2026-05-28
  correction there) removes this garble.** Validate by
  re-checking the house-entry transition once MC-HP-002 is fixed; only if
  garble persists with no hang is there a separate PPU-latch bug to chase.
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
