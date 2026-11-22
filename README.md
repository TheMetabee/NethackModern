# NetHack Modern

https://github.com/user-attachments/assets/0be7569d-6417-454a-9a92-c5f842572008

NetHack Modern is a fork of **NetHack 5.0** that changes how the game
looks and sounds on Windows. It adds a dark interface, floating damage
numbers, health bars, animations, particle effects, lighting, and a full
set of sound effects and level ambience. **The game itself is unchanged.**
Every rule, monster, item and random roll is vanilla NetHack 5.0, and your
saves are ordinary 5.0 saves.

Each addition has its own option, so you can turn off any of them and get
back the look of stock NetHackW.

**Not a variant, but not stock NetHack either.** NetHack Modern changes
NetHack's source code: it adds small hooks that tell the interface what
just happened (a hit, a miss, a spell, a death). Those hooks only
report; they don't change any rules. So it isn't a variant in the sense
of SLASH'EM or EvilHack, which play differently. Think of it as NetHack
5.0 with a new coat of paint.

**Not an official release.** NetHack Modern is not an official NetHack
release and is not affiliated with or endorsed by the NetHack DevTeam.
Please report problems with it here, not to the DevTeam.

> The NetHack DevTeam's own README, which covers NetHack 5.0 itself, is

> still in this repository as [`README`](README).

---

## Contents

- [Status and plans](#status-and-plans)
- [What stays the same](#what-stays-the-same)
- [What's new](#whats-new)
- [Playing](#playing)
- [Options](#options)
- [Building from source](#building-from-source)
- [How it works](#how-it-works)
- [Changes to NetHack's own files](#changes-to-nethacks-own-files)
- [Credits and licences](#credits-and-licences)

---

## Status and plans

NetHack Modern is a work in progress. It changes as it goes: features
get adjusted, rough edges get fixed and new ideas get added along the
way, so expect each release to look and sound a little different from
the last.

**Done so far:** the event stream, damage numbers and health bars,
status text, sound, animation, lasting status effects, the interface,
level ambience, lighting, tilesets and pointing projectiles.

**Still to do:**

- Fix the rough edges found in play. Most of the recent work has been
  this, such as keeping the screen in step with fast movement.
- Make drawing cheaper. NetHackW draws with GDI, which is slow at
  blending and scaling, especially with large tiles.
- Build a new **SDL** front end that draws with the GPU and reads the
  same event stream, for smooth scaling, blending and lighting at any
  tile size. Qt is the fallback.
- **Linux and macOS ports are a work in progress.** Today everything is
  in the Windows tiles interface. The SDL front end is the way to bring
  the same effects and sound to Linux and macOS.

[`docs/modern/ROADMAP.md`](docs/modern/ROADMAP.md) has the full history
and design notes.

**Feedback is very welcome.** If something looks wrong, plays badly,
feels slow, or you have an idea for what to add next, please open a
GitHub issue. I appreciate every bug report and suggestion.

Contributions as pull requests are welcome too. I'll do my best to pick
what fits this modern take on NetHack: changes that make it look, sound
and feel better while the game underneath stays vanilla.

---

## What stays the same

- **The rules.** Nothing NetHack Modern adds changes game state or uses
  the random number generator. A game plays out exactly as it would in
  NetHack 5.0.
- **Saves.** The save format is unchanged. The one extra flag, which
  tells magical sleep apart from paralysis so the right effect shows,
  uses a bit the save file already had spare.
- **What you know.** Effects only show what the hero can already see or
  sense. A monster's hidden speed, a disguised mimic or an unseen
  summoning get no effect. Messages you've hidden with `MSGTYPE` make no
  sound.
- **Your config file.** Your `.nethackrc` is never written to. Settings
  you change while playing go into a separate file (see
  [Remembered settings](#remembered-settings)).
- **Speed.** Animations never hold up input. A keypress finishes any
  running animation at once. Running, travel and count-prefixed commands
  skip the per-step animation, and damage is added up into one number.
  When nothing is moving, the map redraws just as stock NetHack does.
- **Other interfaces.** The tty, curses and other ports build and play as
  before. All the visual work is in the Windows tiles port, `NetHackW.exe`.

## What's new

### Combat feedback

- **Damage numbers.** Damage and healing float up from whoever took it,
  for the hero and for every monster in sight. They're coloured by kind:
  physical white, fire orange, cold blue, poison green, healing bright
  green. Big hits show larger.
- **Health bars.** A thin bar under every damaged creature you can see.
- **Status text.** When a status lands on someone, its name floats up:
  "Confused", "Stunned", "Blinded", "Fast", "Slowed", "Stoning" and so
  on. Bad ones are red or purple, good ones gold or green. Also "Miss",
  "Killed" and "Died", "Level N!" in gold, and "+N XP".

### Animation

- **Lunges.** Attackers nudge toward their target and back.
- **Hit shakes.** The victim shakes and recoils away from the blow, more
  for bigger hits.
- **Deaths.** The body shakes and then fades out.
- **Knockback** slides the target to its new square.
- **Screen shake** for explosions, earthquakes and big hits.
- **Smooth movement.** Creatures glide from one square to the next
  instead of jumping. The glide gets shorter when you move fast, so the
  screen keeps up with your keypresses.
- **Pointing projectiles.** Arrows, bolts, daggers, darts, spears and the
  like turn to point the way they fly, whoever throws or fires them. The
  direction is measured from the tile picture itself, so this works with
  any tileset. Round things such as rocks are left alone.

### Particle effects

Rays and breath attacks, explosions, every wand, potions, scrolls,
teleports, detection, the unicorn horn and more get particle effects. A
monster that is summoned or created in sight arrives with a spiral, a
flash and a ring, coloured by where it came from: violet for create
monster, fire and smoke for demons, gold for angels, a dusty burst for
insects and snakes. Gas clouds billow.

### Lasting status effects (auras)

While a status lasts, it shows on the creature, even when it's standing
still:

| Status | Look |
|---|---|
| Very fast | a trailing afterimage of the sprite |
| Asleep (magical) | rising z's |
| Stunned / confused | orbiting stars / swirls |
| Slowed | sinking drops |
| Levitating | air moving under the sprite |
| Reflecting | an occasional glint |
| Protection spell | a soft ring |
| Stoning / slimed / sick | grit, slime or bubbles |
| Invisible but seen | a see-through sprite |

Auras run on their own slow timer, which only runs while one is in view.

### Lighting

A smooth shade layer lies over the map. Lit rooms are fully bright. Light
from lamps, candles and glowing monsters fades toward the edge of its
reach, with a warm tint. Candles, oil lamps and fire creatures flicker;
lanterns and glowing weapons stay steady. Remembered squares are dimmed
and solid rock stays dark. How dark out-of-sight areas get can be set to
light, medium, dark or deep.

### Interface

- **Dark theme.** One dark palette, Segoe UI fonts, a dark title bar and
  flat panels in place of the sunken Win32 look.
- **Menu bar by topic:** Game, View, Move, Items, Gear, Act, Magic, Info,
  Help. Each entry shows its key, so the menus also teach the keys.
- **Character panel.** Title, HP and Pw as coloured bars, a stat grid
  with up and down arrows when a stat changes, your conditions, and an
  equipment doll showing the tiles of what you wear and wield.
- **Action bar.** Buttons for what makes sense right now, such as Pick
  up, Eat, Search or Rest, each with its key.
- **Messages on the map.** The latest messages float over a corner of the
  map with an outline that reads over any floor. Older turns fade and
  then drop away. Important ones are coloured (traps, "You see here",
  `MSGTYPE` and `menucolors` matches). You can put them in any corner or
  at the middle of the top or bottom edge, and add a translucent panel
  behind them. Ctrl+P still scrolls back through the history.
- **Map zoom.** The whole level fits the window by default. Ctrl+= and
  Ctrl+- zoom, Ctrl+0 resets, and Ctrl+mouse wheel works too.
- **Hero frame.** Gold corner marks around the hero's square, which slide
  along as the hero moves.
- **Inventory panel.** Grouped by item class with headers, each line with
  the item's tile. It updates as your inventory changes and has a Hide
  button.
- **Clearer menus** and a new splash screen.

### Tilesets

The View menu can switch between the built-in tiles and any tile image
laid out like NetHack 5.0's own `tiles.bmp`. The tile size is worked out
from the image, and zoom keeps tiles at a sensible size on screen, so
large tiles fit the whole level and show their detail when you zoom in.
No third-party tileset ships with NetHack Modern. Older tilesets made for
the 3.6 layout need converting to the 5.0 layout first.

### Sound

NetHack Modern adds a new sound library built on
[miniaudio](https://miniaud.io/), which can play many sounds at once,
unlike the basic Windows `PlaySound` backend. It has about 700 samples:

- weapon sounds for each weapon skill, with elemental layers for fire,
  cold and shock;
- misses, critical hits, and a death cry for each kind of creature;
- spells, wands, potions, scrolls, prayer, traps, doors and locks;
- eating, matched to what you're eating;
- level-up and other rewards;
- wading (there are no other footsteps, on purpose).

It also plays the sound effects the core already asks for.

**Level ambience** plays a quiet loop for each kind of level and
crossfades when you change levels, with an occasional sound on top.
There are loops for the Gnomish Mines, Minetown, Sokoban, the Oracle, the
quest, Fort Ludios, Medusa's island, the Castle, the Valley, Gehennom's
fire, ice, water and barred levels, Vlad's Tower, the Wizard's Tower,
the Planes and more. Effects and ambience have separate volume controls,
in the Game menu and as options.

## Playing

NetHack Modern runs on Windows for now; Linux and macOS ports are a work
in progress (see [Status and plans](#status-and-plans)). Download the
release zip, unpack
it anywhere and run **`NetHackW.exe`**. Keep the `sounds` folder next to
the exe: that's where the game looks for its sounds.

Everything else plays like NetHack. The [NetHack Guidebook](doc/) and
the in-game help (`?`) cover the game itself.

### Keys

| Key | What it does |
|---|---|
| Ctrl+= or Ctrl+numpad + | zoom the map in |
| Ctrl+- or Ctrl+numpad - | zoom the map out |
| Ctrl+mouse wheel | zoom in or out |
| Ctrl+0 | back to the default zoom, which fits the whole level |
| Ctrl+P | scroll back through earlier messages |

Every other key is NetHack's own. The menu bar lists each command with
its key, so it doubles as a key reference.

### Remembered settings

Any `fx_` option you change while playing, from the menu bar or the
full options menu (`m O`), is saved to **`nethackmodern.rc`**. It sits beside
your own NetHack config file (normally in `%USERPROFILE%\NetHack\`) and
is read after it. Your own config file is never touched. To go back to
your own settings, delete a line from that file, or the whole file.

You can also set any of the options below in your own config file as
usual, for example `OPTIONS=!fx_screen_shake,fx_shade:dark`.

## Options

Every addition has an option, under `fx_` names in the full options list
(`m O`). Most are also in the View and Game menus.

| Option | Default | What it does |
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
| `fx_turn_missiles` | on | thrown and fired things point the way they fly |
| `fx_theme` | on | dark theme, fonts, panels and menu bar (restart to apply) |
| `fx_msg_overlay` | on | messages on the map instead of a strip (restart to apply) |
| `fx_msg_backdrop` | off | a translucent panel behind those messages |
| `fx_msg_pos` | bottom-left | message corner: top or bottom; left, centre or right |
| `fx_hero_frame` | on | gold corner marks around the hero |
| `fx_hero_frame_light` | off | a lighter version of the hero frame |
| `fx_lighting` | on | soft light on the map; out-of-sight squares dimmed |
| `fx_shade` | medium | how dark out of sight gets: light, medium, dark, deep |
| `fx_bar_style` | flat | HP/Pw bars: flat, smooth, gloss, striped, bevel |
| `fx_bar_border` | off | a thin dark outline around the HP/Pw bars |
| `fx_ambience` | on | level ambience loops |
| `fx_volume` | 100 | sound effects volume, 0–100 |
| `fx_ambience_volume` | 60 | ambience volume, 0–100 |
| `fx_hide_inventory` | off | keep the inventory side panel hidden |

The tileset is NetHack's own `tile_file` option (`tile_file:none` means
the built-in tiles).

## Building from source

You need **Visual Studio 2022** with the "Desktop development with C++"
workload.

1. Open `sys\windows\vs\NetHack.sln`, or build from a Developer Command
   Prompt:

   ```
   cd sys\windows\vs
   msbuild NetHack.sln -m -p:Configuration=Release -p:Platform=x64
   ```

2. The game lands in `vsbinary\`: `NetHackW.exe` (tiles), `NetHack.exe`
   (console), the game data and the `sounds` folder.

3. Close the game before rebuilding, or the linker can't replace the exe.

For testing, `scripts\wizard.cmd` starts the build in wizard (debug) mode
as the player "wizard". It also writes every presentation event to
`vsbinary\fxlog.txt`. Wizard mode needs `WIZARDS=*` (or your user name) in
the sysconf file, normally `C:\ProgramData\NetHack\5.0\sysconf`.

Setting the environment variable `NETHACK_FXLOG=<file>` logs the same
events with any interface.

## How it works

The game core doesn't know about pixels. NetHack Modern adds one small
module, **`src/fx.c`**, which the core calls at interesting moments, and
which passes **presentation events** to whatever front end has signed up
for them:

```
fx_damage(x, y, amount, kind, who)
fx_miss(x, y, who)
fx_death(x, y, mon)
fx_attack(from_x, from_y, to_x, to_y, attack_type)
fx_spell(x, y, spell_or_zap_type)
...
```

- **Damage isn't hooked at all ~140 places that change HP.** Instead
  `fx_sync()` runs whenever the screen is flushed and compares each
  creature's HP and status with what it saw last time. The numbers and
  status names therefore appear together with the message they belong
  to. Explicit hooks only add what a comparison can't know: who attacked
  whom, misses, deaths and the kind of damage.
- **Lasting statuses are state, not events.** Each map square already
  reaches the front end as a `glyph_info`. NetHack Modern adds a bitmask
  to it, `fxauras`, which is worked out from real game state each time
  the square is drawn. Nothing new is saved, and front ends that ignore
  it draw exactly what they did before.
- **Events are pure output.** They never use the random number
  generator, never change the game and are never saved. They're only
  sent for things the hero can perceive.
- **Two listeners.** The display listener (`fx_set_handler`) is the
  NetHackW map. The sound listener (`fx_set_audio_handler`) is the
  miniaudio sound library. A front end that registers neither gets
  nothing, so tty and curses carry on unchanged.

Because a future front end would read the same event stream, the core
work carries over to it.

### Where the code is

| Path | What's there |
|---|---|
| `src/fx.c`, `include/fx.h` | the presentation event stream |
| `sound/miniaudio/` | the miniaudio sound library (`masound.c`) |
| `sound/modern/` | sound samples, ambience loops and `attributions.txt` |
| `win/win32/` | the NetHackW port: map effects and lighting (`mhmap.c`), character panel (`mhstatus.c`), messages (`mhmsgwnd.c`), menus (`mhmenu.c`), menu bar and window (`mhmain.c`), splash (`mhsplash.c`), theme colours (`mhtheme.h`) |
| `docs/modern/ROADMAP.md` | design notes, phases and the full options table |
| `MODERN.md` | notes for developers: remotes, merging upstream, where code goes |

## Changes to NetHack's own files

NetHack Modern keeps its edits to DevTeam files small, usually one
`fx_*()` call at each hook site, so DevTeam fixes merge in cleanly
(`git fetch upstream && git merge upstream/NetHack-5.0`). Apart from the
new files above, it changes:

- **Hook calls** in the combat, zapping, throwing, explosion, potion,
  scroll, teleport and death code (`uhitm.c`, `mhitu.c`, `mhitm.c`,
  `zap.c`, `explode.c`, `dothrow.c`, `mthrowu.c`, `mon.c` and others).
  These only report what happened.
- **`display.c`**: fills in the `fxauras` mask, and tells the front end
  where a thrown or fired object is in flight so it can be pointed.
- **`include/wintype.h`**: the `fxauras` field in `glyph_info`.
- **`include/monst.h`**: the `mfxslept` flag, in a spare bit.
- **`include/seffects.h`**: extra sound effect names.
- **`include/optlist.h`, `include/flag.h`, `src/options.c`**: the `fx_`
  options, and `tile_file:none`.
- **`src/cfgfiles.c`**: writing remembered settings to
  `nethackmodern.rc`.
- **`win/win32/`** and the Visual Studio project files: the restyled
  port and the new sound library.

## Credits and licences

- **NetHack** is by the NetHack DevTeam and is released under the
  [NetHack General Public License](dat/license). NetHack Modern is
  released under the same licence.
- **Sounds:** every file is credited in
  [`sound/modern/attributions.txt`](sound/modern/attributions.txt),
  which also ships in the release's `sounds` folder. They come from:
  - Kenney (RPG Audio, Impact Sounds), rubberduck and Qvantor (Success,
    Level Up and Reward Sounds), CC0;
  - Leonardo Calvo / Soundrack (Swords & Blades, Archery),
    [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/);
  - AHTu20 (Animals & Creatures), TomMusic (Free Fantasy SFX Pack, also
    the ambience loops) and moneywithjj (Medieval Weapons), royalty-free
    for games.

  The royalty-free packs are included only as part of this game. Don't
  extract them and redistribute them as sound packs.
- **miniaudio** by David Reid, public domain or MIT No Attribution.
- **Splash art:** see
  [`win/win32/ART-CREDITS.txt`](win/win32/ART-CREDITS.txt).
