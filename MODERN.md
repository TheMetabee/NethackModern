# NetHack Modern

A fork of **NetHack 5.0** (official DevTeam branch `NetHack-5.0`) whose goal is
to bring the tiles experience up to the feel of *Tales of Maj'Eyal*: clear
visual feedback on what hit what and where, floating damage numbers, monster
health bars, attack/spell animations, and a full sound layer.

Game rules stay vanilla NetHack. Everything we add is presentation, and must
be switchable off with options so the game still plays (and saves) like 5.0.

## Repository layout

| Remote     | Purpose                                                 |
|------------|---------------------------------------------------------|
| `upstream` | https://github.com/NetHack/NetHack.git (branch `NetHack-5.0`) |
| `origin`   | your own GitHub repo, once you create one              |

Branch `main` = upstream `NetHack-5.0` + our work. To pull in DevTeam fixes:

```
git fetch upstream
git merge upstream/NetHack-5.0
```

Our code goes in clearly separated places to keep those merges painless:

- `src/fx.c`, `include/fx.h` — game-core "presentation event" emitter (new)
- `sound/miniaudio/` — new soundlib backend (5.0 already reserves `SND_LIB_MINIAUDIO`)
- `win/win32/` — first front end we modernise (Windows tiles, `NetHackW.exe`)
- `assets/modern/` — sound files and extra sprites (with attributions)
- `docs/modern/` — design notes and the roadmap

Keep edits to upstream files small: ideally one `fx_*()` call per hook site.

## Building (Windows)

See `sys/windows/` in the upstream tree (Visual Studio solution and
`Makefile.nmake`, plus `sys/windows/vs/`). Phase 0 of the roadmap is getting a
stock `NetHackW.exe` building from this repo before we change anything.

## Roadmap

See [`docs/modern/ROADMAP.md`](docs/modern/ROADMAP.md).
