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
- **Next step:** add `extra_func 0x0804B208` (mode TBD — likely
  `thumb` given surrounding ARM/THUMB context shown in trace) to
  `symbols/minishcap.toml`, regenerate, rebuild. Then run again
  and walk the same path; check `dispatch_misses.log` for the next
  follow-on miss and iterate until the doorway transition completes.
  Once a clean traversal exists, audit the function-finder pass to
  understand why this entry was missed (jump-table coverage? IT
  block continuation? IWRAM copy?) so we don't accumulate a long
  tail of manual entries.

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

### MC-HP-004: Gameplay runs at roughly 2x real-time
- **Observed:** 2026-05-25. Entire run (BIOS intro included,
  best estimate) plays back at approximately double speed —
  Zelda walks too fast, dialog auto-advance feels accelerated,
  audio is shifted up.
- **Detail:** Distinct from `gbarecomp/ISSUES.md` LP-001
  ("BIOS intro tempo plays slightly fast"). That issue was
  ~sub-1% drift on the BIOS chime; this is integer-multiple
  fast.
- **Suspected cause:** the host frame loop has no real-time
  pacing — `step_frame` now drives dispatches until PPU
  `frame_count` advances and then immediately presents, with no
  wall-clock cap (only SDL audio backpressure regulates rate).
  If the audio device's queue stays under the threshold that
  blocks `push_audio_samples`, the loop runs as fast as the host
  CPU can dispatch. Tonight's frame-pacing change in `29e81ac`
  is the most likely cause; previously the 1-dispatch-per-frame
  cap was unintentionally rate-limiting.
- **Priority:** high — every other timing-sensitive bug
  (MC-HP-002 hangs, MC-HP-003 transition garbling, audio
  artifacts) reads differently at 2x and may mask root causes.
- **Next step:** add wall-clock pacing keyed to the GBA's
  16.78 MHz cycle budget (or the 59.7 Hz frame rate). Either
  sleep to a deadline at frame-present time or gate
  `push_audio_samples` so SDL's queue stays the controlling
  limiter at the correct rate. Verify with a stopwatch against
  mGBA over 30 seconds of overworld play.

### MC-HP-005: Save states are not implemented
- **Observed:** 2026-05-25. User flagged the absence as a
  debug-loop blocker — every bug repro currently requires
  walking back through the BIOS intro + stained-glass + Zelda
  cutscene from cold start, which makes the other four
  high-priority entries each cost several minutes per attempt.
- **Detail:** EEPROM save persistence (the SRAM-backed save
  file the game itself writes) shipped in `29e81ac`. Save
  *states* (snapshot of full machine state — CPU regs + IWRAM
  + EWRAM + PAL + VRAM + OAM + I/O + PPU + audio + bus state +
  current PC) are a separate, host-driven feature.
- **Priority:** high — promoted from medium 2026-05-25 because
  it is the multiplier on every other high-priority entry.
  Investigation cost on MC-HP-001 (door crash), MC-HP-002
  (cutscene hangs), and MC-HP-003 (transition garbling) all
  drop sharply once a snapshot taken near each repro point
  exists.
- **Next step:** design a single binary snapshot format that
  versions cleanly, dumps every host-owned state buffer, and
  restores it without re-running the BIOS. Wire to a TCP
  `savestate_save` / `savestate_load` command first (debug
  workflow gets the most leverage here); hotkey on the host
  window second. Recommended to do this *before* digging into
  MC-HP-001..004 so each of those gets the loop-tightening
  benefit.

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
