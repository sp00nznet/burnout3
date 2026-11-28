# Changelog

All notable changes to this project are recorded here, newest first. The
format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Releases of this fork (apexJCL/burnout3-recomp) use calendar versions,
`YYYY.MM.N` (the tag `v2026.10.0-rc.1` is the section
`## [2026.10.0-rc.1] - YYYY-MM-DD`), not SemVer. A release tag's section is
its GitHub release notes: .github/workflows/release-wizard.yml stops when
the tagged commit has none.

## [Unreleased]

## [2026.11.3] - 2026-11-27

**Native Linux and the Steam Deck without Proton, a vsync switch and a
volume knob in the menu, and faster frames on the Deck.**
Burnout 3 also gets the in-game menu, on by default.

- **An in-game menu, on by default.** F10, Guide+A or Back+L3 held for
  0.4 s opens it (on a Steam Deck, Back+L3: SteamOS keeps Guide+A).
  `[menu] enabled = false` in `config/config.toml` turns it off.
- **Texture packs read P8 textures with their paired palettes**
  (vehicles, TXDs, track environments).
- **Native Linux.** The game now has a native Linux build of its
  Direct3D 11 renderer (dxvk-native), so SteamOS no longer needs Proton.
  `install.sh` copies an existing Proton install's saves and settings
  over. A game started on a Steam Deck outside Steam warns that Game Mode
  controls need Steam.
- **Vsync on the menu** (`present.vsync`). Stock stays the default.
- **Master volume** (`audio.volume`) on a new Audio page in the menu.
- **The same menu everywhere:** holding Back+L3 opens the menu and F3
  shows the stats strip, as in every game. The menu is drawn at the
  display's native resolution, so it stays sharp at any render scale.
- **Texture packs:** pick a pack by name on the Pack page. Pack v5
  recolours palettised textures from the live palette, and pack v6 gives
  shrunk text and 2D art mip chains, so text at 2x and 3x is no longer
  doubled or stair-stepped.
- **Faster frames:** the occlusion wait happens on the GPU instead of a
  spin, draws merge across more state changes, and stdio is buffered on
  Windows. The run-ahead walker the toolkit now turns on stays off for
  this game.
- **Fixes:**
  - Textures that use signed channels now filter correctly on D3D11 and
    Metal.
  - A pad button held while the game boots no longer fires on the first
    frame.
  - The recompiler no longer reads unset CPU flags where code paths join.
- **Crash reports:** a `[FATAL]` line on every host when startup config
  fails.

### For developers

- **Pins:** xboxrecomp-cli v0.23.0 and toolkit blinx2-v0.16.0, up from
  v0.21.0 and blinx2-v0.14.0 (2026.11.2).
- **Present shaders come from the toolkit:** librashader and the
  `shaders/` presets are staged beside every exe that links the
  enhancements layer, so the game's own `xboxrecomp_present_shaders` call
  is gone.
- **A key that waited for the pin:** `[enhance.textures]` builds the
  texture pack from the dump for `burnout3 enhance textures`; v0.21.0
  refused the table.
- **Toolkit blinx2-v0.16.0:** the dxvk-native D3D11 backend for Linux
  and librashader's D3D11 runtime there; the occlusion settle on the GPU;
  the checkpoint and semaphore defers, on by default; walker decode trims;
  `d3d11_prof` GPU timestamps and `host_prof`; PNG frame dumps on writer
  threads; `recomp --trace-stores`; `RECOMP_TRACE=suspend`; flags assigned
  at joins the recompiler cannot follow; the NOTICE's LGPL list and the
  patched librashader.so.
- **CLI v0.23.0:** `package` for native Linux, with the migration from a
  Proton install; `.recomp.json` for recomp.fyi; `doctor` warns on menu
  drift; `disc run` and `disc list`; `bench golden` ships hdd seeds and
  pad files to native Windows hosts; the build wizard's redesign (welcome,
  update path, help report); `enhance` packs v4 to v6 (recolour, mips,
  detile, dering, tone flattening); `report` replays menu bug reports; a
  disk-floor guard.

## [2026.11.2] - 2026-11-04

**The menu is readable on the Deck.**

- **A menu sized for your screen.** On the Steam Deck the menu is now
  large enough to read, and on a high-DPI desktop it follows your scale.
  Render scale and shaders no longer change its size, the Size setting
  applies at once, and taps reach every button at render scale 2 and 3.

### For developers

- **Pins:** xboxrecomp-cli v0.21.0 and toolkit blinx2-v0.14.0, up from
  v0.20.0 and blinx2-v0.13.0.
- **New in the CLI:** `saves list` and `saves import`, `[system]
  languages` for the toolkit's `system.language`, and the release gate
  (`[xbe] region`, `version`, `title`).
- **New in the toolkit:** mod API 8 (a 3D debug draw at the flip),
  `RECOMP_DEBUG=tex_dump=<dir>` for texture packs, and groundwork for an
  in-window game reset, off by default.

## [2026.11.1] - 2026-11-03

**The Windows build wizard works again, and your saves can travel.**
2026.11.0's wizard could stop partway on Windows; this patch fixes it
and brings a mouse-friendly menu along.

- **Windows wizard fixed.** Builds on a Windows PC with Git's default
  settings finish again: the toolkit's shader files stay LF, so the
  package step no longer refuses `shaders/param-kinds.toml`, text is
  written as UTF-8 whatever the system code page, and Retry works after
  a failed step.
- **Move your saves.** The menu's new Saves page exports your saves to a
  zip and imports one on another PC. Your current saves are backed up
  first, and the game closes so its next start loads the imported ones.
- **Share your settings.** The Files page exports your settings or a
  shader profile and imports one back, with a file browser in the menu.
- **The menu takes the mouse.** Hover, click, drag sliders and scroll;
  an on-screen keyboard pops up for renames on a touch screen.
- **A tidier menu.** Compact and expanded sizes (R3), a toolbar that
  folds to icons and button glyphs in the footer.

### For developers

- **Pins:** xboxrecomp-cli v0.20.0 and toolkit blinx2-v0.13.0, up from
  v0.19.0 and blinx2-v0.12.0, the same as BLiNX 2's 2026.11.1. The CLI
  writes every text file as UTF-8, clones the toolkit with
  `core.autocrlf=false` and heals an older CRLF clone; the toolkit's
  `.gitattributes` keeps packaged text LF. The bootstrap `burnout3.py` is the
  CLI's refreshed template.
- **Why a patch release:** the fix lives in the CLI and the toolkit, and
  the wizard runs the pinned CLI, so the pins had to move before players
  could build again. The toolkit's menu changes come along.
- **Mod API 7** (`glyph_name`, the `{ctl:}` token, a per-mod status
  panel) and **deterministic mode** (`RECOMP_DEBUG=deterministic`, off: a
  virtual clock, a fixed thread order and OHCI pads on that clock, so a
  pad replay repeats on one host) are in the toolkit; stock runs read
  the clock as before.

## [2026.11.0] - 2026-11-01

The fork's first full release: the console's own sound mix, less disk wear,
and a game that no longer holds a CPU core. Heads-up: custom key and pad
bindings need their new names (see below).

### Changed
- **The cache partition lives in memory** (`storage.ram_cache = true`, the
  toolkit's new default for every game): what the game writes to its
  scratch cache drive while it plays stays in RAM, up to the console's
  750 MB, instead of being rewritten on your disk. The game sees the same
  files. `ram_cache = false` under `[storage]` in `config/config.toml`
  puts them back on the disk.
- **The real audio DSP, on by default.** The game's own sound programs now
  run on the emulated Xbox audio DSP (reverb, effect sends, the game's own
  mix) instead of a plain stereo fold. `RECOMP_APU_DSP=0` in
  `config/launch.env` brings the old sound back.
- **Breaking: new names for controls and keys** in `[input.bindings]` and
  `[input.actions]` of `config/config.toml`. The toolkit now uses upstream
  xboxrecomp's names and no longer reads the old ones, so rewrite any line
  of yours that uses them: `ltrigger` `rtrigger` `lthumb` `rthumb` become
  `left_trigger` `right_trigger` `left_thumb` `right_thumb`; `lstick_*`
  and `rstick_*` become `left_stick_*` and `right_stick_*`; `KP0`..`KP9`
  become `Numpad0`..`Numpad9`; `KPEnter` `KPPlus` `KPMinus` `KPMultiply`
  `KPDivide` `KPPeriod` become `NumpadEnter` `NumpadAdd` `NumpadSubtract`
  `NumpadMultiply` `NumpadDivide` `NumpadDecimal`; `LBracket` `RBracket`
  become `LeftBracket` `RightBracket`; `MouseLeft` `MouseRight`
  `MouseMiddle` become `Mouse1` `Mouse2` `Mouse3`. An old name logs an
  `unknown control`, `unknown input` or `unknown name` line: an old control
  name is ignored and that control keeps its default; an old key name is
  skipped but the rest of the line still applies, so a line left with no
  valid entries unbinds that control (`start = "KPEnter"` leaves Start with
  no key); an action line keeps only its valid entries.
  The game's defaults bind nothing by name, so a file without such lines
  needs no change.
- The build pins xboxrecomp-cli v0.19.0 (was v0.7.7) and the toolkit fork
  at `blinx2-v0.12.0` (was `blinx2-v0.4.0`), which merges upstream
  xboxrecomp v0.14.0 through v0.16.0 and moves the
  macOS and Linux hosts from SDL2 to SDL3. The lifted code honours the
  SSE rounding mode (MXCSR) the game sets. The disassembler refuses a
  function start that falls inside an instruction of the body before it,
  and keeps tail blocks inside the function that jumps to them instead of
  splitting them off, so the game now lifts as 23,652 functions, not
  24,000. File handles are checked before use (a bad handle is an error,
  not a crash), and the title cache (`E:\CACHE`) gets its own `TitleCache`
  folder in the save folder instead of sharing `Cache` with the cache
  partitions.
- `present.pacing` defaults to `sleep` in `config.default.toml`, as in the
  toolkit (was `spin`): the game's lowered spin waits sleep until the kernel
  signals, so the game no longer holds a CPU core. `spin`, the
  console-original timing, is still an option, and the
  race gate still pins it. The file also lists `present.surface`.

### Fixed
- The lifter keeps narrow `mul`, `imul`, `div` and `idiv` on their 8- and
  16-bit registers instead of widening them, so a build from your dump
  computes those the way the console does.
- When a write needs room past the RAM cache's 750 MB cap, the cache drops
  data the game never opened, oldest first, instead of filling up.

## [2026.10.0-rc.2] - 2026-10-17

A fix for the Steam Deck: the game starts again under Proton.

### Fixed
- Steam Deck and Linux PC bundles pin their Proton: the installer fetches
  GE-Proton11-7 (checked against its sha256) and the launcher runs it, so
  no launch follows the newest GE-Proton. GE-Proton11-9 starts no game on a
  Deck since the October 2026 Steam Linux Runtime update, and lost the pads
  before that. The launcher also refuses a second launch while the game
  starts or runs. These come from xboxrecomp-cli v0.7.7, which the build now
  pins; the shared launcher defaults no longer set PROTONPATH=GE-Proton.

### Changed
- The release workflow leaves a release made by hand, or already published,
  as it is: it edits only a draft it made itself, and attaches the wizard
  assets to any other release only when it has none of them.

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
