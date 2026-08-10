# Zelda 1 overworld music renderer

`Zelda1OverworldMusicRenderer` is a direct, bounded translation of
`Z_00:DriveAudio` / `DriveSong` for song `$01`. It interprets phrase headers,
cue bytes, period table and custom envelope from the in-memory `VerifiedPrg`;
it does not emulate a 6502 and carries no ROM, song, PCM, or WAV data.

The renderer outputs 65,536 Hz signed mono, advances the NTSC CPU clock as the
exact rational `78,750,000 / 44 Hz`, and clocks the 262x341-dot video cadence.
It models the two pulse channels, triangle and noise; DMC is intentionally not
implemented because this background song has no DMC request in the pinned
driver. It is instance-owned, allocation-free while rendering, resettable,
pausable, and has a fixed 79-byte strict state format. `validate_serialized`
is a no-allocation structural preflight for native save containers; `restore`
then validates the same state against the bound verified PRG and commits it
only after all checks succeed.

The focused CTest target produces
`zelda1_overworld_music_synthetic.wav` and
`zelda1_overworld_music_synthetic_spectrogram.pgm` in its working directory.
They derive from an invented synthetic PRG used only by the test, so the tree
does not distribute copyrighted Zelda song/ROM/audio data.

When `ZELDA1_PRG0_INES` names a user-supplied verified iNES image, the focused
test additionally traces the actual phrase sequence `9,10,11,12,13,14,15,10`
before writing ignored canonical WAV and spectrogram QA artifacts. Those files
are never source assets and are not tracked.

## Live Minish integration

The committed Zelda plugin registers exactly one activation-scoped pull stream
after its `Zelda1OverworldSession` has exposed its immutable, loader-owned
`VerifiedPrg`.  The callback renders at the engine's 65,536 Hz producer rate
and checkpoints the fixed 79-byte transport record through FWNS's fixed-copy
API; it performs no guest-bus write, allocation, file access, or ROM reload.

Entering the portal starts a fresh `$01` transport, mutes only audible native
PCM (the Minish M4A engine still advances), and plays the stream. Caves retain
that overworld song. Start pauses delivery and restores native gain for the
Minish menu; closing it resumes the exact transport. Explicit exit, reset, or
asset failure stops delivery and restores native gain. A saved active session
restores its transport before playback; legacy saves without the optional
record begin a fresh tune. Level 1 is deliberately outside the current music
scope: entry stops the overworld stream and restores native audio, and its
return starts `$01` from the beginning.
