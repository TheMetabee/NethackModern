# NetHack Modern — Roadmap

Target: Tales of Maj'Eyal–level feedback (animation + sound) on top of
NetHack 5.0, game rules unchanged.

## Status

Phases 0 to 4c are done; Phase 5 (polish) is next. The work is in the
`fx` event stream (`src/fx.c`, `include/fx.h`), the miniaudio soundlib
(`sound/miniaudio/`, samples and credits in `sound/modern/`) and the
NetHackW port (`win/win32/`, mainly `mhmap.c`, `mhstatus.c`,
`mhmsgwnd.c`, `mhmenu.c`, `mhmain.c`, `mhsplash.c`, `mhtheme.h`). Every
effect has its own `fx_*` option (see "Options" at the end).

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
events to the window port through a handler the port registers with
`fx_set_handler()`. We chose this over a new window-proc slot because a
new slot would mean editing every port's procs table, and keeping upstream
files lightly touched makes NetHack updates easier to merge.

Damage and healing aren't hooked at every one of the roughly 140 places
that change HP. `fx_sync()` runs from `flush_screen()` and compares each
creature's HP with what it saw last time, so a number shows up together
with the message it belongs to. The explicit hooks only add what a
comparison can't know: who attacked whom, misses, deaths, and the damage
kind. For debugging, setting `NETHACK_FXLOG=<file>` logs every event and
status change, with any interface.

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

## Persistent status effects ("auras")

ToME keeps a particle effect running on a creature for as long as a status
lasts, even while it stands idle: quaff speed and you get an afterimage trail,
go invisible and you shimmer. We want the same thing. One-shot fx events
can't do this, because a status is *state*, not an event. It has to survive
redraws, level changes, save/restore and the monster moving out of sight and
back again.

**Mechanism.** Every map cell already reaches the window port as a
`glyph_info` (`include/wintype.h`) through `print_glyph()`. We add one field,
`uint32 fxauras`, which is a bitmask computed in `display.c` at the moment the
glyph is mapped:

- For the hero, it comes from `u.uprops` through the existing macros
  (`Very_fast`/`Fast`, `Invis`, `Levitation`, `Reflecting`, `Confusion`,
  `Stunned`, `Stoned`, `Sick`, `Slimed`, `Hallucination`, protection
  spell, ...).
- For a monster the hero can see, it comes from `struct monst` bitfields
  (`mspeed == MFAST`, `minvis` when it's seen anyway, `mconf`, `mstun`,
  `mflee`, `mcan`, ...).

The core decides **which** statuses are active and the port decides **how**
they look. Nothing new is saved, because the mask is recomputed from real game
state on every redraw. Ports that ignore the field draw exactly what they draw
today. The usual visibility rules apply: we never show an aura the hero
couldn't perceive, so a monster's speed doesn't show unless you can see it.

When a status turns on or off, the game calls `newsym()` on that square, so
the port gets the new mask right away.

**Starter set (hero first, then monsters):**

| Status | Look |
|---|---|
| Very fast / fast | Fading afterimage copies of the sprite trailing the last move; faint speed lines when idle |
| Invisible (but seen) | Translucent sprite with a slow shimmer |
| Levitation / flying | Sprite bobs a few pixels with a soft shadow under it |
| Reflection | Occasional glint sweeping across the sprite |
| Protection spell / AC buff | Pulsing translucent ring |
| Confused / stunned | Small orbiting stars or spirals over the head |
| Stoning / slimed / sick | Colour tint creeping over the sprite (grey / green / sickly yellow) |
| Fleeing (monster) | Small sweat-drop marker |

**Budget.** Auras are the one effect that keeps running while nothing is
happening, so they bend design rule 5 below. To stay cheap, auras use a slow,
separate timer (about 10–15 fps). The timer runs only while some visible cell
has a non-zero mask, and it redraws only those cells. Turning the
`fx_auras` option off stops the timer completely.

## Design rules: animation must never slow the player

Applies to every visual and audio effect, in every phase.

1. **Never block input.** Effects play asynchronously. The game never waits
   for an animation to finish before accepting the next command.
2. **A keypress fast-forwards.** Any input instantly snaps running animations
   to their end state (numbers at final position/faded, sprites back on
   their square) before the command is processed.
3. **Multi-turn actions summarise instead of animate.** While the hero is
   running (Shift+dir, `G`), travelling (`_`), using a count prefix
   (`n20s`), resting or otherwise in `multi`/occupation mode: skip per-step
   animations and merge damage per target into one number (e.g. a single
   "-8" over the monster that interrupted you). Full animation resumes when
   control returns to the player.
4. **Everything is optional.** Each effect family (damage numbers, health
   bars, shake/lunge, screen shake, particles, sound) has its own option,
   plus an animation-speed setting.
5. **Cheap when idle.** The map's frame/timer loop runs only while an effect
   is active; otherwise the window redraws exactly as stock NetHack does.
   Persistent auras are the one exception. They run on their own low-rate
   timer and redraw only the cells that have one (see above).

## Phases

**Phase 0 — Baseline build.** *Done.* Build stock `NetHackW.exe` from
this repo with Visual Studio; confirm tiles and the existing `windsound`
work. Tag it `baseline-5.0`.

**Phase 1 — Event stream.** Add `fx.c`/`fx.h` and the new window-proc; wire
the hook sites above. Verify with a debug port that logs events to a file.
No visuals yet. This is the most important phase: every later feature reads
from it. This phase also adds the `glyph_info.fxauras` field and fills it in
for the hero and visible monsters, and the debug port logs mask changes.
*Done:* the handlers are registered with `fx_set_handler()` (display) and
`fx_set_audio_handler()` (sound) rather than a new window-proc slot;
`NETHACK_FXLOG` logs every event. The stream has since grown to cover
moves, knockback, explosions, wand and scroll effects, quaffing,
teleports, detection, the unicorn horn and more.

**Phase 2 — Damage numbers & health bars (win32).** In `win/win32/mhmap.c`
add an overlay layer redrawn on a timer: floating numbers that rise and fade
over ~600 ms (colour by type: physical white, fire orange, cold blue, poison
green, healing bright green, crits larger), and a thin HP bar under each
visible damaged monster. Options: `fx_damage_numbers`, `fx_healthbars`.
*Done.*

**Phase 2b — Status text floaters (win32).** Like ToME, when a status
lands on someone, its name briefly floats up from them the same way damage
numbers do: "Confused", "Stunned", "Blinded", "Fast", "Slowed",
"Invisible", "Levitating", "Stoning", "Slimed", "Sick", "Paralyzed",
"Fleeing", "Cancelled" and so on.

- **Source:** `fx_sync()` already compares each creature's `FXA_*` status
  bits with the last value it saw. Newly set bits become an `FXE_STATUS`
  event, which carries the gained bits and, optionally, lost bits for a
  dimmer "-Fast" / "Unconfused".
- **Look:** a smaller font than damage numbers. Bad statuses are
  red/purple, good ones gold/green. Several statuses gained in one turn
  stack upward like numbers.
- **Rules:** only for things the hero can see (and always for the hero).
  It's suppressed when a status flickers on and off within one turn, and
  merged while running. Option: `fx_status_text`.
- **Later:** the sound layer can attach a short sting per status (dizzy
  warble for confusion, and so on).

*Done,* along with "Miss", "Killed"/"Died" where the health bar was,
"Level N!" in gold and a "+N XP" floater (`fx_exp_text`).

**Phase 3 — Sound backend.** Implement `sound/miniaudio` (single-header,
public-domain, mixes many sounds at once — unlike basic Win32 PlaySound).
Add sound effects keyed to fx events (hit, miss, crit, death, spell schools,
footsteps, doors) plus level ambience. Ship CC0/CC-BY assets with
attributions.
*Done:* `sound/miniaudio/masound.c` plays samples from `sound/modern/` for
fx events, the core's `Soundeffect()` calls and matching messages (hidden
`MSGTYPE` messages stay silent); weapon sounds per skill with elemental
layers, death cries per creature type, eating by texture, traps, doors and
locks, prayer, wands, potions, scrolls and so on. No footsteps except
wading. Every file is credited in `sound/modern/attributions.txt`.
Level ambience is still to do.

**Phase 4 — Animation.** Attack "lunge" (sprite nudges toward target and
back), hit flash/shake on the victim (150–250 ms, strength scaled by damage
relative to max HP), knockback recoil, screen shake for big events
(explosions, earthquakes, crits), death shake-then-fade, smooth movement
interpolation between squares, particle effects for spells and explosions
built on `tmp_at` paths. Needs a frame-driven render loop in the map window
and drawing monster sprites separately from the floor beneath them. All of
it follows the design rules above.
*Done:* lunge, hit shake with a recoil away from the blow, death shake and
fade, screen shake (`fx_screen_shake`), knockback slides, smooth movement
(`fx_motion`), and particles (`fx_particles`) for rays, explosions,
striking, every wand, teleports, potions, scrolls and detection. Animations
keep running through the core's output delays.

**Phase 4b — Persistent auras (win32).** Render the starter set from the
`fxauras` mask on the low-rate aura timer. Start with hero speed (the
afterimage trail), because it's the most visible and tests the whole
pipeline. Add the others one at a time. Option: `fx_auras`.
*Done:* a single trailing afterimage for speed (`fx_afterimage`; the
hero's intrinsic speed alone doesn't show), rising z's while asleep
(magical sleep is kept on the monster, `mfxslept`, so it survives a
save), stars when stunned, swirls when confused, sinking drops when
slowed, air under levitation, a glint for reflection, a ring for the
protection spell, grit, slime and bubbles for stoning, slime and
sickness, and a see-through sprite when invisible but seen. Gas clouds
billow. Fleeing and flying were left out as too common.

**Phase 4c — Clean, modern interface.** Restyle the NetHackW window
chrome around the map, so it looks like a modern client and not a set of
Win32 dialogs. Everything stays keyboard-driven, and every panel can be
hidden. Target layout, from a reference screenshot:

- **Dark theme:** flat dark-grey panels, one readable UI font, and soft
  separators instead of sunken borders.
- **Menu bar by topic:** Game, View, Move, Items, Gear, Act, Magic, Info,
  Help. Each entry lists the command and its key, so the menus also teach
  the keys.
- **Character panel (top left):**
  - name, rank, alignment, race, role and dungeon level;
  - HP and Pw as coloured bars with the numbers on top;
  - stats in a compact grid, with small up/down arrows when a stat
    changes.
- **Equipment doll (beside it):** a 3×4 grid of slots that shows the
  tiles of worn and wielded items.
- **Message log (top centre):** scrollback with a separator between
  turns. Important messages are coloured: traps, "You see here", and
  anything that matches `MSGTYPE`/`menucolors`.
- **Inventory sidebar (right):**
  - grouped by class (Weapons, Armor, Comestibles, Potions, Gems, ...)
    with headers;
  - each line shows the item's tile, its letter and its name;
  - a Hide button, and updates live like `perm_invent`.
- **Context action bar (bottom):** buttons for likely actions right now
  (Pick up `,`, Eat `e`, Disarm, Search `s`, Rest `.`), each with its key
  hint. Buttons depend on what's underfoot and nearby.
- **View controls (bottom right):** an ASCII/tiles toggle and map zoom −/+.
- **Hero highlight:** a thin coloured frame around the hero's tile.

Most of this is window-port work in `win/win32` (status, message, menu
and inventory windows, plus a new action bar). It reads existing core
data: status fields from `status_update`, inventory from
`update_inventory`/`perm_invent`, and messages with their attributes.
The only core addition is likely a small "suggested actions here" query
for the action bar.

*Done* with `fx_theme` (on by default): one dark palette (`mhtheme.h`),
Segoe UI fonts, a dark title bar and a topic menu bar whose drop-downs
send the keys they show; a character panel in the status window (title,
HP/Pw bars, stat grid with change arrows, conditions, an equipment doll of
two rows of six slots, and a context action bar); messages floating on the
map's lower left (`fx_msg_overlay`, `fx_msg_backdrop`) with MSGTYPE,
menucolor, trap and "You see here" colours and fading; map zoom that
shows the whole level by default (Ctrl+=, Ctrl+-, Ctrl+0, Ctrl+wheel); a
hero frame (`fx_hero_frame`, `fx_hero_frame_light`); polished menus and
inventory with a Hide toggle; and a splash with the dragon-rider painting
(`win/win32/splash_art.png`, see `ART-CREDITS.txt`). The action bar is
worked out in the port from game state, so no core query was needed. Not
built: on-screen zoom and tiles/ASCII buttons (the keys and the View menu
cover them).

**Phase 5 — Polish / possible new front end.** *Next.* Fix rough edges
found in play. Then: level ambience, a higher-resolution tileset,
lighting/FOV shading. If GDI in `win/win32` becomes the bottleneck,
consider a new SDL2 or Qt-based port reading the same fx stream.

## Open decisions

- First front end: **win32** (decided; everything so far is in NetHackW).
  Qt remains an option later.
- Tileset: keep the default tiles at first; choose a larger set in Phase 5.
- Asset licensing: CC0, CC-BY (credited) and royalty-free game packs that
  allow use in games, each file credited in `sound/modern/attributions.txt`
  or `win/win32/ART-CREDITS.txt`. Packs that forbid redistribution are not
  used.

## Options

All are in the full options list (`m O`), under the `fx_` names:

| Option | Default | Effect |
|---|---|---|
| `fx_damage_numbers` | on | floating damage and healing numbers |
| `fx_healthbars` | on | health bars under creatures |
| `fx_status_text` | on | status names floating up as they land |
| `fx_exp_text` | on | "+N XP" floaters |
| `fx_motion` | on | lunges, shakes, death fades, smooth movement |
| `fx_screen_shake` | on | the view shakes for explosions and big hits |
| `fx_particles` | on | rays, explosions, wand, potion and scroll effects |
| `fx_auras` | on | lasting status effects on creatures, gas clouds |
| `fx_afterimage` | on | speed afterimages |
| `fx_theme` | on | NetHackW dark theme, fonts, panels and menu bar (restart) |
| `fx_msg_overlay` | on | messages on the map instead of a strip (restart) |
| `fx_msg_backdrop` | off | a translucent panel behind those messages |
| `fx_hero_frame` | on | gold corner marks round the hero |
| `fx_hero_frame_light` | off | a lighter version of the hero frame |
