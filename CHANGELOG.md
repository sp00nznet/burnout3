# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/).

## [Unreleased]

## [0.1.0] - 2026-10-20

The project now runs on the shared xboxrecomp runtime instead of its own fork,
and the title boots and plays through its own code. Needs xboxrecomp
#138-#156, pinned at `afeb69f` (2026-09-30): the vertex-program interpreter,
register combiners, x87 precision control, SSE compare predicates, multi-pad
USB, the DPC queue lock and per-flip frame dumps.

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
- A Windows installer (`Burnout3-Installer-windows-x64.exe` on the release),
  built by recomp-launcher from `launcher/launcher.json`: choose the folder with
  your `.iso`, **Build my recomp**, **Play**.
- `Setup.cmd -Yes -GameDir <iso or folder>`: installs what's missing and asks
  nothing (the recomp-launcher setup contract). `-GameDir` takes the image, a
  folder holding it, or an unpacked disc; `-Iso` still works.
- `regen.sh`: the toolkit pipeline in order. `config/seed_functions.json`:
  function entries only a run can find.
- Hang watchdog and a NaN trap (`BO3_TRAP_NAN`) in `main.c` for debugging.
- Docs: `architecture.md`, `running.md`, and `boot.md`, which records each fix
  between the entry point and a playable race.
- CONTRIBUTING, CONTRIBUTORS, ROADMAP, this changelog, and the MIT LICENSE file.

### Changed
- Setup and the installer fetch xboxrecomp `afeb69f`, not `main`: toolkit
  changes after it (by 2026-10-18) stop the title before its first draw.
- The build links the xboxrecomp runtime libraries (`../xboxrecomp`) instead of
  in-tree copies.
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

[Unreleased]: https://github.com/sp00nznet/burnout3/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/sp00nznet/burnout3/releases/tag/v0.1.0
