# Changelog

All notable changes to this project are recorded here, newest first. The
format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Releases of this fork (apexJCL/burnout3-recomp) use calendar versions,
`YYYY.MM.N` (the tag `v2026.10.0-rc.1` is the section
`## [2026.10.0-rc.1] - YYYY-MM-DD`), not SemVer. A release tag's section is
its GitHub release notes: .github/workflows/release-wizard.yml stops when
the tagged commit has none.

## [Unreleased]

## [2026.10.0-rc.1] - 2026-10-15

The fork's first release: the build wizard for players, one window on
Windows and the Steam Deck, and the macOS build, on the xboxrecomp fork's
Direct3D 11 and Metal renderers.

The project now runs on the shared xboxrecomp runtime instead of its own fork,
and the title boots and plays through its own code. It builds on the
xboxrecomp fork (apexJCL/xboxrecomp `blinx2/portability`, tag
`blinx2-v0.4.0`), which carries upstream xboxrecomp #138-#156: the
vertex-program interpreter, register combiners,
x87 precision control, SSE compare predicates, multi-pad USB, the DPC queue lock
and per-flip frame dumps.

### Added
- Every game mode plays: Race, Time Attack, Road Rage and Crash for one player;
  Race, Road Rage, Double Impact and Team Crush in two-player split-screen; and
  Party Crash.
- Two controllers connected by default (`RECOMP_USB_PADS=2`); pad 2 is XInput
  pad 2 or `p2-` pad-script steps.
- `--headless`, `--record out.mp4` and `--seconds N`: run with no window, and
  encode every presented frame with ffmpeg when the run ends.
- A window by default, titled with the XBE title, fps and draw count.
- `Setup.cmd` / `tools/setup.ps1`: one-click setup from a clean machine. It
  checks and offers to install the tools, fetches the toolkit, unpacks your disc
  image, regenerates, builds and leaves a launcher.
- `regen.sh`: the toolkit pipeline in order. `config/seed_functions.json`:
  function entries only a run can find.
- Hang watchdog and a NaN trap (`BO3_TRAP_NAN`) in `main.c` for debugging.
- Docs: `architecture.md`, `running.md`, and `boot.md`, which records each fix
  between the entry point and a playable race.
- CONTRIBUTING, CONTRIBUTORS, ROADMAP, this changelog, and the MIT LICENSE file.
- Options files, from the toolkit's enhancements layer: `config.default.toml`
  beside the game lists every option at its stock value, and
  `config/config.toml` holds the ones the player changes. Render scale
  (`[render] scale = 2`, or `RECOMP_RENDER_SCALE=2`) draws at two to four
  times 640x480 on Metal and Direct3D 11. Anisotropic filtering
  (`[render] aniso = 2..16`), the present filter, fullscreen at start and
  spin-wait pacing are options too. Every option is stock unless
  set, and the race gate pins them stock (`race_gate.py --stock-env`).
- The build wizard: a small program for Windows, macOS and Linux (Steam Deck
  included), attached to every release, that builds and installs the game
  from your own disc image. It asks for the render scale and whether to start
  fullscreen.
- Releases on GitHub, with these notes and the wizard downloads; `-rc.`
  tags are pre-releases.
- `-DBO3_CONSOLE=ON` builds the console program, which prints to the
  terminal, for debugging from a command line.
- A dialog says what failed when the game cannot boot (no game files, a
  wrong path).

### Changed
- The build links the xboxrecomp runtime libraries (`../xboxrecomp`) instead of
  in-tree copies.
- One window on Windows and under Proton: the game window, titled
  "Burnout 3: Takedown". It opened up to three (a console, a blank
  framebuffer window and the game). The exe is now a Windows GUI program.
- Fullscreen is off by default. On a Steam Deck, start the game from Steam
  with the Gamepad template (see the bundle's README).
- Format and engine research notes moved to `docs/research/`.

### Removed
- The in-tree runtime fork (`src/kernel`, `src/d3d`, `src/audio`, `src/apu`,
  `src/input`, `src/nv2a`, `src/platform`) and the hand-written game layer
  (`fe_menu`, `rw_renderer`, `rw_bridge`, the pseudo-3D road, fake physics,
  loaders). The title draws its own menus and races now.
- `src/nv2a`'s captured pushbuffer and font data, which came from the game, and
  its xemu-derived code.
- The old Python tools (`tools/`); the toolkit's versions replace them.
- `CLAUDE.md` from tracking.
- Stale docs describing the fork (`d3d8ltcg_device_context.md`,
  `docs/technical/`, `kernel-imports.md`), and `renderware_layouts.md`, which was
  taken from RenderWare SDK headers.

### Fixed
- The README described a build that did not run (#1, reported by
  [@VincxBlox](https://github.com/VincxBlox)).
- The picture flickered and changed height at the start of a race.
- The game no longer writes the Xbox cache files (`CACHE/`) into your game
  files folder; they go with the saves.
- Sound effects (menu select, turbo, engines) played as fizz and noise. The
  game loads them into its heap, whose physical addresses are also handed out
  as contiguous memory, and the APU read the contiguous copy. The toolkit's APU
  now reads RAM for any heap page the game named through MmGetPhysicalAddress.
