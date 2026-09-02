# Local Astral Plane asset

Twilight Visuals reads `res/music/astral-plane.ast` beside the executable. The
existing resource-copy build step packages the source `res/music` directory.
The recording is ignored by Git; keep the `res` folder with your local build.
No MP3 decoder, Windows audio resource, or secondary SDL mixer is used at runtime.

`mp3_to_ast.cpp` is an offline converter. Compile it as a standalone C++ program
(for example, `cl /O2 /EHsc mp3_to_ast.cpp`), then run:

```
mp3_to_ast input.mp3 astral-plane.ast
```

Copy the result into the repository's `res/music` directory before building.
The output is native big-endian stereo PCM16 AST, with 0x2760-byte channel blocks
matching `JAUInitializer`. It retains the original sample rate and loops from
sample zero to the recording's final sample. This does not remove any existing
fade or silence in the recording or author a new seamless musical loop.

The game uses `JASAramStream`'s normal ring buffer, looping, channels and DSP.
Only its source reads are redirected to the retained host AST asset; normal DVD
streams continue to use their original read path. A separate ring buffer leaves
the vanilla cutscene stream allocation available. Missing or invalid assets fall
back to Palace music. Music eligibility remains controlled by Twilight Visuals.
The Twilight Visual Style selects its matching music automatically: Normal and
Black and White use Palace music, while Astral Plane and The Dark Hour use their
native tracks. Once started, custom music keeps its native loop advancing at zero
volume while interrupted or unselected; choosing it again does not restart it. Selection fades are exclusive
(fade out, silent handoff, fade in), not overlapping crossfades. Palace sequences
are tagged by their owning sequence and gain-controlled directly in their native
channel callbacks, after cached sequence parameters, without stopping their notes.
Interruptions gate Astral audibility separately from its selected track, so a
battle or load does not reset the selection and briefly restore Palace music.

`test_twilight_music_fade.cpp` is a standalone regression test for the exclusive
gain envelopes, rapid reversals and protected-music handoff.

The converter's `dr_mp3.h` comes from mackron/dr_libs (v0.7.4 header); its licenses
are included in the header. It is a build-time tool dependency only.

## Combat variant

Convert the supplied combat recording with the same tool and place it at
`res/music/astral-plane-combat.ast`. It is also ignored by Git and copied with
the local build's resources. In Astral Plane mode, ordinary normal/Twilight
enemy-battle sequences select this track; unique boss, miniboss and story themes
are not replaced. The native battle sequences are gated at channel output.
Missing/invalid combat data retains vanilla combat music.

Each native AST has its own ring buffer and playback position. Both keep
advancing silently after starting. Exploration/combat handoffs use the existing
two-second exclusive fades, and both recordings use the 0.85 volume multiplier.

## The Dark Hour

`res/music/dark-hour.ast` is converted with the same native tool and is selected
by The Dark Hour visual style. It keeps advancing silently while inactive, uses the
same scene/load/fanfare/cutscene/boss/speedrun restrictions, and yields to the
game's ordinary battle music because no separate Dark Hour combat asset exists.
