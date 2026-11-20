# NetHack Modern — Roadmap

Target: Tales of Maj'Eyal–level feedback (animation + sound) on top of
NetHack 5.0, game rules unchanged.

## What 5.0 already gives us

- **Sound interface** (`include/sndprocs.h`, `src/sounds.c`): pluggable
  "soundlibs" with hooks for sound effects, achievements, instrument notes,
  ambience, user sounds and speech. Backends exist for Windows (`windsound`),
  macOS, Qt and FMOD. `SND_LIB_MINIAUDIO` is *declared* but has no
  implementation yet — that is our slot.
- **Temporary-glyph animation** (`src/display.c`): `tmp_at()` already draws
  projectile/beam paths, plus `shieldeff()`, `flash_glyph_at()` and
  `explode()` effects. These are the natural places to attach smoother
  animations.
- **`showdamage` option** (`iflags.showdamage`, used in `src/mhitu.c`): prints
  damage taken by the hero as a message. Proof that the hook points exist;
  we generalise it to all combatants and render it on the map instead.
- **Tile front ends**: `win/win32` (NetHackW), `win/Qt`, `win/X11`.

## Core design: a presentation event stream

The game core does not know about pixels. We add one small module,
`src/fx.c`, which the core calls at interesting moments, and which forwards
events to the active window port through a new optional window-proc.

```
fx_damage(x, y, amount, kind, who)   /* hit for N, on that square */
fx_miss(x, y, who)
fx_hp_changed(mon)                   /* drives health bars */
fx_death(x, y, mon)
fx_attack(from_x, from_y, to_x, to_y, attack_type)
fx_spell(x, y, spell_or_zap_type)
```

Rules:
- Only emitted for things the hero can **see** (respect `canseemon`,
  blindness, hallucination — hallucination can scramble numbers for fun).
- Pure output: never consumes RNG, never changes game state, never saved.
- A port that doesn't implement the hook ignores it (tty keeps working).

Hook sites to start with: `uhitm.c` (hero hits monster), `mhitu.c` (monster
hits hero, where `showdamage` lives), `mhitm.c` (monster vs monster),
`zap.c` / `explode.c` (wands, spells, breath), `losehp()` and the monster
death path in `mon.c`.

## Phases

**Phase 0 — Baseline build.** Build stock `NetHackW.exe` from this repo with
Visual Studio; confirm tiles and the existing `windsound` work. Tag it
`baseline-5.0`.

**Phase 1 — Event stream.** Add `fx.c`/`fx.h` and the new window-proc; wire
the hook sites above. Verify with a debug port that logs events to a file.
No visuals yet. This is the most important phase: every later feature reads
from it.

**Phase 2 — Damage numbers & health bars (win32).** In `win/win32/mhmap.c`
add an overlay layer redrawn on a timer: floating numbers that rise and fade
over ~600 ms (colour by type: physical white, fire orange, cold blue, poison
green, healing bright green, crits larger), and a thin HP bar under each
visible damaged monster. Options: `fx_damage_numbers`, `fx_healthbars`.

**Phase 3 — Sound backend.** Implement `sound/miniaudio` (single-header,
public-domain, mixes many sounds at once — unlike basic Win32 PlaySound).
Add sound effects keyed to fx events (hit, miss, crit, death, spell schools,
footsteps, doors) plus level ambience. Ship CC0/CC-BY assets with
attributions.

**Phase 4 — Animation.** Attack "lunge" (sprite nudges toward target and
back), hit flash/shake on the victim, smooth movement interpolation between
squares, particle effects for spells and explosions built on `tmp_at` paths.
Needs a frame-driven render loop in the map window; keep turn input
responsive (animations skip when you press a key, like ToME).

**Phase 5 — Polish / possible new front end.** Higher-resolution tileset,
lighting/FOV shading, UI skin. If GDI in `win/win32` becomes the bottleneck,
consider a new SDL2 or Qt-based port reading the same fx stream.

## Open decisions

- First front end: **win32** (you're on Windows, it ships with tiles) vs
  **Qt** (cross-platform, already has a Qt soundlib). Current plan: win32.
- Tileset: keep the default 32×32 tiles at first; choose a larger set in Phase 5.
- Asset licensing: only CC0 / CC-BY / OGA-licensed sound and art.
