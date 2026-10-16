# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/).

## [Unreleased]

The project now runs on the shared xboxrecomp runtime instead of its own fork,
and the title boots and plays through its own code. Needs xboxrecomp
#138-#156 (on its `main`): the vertex-program interpreter, register combiners,
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

### Changed
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
- Boots on xboxrecomp v0.14.0: six functions its disassembler misses are
  seeded. Without 0x00014C80 the game stays on a black screen at the
  language movie; without 0x000782C0 the main menu stops advancing once
  the profile has loaded. sp00nznet/xboxrecomp#186 finds them statically,
  and the seeds are then redundant.
- The DSP doorbell is left to the toolkit (`RECOMP_APU_DSP_ACK` unset means
  `auto` since sp00nznet/xboxrecomp#193); the fixed 0x825F8810 was the
  address before the contiguous allocator changed in
  sp00nznet/xboxrecomp#189. This needs xboxrecomp v0.14.0: update an
  existing checkout with `git -C ../xboxrecomp pull`, then regenerate and
  rebuild.
- The README described a build that did not run (#1, reported by
  [@VincxBlox](https://github.com/VincxBlox)).
