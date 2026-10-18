# b3-public-release: Burnout 3 on the CLI scaffold, one window, its own wizard, public releases

Status: drafted by Fable (2026-10-15), revised after the Opus review of
c3aa8cf. Successor of `b3-on-xbr` (phases 1-3 merged in b3; see design
D0). Facts checked against:
- b3 `main` d6eab90 (19 local commits on upstream `sp00nznet/burnout3`
  79b578d, i.e. `origin/main..main`; push URL disabled); the only live
  branch is xb-upstream's `fix/race-gate-aniso` edb0835
  (config.default.toml +3, `scripts/race_gate.py` and its test; nothing in
  main.c, CMake, the window code or game.toml);
- xboxrecomp-cli `main` 489196b (v0.7.4 at 2e7c207, docs after it) and
  the branch `docs/changelog` 2182758 (0.7.5: the release workflow's
  `notes` job, pre-releases for `-rc.` tags); P1 rechecks the base at its
  start;
- toolkit `posix-host/portability` 6c06ae4: `templates/new-game/`
  (`CMakeLists.txt`, `src/main.c`, `src/recomp_manual.c`, `.gitignore`;
  no game env header), `tests/template_main/`, `src/video/fb_present.c`
  (Windows only, `src/video/CMakeLists.txt:9`), `video_pump.c`,
  `fb_present_sdl.c`, `src/d3d/nv2a_pb_d3d11.c`; the in-flight toolkit
  branches `fix/present-mode-height` 316377d (contains babd350; it edits
  `xbox_FramebufferWindowStart`) and `fix/occ-reports` d6ae77e;
- cat `main` c6c5f96: `src/main.c`, `game.toml` `[wizard]`,
  `.github/workflows/release-wizard.yml`, `scripts/export-public.sh`,
  `src/env/recomp_env_game.h`;
- the dump `burnout_3/`: read, never written.

The spec lives in cat because b3 has no `openspec/` (as b3-on-xbr).

## Why

The user (2026-10-15): tidy up Burnout 3 so that it uses the
`xboxrecomp-cli` scaffolding, has its own build wizard, publishes releases,
boots into one window like BLiNX 2, and so that both games are public and
cross-reference each other's progress. The public home is a fork of
`sp00nznet/burnout3` (MIT, (c) 2026 sp00nz) at `apexJCL/burnout3`, our work
on the fork's `main`, pushed in batched rounds like `blinx2-recomp`. The
user also wants upstream kept and augmented the way BLiNX 2 is, with the
result offered upstream (U8).

What stands in the way today:
- **Three windows on Windows and under Proton** (seen by the user on the
  native Windows PC and on the Deck). B3's exe is a console-subsystem
  program (`add_executable(burnout3 ...)` without `WIN32`, entry `main`):
  started by the bundle's GUI launcher on Windows (`launcher.c`,
  `-mwindows`, `CreateProcessW` with no creation flags), or by
  `launch.sh` through umu-run on the Deck, it has no console attached, so
  Windows and Wine each give it a new console window. Its `main.c` then
  sets `RECOMP_FB_WINDOW=1` whenever not `--headless`
  (`src/game/main.c:1075`), which opens the toolkit's GDI framebuffer window
  (`fb_present.c`, `xbox_FramebufferWindowStart`, started from
  `AvSetDisplayMode` in `kernel_bridge.c:2413`); and the bundle's
  `RECOMP_PB_BACKEND=d3d11` opens the D3D11 backend's own window
  (`nv2a_pb_d3d11.c:497`, logged as `device up: feature level ...`),
  "the only one a D3D11 run has" for BLiNX 2 (cat links `WIN32`, starts at
  `WinMain`, never sets `RECOMP_FB_WINDOW`). Console + GDI + D3D11 = three;
  the D3D11 one shows the game. On the Mac both games already have one
  window: the SDL window that `xbox_HostWindowMain` opens, into which Metal
  blits (`fb_present_sdl.c:556`, `fb_present_metal.m`, logged as
  `[PRESENT] window up: CAMetalLayer`).
- **Scaffold drift.** `xbr new` now writes a game (`game.toml`, the
  bootstrap, `pyproject.toml`, `.gitignore`, `.gitattributes`, README,
  `.github/workflows/release-wizard.yml`) and copies the toolkit's
  `templates/new-game/`. B3 was ported by hand before `new` existed
  (b3-on-xbr D5): it has no `release-wizard.yml`, no `.gitattributes` from
  the scaffold, no `[wizard]` table, and its `main.c` carries its own
  copies of what the template now provides.
- **The template is behind both games.** `templates/new-game/src/main.c`
  (938 lines) has the stdio redirect, the `[CRASH]` tags, the VEH and
  POSIX handlers, the env table, the fatal dialog, the GUI subsystem and
  the Metal default. It has no enhancements layer (`xbox_enhance_init`,
  `enhance_cfg_report_unused`; cat `src/main.c:1358`, b3 `main.c:877`), no
  `if(TARGET xbox_enhance)` link or `config.default.toml` copy in its
  CMake, no window title (`xbox_HostWindowSetTitle`, which the toolkit has
  at `fb_present.c:270`), and does not read the launcher's folder keys
  (`RECOMP_GAME_FILES`, `RECOMP_HDD_DIR`, which `launcher.c:169-171` sets
  for every game and which cat and b3 each declare as identical game rows
  in their `recomp_env_game.h`).
- **No public home, no releases.** b3's `origin/main..main` commits are
  authored with the private local identity; the tree is clean of host names and private
  paths (`git grep` over tracked files: no hit), the commit messages carry
  tracker ids (`BLX-nn`) only. cat publishes through `export-public.sh`
  (one fresh commit, no history); a fork that keeps upstream's history
  cannot use it.
- **The Deck playtest** found the bundle launched fullscreen from Konsole
  with no quit and no pad; the fix was `fullscreen = false` and a README
  note (Desktop Mode: add to Steam with the Gamepad template). The wizard
  defaults and the release README must carry that.

## What changes

Five phases, each a reviewable merge (design D8):

- **P0 (cat):** close out `b3-on-xbr`'s bookkeeping (its tasks.md says 16
  ticked while phase 1 is merged in b3 as 5cf69e5; 2.7's B4, 3.7's Deck
  install and 4.x are done or superseded).
- **P1 (toolkit, then CLI):** `xbox_FramebufferWindowStart` returns right
  after its `RECOMP_FB_WINDOW` check when `RECOMP_PB_BACKEND` is `d3d11`
  (case-insensitive), logging once; the decision is a function built on
  every host with a ctest. `RECOMP_GAME_FILES` and `RECOMP_HDD_DIR` become
  toolkit rows (the generic launcher sets them for every game), and the
  template reads them, gains the enhancements layer and the window title;
  cat and b3 drop their duplicate rows at the pin move. `scaffold.py`'s
  `TEMPLATE_PATCHES` already patches `YOUR_GAME_NAME`; a test checks the
  patched title reaches `xbox_HostWindowSetTitle`.
- **P2 (b3):** one window. `WIN32` on `add_executable` (GUI subsystem, with
  `main` kept as the entry through `/ENTRY:mainCRTStartup` under MSVC and
  `-mwindows` under MinGW, so `--headless --record --seconds` still parse),
  a `BO3_CONSOLE` cache option for terminal debugging, and
  `RECOMP_FB_WINDOW=1` for every backend but `d3d11` under the env table
  (always, on upstream's toolkit, which has no D3D11 backend).
  `docs/running.md`, TESTING.md's run lines and the README say so. Gate:
  the window count goes from three to one on the Deck and on the Windows
  PC, counted as text; one Proton run on the CPU rasteriser keeps its
  one GDI window.
- **P3 (b3, CLI if a gap shows):** align B3 with `xbr new`: a scaffold diff
  (`xbr new` into a temp dir with `--offline`, compared file by file),
  adopting the 0.7.5 `release-wizard.yml`, `.gitattributes`, the bootstrap
  text and `.gitignore` lines; the `[wizard]` table (repo, targets,
  options, credits with `own = true` for `apexJCL/burnout3` and an entry
  for `sp00nznet/burnout3`; no icon: the CLI's generic gamepad glyph);
  `./burnout3 wizard config --dev` and `wizard build --dev` pass on the
  Mac; a CalVer `CHANGELOG.md`; README "Progress" with fresh curated
  images of our build in `docs/images/` beside upstream's
  `docs/screenshots/`; the Deck notes; b3's `scripts/audit-public.sh`.
- **P4 (b3 and cat; outward-facing, the user's go at each step):** the fork
  `apexJCL/burnout3` (made by the user, Actions enabled with workflow
  write permission), the upstream-moved check, the one-time date-keeping
  identity rewrite of `origin/main..main` with the old tip kept as
  `archive/main-pre-public`, the audit, the first push, the CHANGELOG cut
  and the tag `v2026.10.0-rc.N`, the published pre-release from
  `release-wizard`, cat's README cross-link in the same push round, and
  the upstream PR plan (U8).

Kept as upstream ships them (design D2): `regen.sh`, `Setup.cmd`,
`tools/setup.ps1` and the MSVC CMake path with `bin/` output. Only P2's
subsystem change touches that path, and `BO3_CONSOLE=ON` restores the
console build for it.

## Out of scope

- Golden scenarios for Burnout 3 (`[golden] json = ""` stays; the race gate
  and boot-to-START are the gates, D6).
- Enhancements beyond what B3 has (scale, filter, fullscreen, pacing,
  aniso); a HUD pack; widescreen.
- Opening the upstream PR to `sp00nznet/burnout3`: P4 plans it (D7, U8);
  it is opened only on the user's go, after the first public push, tested
  per CLAUDE.md's upstream-PR rule.
- The race-start jitter (toolkit `fix/present-mode-height` 316377d) and
  the occlusion reports (`fix/occ-reports` d6ae77e):
  other changes. 316377d edits `xbox_FramebufferWindowStart` itself, so
  P1's guard is a single early return and the merge order is checked with
  `git merge-tree` (design D1).
- A native-Windows playtest beyond the window count P2 asks for.

## Impact

- **toolkit:** `src/video/fb_present.c` (the early return),
  `src/video/window_title.c` or a new `src/video/fb_window_wanted.c` (the
  decision, built on every host) with its header line in
  `video_player.h`, `src/platform/recomp_env.h` (the two rows),
  `templates/new-game/{CMakeLists.txt,src/main.c}`, a ctest under
  `tests/`, `tests/template_main`, `docs/env.md`.
- **xboxrecomp-cli (public, noreply identity):** `tests/test_scaffold.py`
  (the title test); nothing else unless the scaffold diff in P3 shows a
  gap.
- **b3:** `CMakeLists.txt`, `src/game/CMakeLists.txt`, `src/game/main.c`,
  `src/env/recomp_env_game.h` (the two rows dropped), `game.toml`,
  `.github/workflows/release-wizard.yml`, `.gitattributes`, `.gitignore`,
  `README.md`, `CHANGELOG.md`, `docs/running.md`, `docs/images/`,
  `packaging/*/README.part`, `scripts/audit-public.sh`; a `public` remote;
  the commit identity (D7).
- **cat:** `src/env/recomp_env_game.h` (the two rows dropped at the pin
  move), `openspec/changes/b3-on-xbr/tasks.md`, `TASKS.md`, `README.md`
  (the cross-link), `docs/` where the sister project is named.
- **Hosts:** the Deck (Proton gate, exe built on the Mac), the Windows PC
  (P2's before and after counts under the Windows PC's work folder, the user's go at run
  time), the Mac.
- **Overlap:** `fix/race-gate-aniso` rebases trivially; P2 and P3 branch
  off b3 `main` d6eab90 now and rebase after it merges. The toolkit guard
  shares `xbox_FramebufferWindowStart` with 316377d (D1). The read-only
  trees `wt/b3jitter`, `wt/present-mode-height`, `wt/present-scanout` and
  the stale `b3/wt/apulife` are not touched.
