# b3-public-release: design

## Context

Burnout 3 (b3) builds, benches and packages through `./burnout3` since
b3 5cf69e5 (b3-on-xbr phase 1-3), boots on the Mac on Metal and packages
as a DMG since dfe602f (b3-macos-host), and has the enhancements layer and
`config.toml` since 37204d3. What it is not yet: a game that looks like one
`xbr new` would write, a one-window program on Windows, a project with a
wizard, releases or a public home. The user wants all four and a
cross-reference between the two games.

Constraints that shape every decision:
- b3 is a clone of `sp00nznet/burnout3` whose `origin` must stay the
  fetch-only upstream: Burnout 3 is the reference game for upstream
  toolkit PRs (TESTING.md, local only), run as upstream ships it
  (`regen.sh --disasm`, MSVC 2022, `bin/burnout3.exe`).
- The CLI is public with the noreply identity; the toolkit's public branch
  is `blinx2/portability` and receives squash commits; cat's public tree is
  an export with no history.
- Game files, frames, audio and saves never leave the user's machines. The
  dump `burnout_3/` is read-only. No screenshots are taken by agents (the
  user); the README's curated images are the one exception CLAUDE.md
  allows, and the user approves them.
- Backends agree: a window behaviour fixed on D3D11 is mirrored or noted
  for Metal; the CPU rasteriser only has to build (and here, still show a
  window on Windows, since upstream's toolkit has no other).

## Goals / Non-Goals

Goals: one window at boot on every host; B3 aligned with the scaffold so a
later `xbr new` diff is small and named; a `[wizard]` table and a
`burnout3-wizard-*` release flow equal to cat's on CLI 0.7.5; a public
fork with an audited publish path; the gates of CLAUDE.md applied to B3
and written down; the two READMEs linking each other's progress; the
upstream PR planned.

Non-goals: regenerating B3 from the scaffold (its `main.c` is 1117 lines
of title-specific host code: the MMIO VEH hook, the hang watchdog,
`--record`, two pads, the AC'97 and DSP doorbell defaults; the scaffold's
`src/main.c` is the start of a port, not a replacement); goldens;
opening the upstream PR in this change; changing upstream's build path.

## Decisions

### D0. Where b3-on-xbr stands, and what closes here

cat's copy of `b3-on-xbr/tasks.md` has 16 boxes ticked and 21 open, but
the open ones are stale:
- Section 1 (CLI `feat/b3-p1`, b3 `feat/xbr`) is merged: b3 5cf69e5 "xbr:
  build, bench and package Burnout 3 through xboxrecomp-cli", and the CLI
  pins moved through e3275fc..c163f96. B1 (gen byte-equal to `regen.sh`)
  and B2 were the merge gates; they are re-run in P2 as a regression gate
  (D6), not redone.
- 2.7's B4 (D3D11 verdict) was answered by U5's later outcome: the bundle
  ships `RECOMP_PB_BACKEND=d3d11` (`packaging/launch.env.default.windows`)
  and the Deck installs of 2026-10-12 play on it (cat TASKS.md).
- 3.7 (Deck install) happened 2026-10-12 (`~/Games/Burnout3Takedown`).
- 2.8, 3.8 (reviews and merges) happened per phase; 4.1 and 4.2 (TASKS
  line, notes copy) are P0 here.

"Uses the xboxrecomp-cli scaffolding" therefore means: the commands are
already `xbr`'s (`setup`, `doctor`, `analyze`, `recomp`, `build`, `bench`,
`package`, `wizard`), the bootstrap `burnout3.py` is `xbr wrapper
--print`'s text at the pin, and what remains is the file set `xbr new`
writes around them (D3) plus the template's host code (D4).

### D1. The three windows, and the fix (P1 + P2)

Facts (toolkit 6c06ae4, b3 d6eab90):

| Window | Opened by | When |
|---|---|---|
| Console | Windows or Wine, for a console-subsystem exe that starts with no console attached | always in the bundle: on Windows `launcher.c` (`-mwindows`) calls `CreateProcessW(..., 0, ...)`; on the Deck `launch.sh` starts the exe through umu-run; b3 links no `WIN32`, entry is `main` |
| GDI framebuffer (`XboxRecompFramebuffer`) | `fb_present.c` `xbox_FramebufferWindowStart`, from `AvSetDisplayMode` (`kernel_bridge.c:2413`) and the executor's first clear (`nv2a_pb_exec.c:1455`); logs `[FBWIN] framebuffer window open` | `RECOMP_FB_WINDOW` set; b3's `main.c:1075` sets it whenever not `--headless` |
| D3D11 (`RecompNv2aD3D11`) | `nv2a_pb_d3d11.c` init on the ack thread; logs `device up: feature level ...` | `RECOMP_PB_BACKEND=d3d11`, the bundle's default; ignores `RECOMP_HEADLESS` and `RECOMP_FB_WINDOW` |

Not involved: the video pump window (`video_pump.c`, only for `.wmv`
under `RECOMP_FMV_HOST`; B3's movies are `.xmv`), the fatal `MessageBoxA`
(only on a failed boot step). On the Mac there is one window on every
backend: `xbox_HostWindowMain` opens the SDL window and Metal blits into
its `CAMetalLayer` (`fb_present_sdl.c:548-571`, `[PRESENT] window up`);
`fb_present.c` is not built on POSIX (`src/video/CMakeLists.txt:9`).

A developer bench run (`./burnout3 bench run`, umu from a shell) shows two
windows (GDI + D3D11); the packaged game shows three. The user saw the
three on both the native Windows PC and the Deck (U7), so P2's first task
reproduces the count on both before changing anything, as text only: on
the Deck `wmctrl -l` or `xdotool search --name` beside the `[FBWIN]` and
`device up` lines in the bench log; on the PC a text listing of the
process's visible top-level windows (`Get-Process burnout3 | Select
MainWindowTitle` plus an `EnumWindows`-style PowerShell listing by process
id) written to a text file under the Windows PC's work folder, with the user's go at run
time; no screenshots. The after count is taken the same way on both.

**The fix is in both places, each for its own reason:**

1. **b3 (P2), the cause.** `src/game/CMakeLists.txt` adds `WIN32` to
   `add_executable(burnout3 ...)`, as the template and cat do, and keeps
   `main(argc, argv)` as the entry: `/ENTRY:mainCRTStartup` under MSVC
   (`target_link_options` guarded by `if(MSVC)`) and nothing extra under
   llvm-mingw, whose CRT starts a `main` program under `-mwindows`. The
   argv parsing (`--headless`, `--record`, `--seconds`, the XBE path) is
   unchanged. A `BO3_CONSOLE` cache option (default OFF) drops `WIN32` for
   terminal debugging, since under the GUI subsystem `cmd` no longer waits
   for the exe and `printf` without `RECOMP_STDIO_LOG` reaches nothing.
   TESTING.md's upstream run lines gain `RECOMP_STDIO_LOG=<file>` or
   `-DBO3_CONSOLE=ON`, and `docs/running.md` says the same.
   `main.c:1075` keeps two branches: without `BO3_TOOLKIT_ENV_TABLE`
   (upstream's toolkit, which has no D3D11 backend) `RECOMP_FB_WINDOW=1`
   whenever not headless, as today; under the table, whenever not headless
   and `recomp_env(RENV_PB_BACKEND)` is not `d3d11` by `_stricmp` (the same
   test `nv2a_pb_d3d11_register_from_env` makes). Not "unset or cpu": a
   `null` or mistyped backend would then give zero windows.
   The `#ifdef BO3_TOOLKIT_ENV_TABLE` block also calls
   `xbox_HostWindowSetTitle("Burnout 3: Takedown")`, which the pin already
   has (`fb_present.c:270`), and `host_report_fatal` with the template's
   dialog replaces the stderr-only failures.
2. **toolkit (P1), the guard.** `xbox_FramebufferWindowStart` gains one
   early return right after `if (!recomp_env(RENV_FB_WINDOW)) return;`:
   `if (!fb_window_wanted(recomp_env(RENV_PB_BACKEND), 1)) { log once;
   return; }`, where the log line is `[FBWIN] RECOMP_FB_WINDOW ignored: the
   d3d11 backend presents in its own window`. `fb_window_wanted(backend,
   fb_window)` returns 0 only for `d3d11` case-insensitively; `metal`
   keeps the window, because on Windows `metal` falls back to the CPU
   rasteriser and suppressing it would give zero windows; `cpu`, `null`
   and unset keep it. The function lives in a file built on every host
   (`src/video/window_title.c`, already in `xbox_video` everywhere, or a
   new `src/video/fb_window_wanted.c` beside it), declared in
   `video_player.h`, so the ctest runs in `tests/posix_ctest.sh` on the
   Mac. The POSIX stub of `xbox_FramebufferWindowStart` stays a no-op;
   Metal needs nothing (one window already), which is the backends-agree
   note.

Why not only the toolkit guard: the console window is b3's subsystem, and
b3's `main.c` would still carry a wrong default for upstream readers. Why
not only b3: cat's bundles never set `RECOMP_FB_WINDOW`, but a player's
`config/launch.env` can, and the toolkit is where the two presenters meet.

**Overlap (corrected after review).** `fix/present-mode-height` 316377d
(contains babd350) edits `xbox_FramebufferWindowStart` itself: after the
`RENV_FB_WINDOW` check it sizes the window from `xbox_GetDisplayMode`.
`fix/occ-reports` d6ae77e changes how reports settle at the flip. P1's
guard is therefore a single early return placed right after the existing
`RENV_FB_WINDOW` check and nothing else in that function, and before the
merge the agent runs `git merge-tree` of the guard branch against 316377d
and d6ae77e; whichever of the three lands second rebases onto the merge.
P1 stays out of `present_pick`, the flip and `nv2a_pb_d3d11.c`.

### D2. Upstream's build path stays; what changes around it

`regen.sh`, `Setup.cmd`, `tools/setup.ps1` and the MSVC CMake path with
`bin/` output are kept as upstream ships them (b3-on-xbr D6 still holds:
they are upstream's documented path and the upstream gate runs them). The
only touch is D1's subsystem, which MSVC honours with the same
`add_executable` line; `BO3_CONSOLE=ON` gives upstream's terminal
behaviour back. P2's gate checks that the MSVC generator still configures
and links, on the Windows PC in the same session as the window count
(D6).

The fork's README leads players to the wizard and releases, and keeps
upstream's "Quick start" and "Step by step" (MSVC) and the "Building with
./burnout3" section for developers. No upstream file is deleted: the
upstream PR (U8, D7) is cut from `origin/main`, and a small, additive
diff keeps that possible.

### D3. Scaffold alignment (P3): diff, do not regenerate

`xbr new` writes: `game.toml`, `<slug>.py`, `<slug>`, `<slug>.cmd`,
`pyproject.toml`, `.gitignore`, `.gitattributes`, `README.md`,
`.github/workflows/release-wizard.yml`, `config/setup-pins.json`,
`uv.lock`, and copies `templates/new-game/{CMakeLists.txt,src/main.c,
src/recomp_manual.c}` with `YOUR_GAME_*` patched. B3 differs by layout
(`src/game/main.c`, `src/game/recomp/gen`, upstream's; `game.toml` names
them, so the CLI needs no change) and by history.

P3 runs `xbr new /tmp/b3-scaffold --slug burnout3 --name "Burnout 3:
Takedown" --xbe "../burnout_3/default.xbe" --offline` with
`XBOXRECOMP_DIR` pointing at the pinned toolkit and the CLI at the 0.7.5
pin, then diffs each scaffold file against b3's:
- **adopt as written:** `.github/workflows/release-wizard.yml` (0.7.5's,
  slug `burnout3`; the file says to keep it equal to the template),
  `.gitattributes`;
- **merge lines:** `.gitignore` (the CLI's outputs), `burnout3.py` (must
  equal `xbr wrapper --print` at the pin; `./burnout3 wrapper --check`),
  `pyproject.toml` (ruff config, the pinned capstone and pefile);
- **keep b3's:** `game.toml` (richer), README, CMake and `main.c` (D4
  names what moves back toward the template), `config/`.
The diff and the verdict per file go in the merge commit's body, so the
next alignment starts from it.

### D4. The template (P1) and what each game keeps

| Piece | Template today | cat | b3 | Decision |
|---|---|---|---|---|
| `RECOMP_STDIO_LOG` redirect | `host_stdio_setup` | same | `stdio_log_setup` | template; b3 keeps its name, same behaviour |
| Crash tag | `[CRASH]` via `crash_printf` | `[CRASH]` | `[FAULT]` via `fault_printf`, `bench.crash_tag` | b3 keeps `[FAULT]` (upstream's gate scripts and TESTING.md grep it) |
| VEH + MMIO hook, POSIX handlers | yes | yes | yes | template |
| Fatal dialog | `host_report_fatal` | yes | stderr only | b3 adopts the dialog (a GUI exe has no console; D1) |
| Env table | `recomp_env_init`, `RENV_*` | yes | `getenv`/`env_set` behind `BO3_TOOLKIT_ENV_TABLE` | b3 keeps the ifdef pair: it must build on upstream's toolkit |
| Enhancements layer (`xbox_enhance_init(".", NULL)`, `enhance_cfg_report_unused`) | none | `host_enhance_init` | `host_enhance_init` | **moves into the template**, under `RECOMP_ENV_HAVE_ENHANCE_KEYS`, plus CMake `if(TARGET xbox_enhance)` link and the `config.default.toml` copy beside the exe, guarded by `if(EXISTS ${CMAKE_SOURCE_DIR}/packaging/config.default.toml)` (the template ships none; `tests/template_main` builds without one) |
| Window title | none | `xbox_HostWindowSetTitle("BLiNX 2")` | none | **moves into the template** as `xbox_HostWindowSetTitle("YOUR_GAME_NAME")`; `scaffold.py:336` already replaces `YOUR_GAME_NAME` everywhere in `src/main.c`; b3 sets "Burnout 3: Takedown" |
| Launcher folder keys `RECOMP_GAME_FILES`, `RECOMP_HDD_DIR` | none | game rows (`recomp_env_game.h:12-13`) | game rows (`recomp_env_game.h:37-38`) | **promoted to toolkit rows** (option (a), below); the template reads them with `YOUR_GAME_DIR` as the default; cat and b3 drop their rows at the pin move |
| Subsystem | `WIN32`, `WinMain` + `main` | same | console | b3 adopts (D1) |
| Metal default on the Mac | yes | yes | yes | template |
| Per-title defaults (`RECOMP_USB_PADS=2`, `RECOMP_AC97_READY`, DSP doorbell, `RECOMP_PB_EXEC`) | `RENV_VBLANK`, `RENV_TITLE_KEVENTS` only | own | own | per game |
| `--headless --record --seconds`, hang watchdog, `BO3_TRAP_NAN` | none | none | yes | per game (upstream's contract) |

**The folder keys: option (a), toolkit rows.** The template has no game
env header (`templates/new-game/` is `CMakeLists.txt`, `src/main.c`,
`src/recomp_manual.c`, `.gitignore`; `tests/template_main` compiles with
none). Two ways to let the template read the keys: (a) two CONFIG rows
`GAME_FILES` and `HDD_DIR` in the toolkit's `src/platform/recomp_env.h`
with generic descriptions, and the identical rows removed from
`cat/src/env/recomp_env_game.h:12-13` and `b3/src/env/recomp_env_game.h:
37-38` in the same pin move (a duplicate X-macro row would not compile,
which is the guard against a missed one); or (b) the scaffold writes
`src/env/recomp_env_game.h`, `game.toml`'s `env_header` and a CMake
include, and `template_main` gets a test header. (a) is chosen: the keys
are set by the CLI's generic launcher for every game (`launcher.c:169-171`,
`launch.sh`, the macOS launcher), so they are the launcher-to-game
contract, which belongs to the toolkit like `RECOMP_STDIO_LOG` and
`RECOMP_CONFIG`; (b) adds three scaffold-written files and a test fixture
to carry two lines. The per-game default folder stays in each `main.c`
(`YOUR_GAME_DIR`, `game_files`, `Burnout 3 Takedown`). `docs/env.md` and
b3's `docs/running.md` rows move with them.

The template change is small (two functions, one title call, one CMake
block, two rows). cat is not changed by P1 beyond the row removal at its
next pin move; a later `cat` alignment of other pieces is a follow-up.

### D5. The wizard (P3)

`game.toml` gains, modelled on cat's (`[wizard]` needs `[package]`, which
b3 has with `windows`, `steamos`, `macos`):

```toml
[wizard]
repo = "https://github.com/apexJCL/burnout3.git"
targets = ["windows", "steamos", "macos"]
# No icon: the CLI's generic gamepad glyph. Burnout, EA and Criterion marks
# and game art are not ours to use.

[[wizard.credit]]
name = "burnout3"
role = "this recompilation of Burnout 3: Takedown for PC and Mac"
license = "MIT"
url = "https://github.com/apexJCL/burnout3"
own = true

# The project this fork continues. credits.py lists a game's non-own
# entries after the CLI's own group, at the head of the third-party list.
[[wizard.credit]]
name = "burnout3 (upstream)"
role = "the original recompilation of Burnout 3, by sp00nz"
license = "MIT"
url = "https://github.com/sp00nznet/burnout3"

[[wizard.option]]
key = "scale"
label = "Render scale"
hint = "Draws the game sharper on a big screen; needs a faster graphics card."
kind = "range"
min = 1
max = 4
default = 1
config = "render.scale"
labels = { "1" = "640×480, as on the Xbox", "2" = "1280×960", "3" = "1920×1440", "4" = "2560×1920, the sharpest" }

[[wizard.option]]
key = "fullscreen"
label = "Start fullscreen"
hint = "Off by default: on the Steam Deck's Desktop Mode, add the game to Steam with the Gamepad template instead."
kind = "bool"
default = false
config = "present.fullscreen"
```

`credits.py` `merged()` orders: the game's `own` entries, then OURS
(xboxrecomp-cli, the toolkit fork, `xboxrecomp (upstream)` "the original
toolkit, by sp00nz"), then the game's other entries, then THIRD_PARTY.
So the wizard's list reads: `burnout3` (own), xboxrecomp-cli, xboxrecomp,
xboxrecomp (upstream), `burnout3 (upstream)`, then Go, Fyne and the rest.
sp00nz is credited twice, in the same words for both projects, and
`wizard config` lists a url once, so there is no duplicate.
`present.pacing` and `render.aniso` stay in `config.default.toml` (no
`choice` kind is needed for the first release). `wizard config` checks
each option's key and default against `packaging/config.default.toml`
(`scale = 1`, `fullscreen = false`: both match today).

**The release flow is CLI 0.7.5's** (`docs/changelog` 2182758, merging
into the CLI's `main` now): its scaffolded `release-wizard.yml` has a
`notes` job that takes the tag's `## [X]` section of `CHANGELOG.md` (the
tag without its `v`) and fails the run before any build when the section
or its text is missing, then the release job publishes the release with
those notes and the four assets (`burnout3-wizard-<tag>-{macos.zip,
windows-x64.zip,linux-x64.tar.gz}`, `SHA256SUMS`), as a pre-release for
a `-rc.` tag. So b3's `[cli]` pin moves to v0.7.5 or later in P3, the
workflow is copied from that scaffold, and b3's `CHANGELOG.md` uses
CalVer sections `## [2026.10.0-rc.1] - 2026-10-DD` (not SemVer), cut from
`[Unreleased]` in the tagged commit (task 5.5a). There is no draft to
publish by hand: the check after the run is that the release exists, is
marked pre-release, carries the notes and the four assets.

### D6. Gates for B3

| Gate | How | Pass |
|---|---|---|
| Mac build | `DEVELOPER_DIR=/Library/Developer/CommandLineTools ./burnout3 build` (macos) and `./burnout3 build windows` | both link; `build/burnout3`, `build-win/burnout3.exe` + `.pdb` |
| Mac run | `SDL_AUDIODRIVER=dummy RECOMP_PAD_SCRIPT=... ./burnout3 ...` to Press START, through `notes/mac-run-lock.sh` | one window (`[PRESENT] window up: CAMetalLayer`), Press START reached, no `[FAULT]` |
| Proton (U5: the Deck) | `./burnout3 bench run` on the Deck (BENCH_HOST from `scripts/bench.env`; the exe is built on the Mac), the boot scene, `scripts/race_gate.py`, and one boot scene with `BENCH_ENV=RECOMP_PB_BACKEND=cpu` | one top-level window (D1's text count) on d3d11; on cpu exactly one `[FBWIN] framebuffer window open` and one window; race gate passes; race flips/s within 25% of the last recorded (57.0-57.6 on D3D11) |
| Native Windows (U7: a P2 gate, the three windows were seen there too) | the packaged bundle under the Windows PC's work folder, before and after P2, the user's go at run time, counted as text (D1) | the count goes from three to one; Press START; the log in `%LOCALAPPDATA%\Burnout3Takedown\logs` |
| Goldens | none (`[golden] json = ""`) | the scenes above stand in |
| Upstream gate | `./burnout3 analyze && ./burnout3 recomp` then `regen.sh --disasm` with the `py -3` shim: `gen/` byte-identical (b3-on-xbr B1); TESTING.md's run lines still work under the GUI subsystem (`RECOMP_STDIO_LOG` set, or `-DBO3_CONSOLE=ON`); the MSVC generator configures and links on the PC with `WIN32` | identical; the runs produce their log; links |
| Toolkit (P1) | `tests/posix_ctest.sh <build-dir>` 0 FAIL on the Mac with the new `fb_window_wanted` ctest in it; `tests/template_main` with `XBOXRECOMP_ENHANCE` ON and OFF on the Mac and in the Proton standalone list; `pytest tools/recomp tools/disasm`; cat's Metal goldens unchanged (cat built against the branch with its two rows dropped); one Proton D3D11 golden | as CLAUDE.md |
| CLI (P1, P3) | `uv run pytest tests`, ruff, `./blinx2 --help` and `./burnout3 --help` byte-equal | as CLAUDE.md |
| Wizard (P3) | `./burnout3 wizard config --dev`, `wizard build --dev --only macos,windows` on the Mac, both binaries `--dry` | builds; the credits list is `burnout3`, the CLI's three, `burnout3 (upstream)`, then third party |

### D7. Publishing: the fork, the identity, the audit, the tags, upstream

**The fork.** The user creates `apexJCL/burnout3` as a GitHub fork of
`sp00nznet/burnout3` (keeps upstream's history, MIT, the fork link),
enables Actions on it and allows workflows write permission (Settings >
Actions > General > Workflow permissions: read and write), since a fork
starts with Actions off and `release-wizard`'s release job needs
`contents: write`. b3 gets a second remote `public` =
`git@github.com:apexJCL/burnout3.git`; `origin` stays `sp00nznet/burnout3`
with its push URL disabled. The fork's `main` is b3's `main`:
fast-forward pushes only.

**Upstream may have moved.** Before any of this, `git fetch origin`. If
`origin/main` is past 79b578d, the fork's `main` starts there and b3's
push is not a fast-forward: the user decides between merging upstream
into b3 `main` and rebasing `origin/main..main` onto it (a prepared
`merge-tree` shows the conflicts, if any); never a force push.

**The identity (U1, answered).** The commits `origin/main..main` are
authored and committed with the private local identity. Before the first push they
are rewritten once, keeping their dates: `git filter-branch
--env-filter` over `origin/main..main` setting `GIT_AUTHOR_NAME/EMAIL` and
`GIT_COMMITTER_NAME/EMAIL` to the noreply identity (`git-filter-repo` is
not installed; a `rebase --exec` with `--reset-author` would reset the
dates), or `git rebase --committer-date-is-author-date` with the four
`GIT_*` variables set, in a scratch clone. Messages are unchanged (U4).
Checks: `git log --format='%ae %ce %ad %cd'` shows only the noreply
address and the original dates; `git diff <old-tip> <new-tip>` is empty.
The old tip is kept as `archive/main-pre-public` before `main` moves.
Moving `main` is a reset to a rewritten history, not a fast-forward: the
orchestrator does it with the user's explicit go, and every branch off
the old shas rebases (`fix/race-gate-aniso` first, with xb-upstream).
`notes/b3-public/sha-map.txt` maps old to new, as
`notes/public-toolkit-map/` does for the toolkit. Since U1 is answered,
P2 and P3 commit as the noreply identity from the start (squash merges
pass `--author` and `GIT_COMMITTER_*`), so only the history before them
is rewritten. CLAUDE.md's "b3: the local identity" line is an
orchestrator or user follow-up; agents never edit CLAUDE.md.

Why not `export-public.sh`: it makes one fresh commit with no history,
right for cat (private history) and wrong for a fork whose value to the
upstream author and to cross-referencing is the commit-by-commit delta.

**The audit.** b3 gains `scripts/audit-public.sh`, the CLI's script with
`public/main..main` as the default range and b3's excludes, run before
every push: private paths, assistant session links, the private patterns from
`.git/info/audit-private` (host names, the personal email), binaries and
game data by extension (`*.xbe *.iso *.bmp *.png *.wav *.sav`), and
`git log --format='%ae %ce'` showing only the noreply address. Two
departures from the CLI's script: the identity check runs on
`origin/main..main` only, never `--all` (sp00nz's commits are upstream's
and stay as they are); and an allow-list for `docs/screenshots/*.png`
(upstream's, kept per U8) and `docs/images/*.png` (ours, D7 below). On
top: `git ls-files` must contain nothing under `_local/`, `Burnout 3
Takedown/`, `saves/`, `runs/`, `bench-logs/`, `dist/`, `build*/`,
`TESTING.md` (gitignored or `.git/info/exclude` today; the audit lists
them as refusals so a future `git add -A` cannot slip them in). The
tracked tree today has no host name or private path (`git grep`,
2026-10-15).

**Tags and releases.** `vYYYY.MM.N-rc.N`, b3's own counter (first:
`v2026.10.0-rc.1` once the wizard round is in), pushed by the user as for
cat, on a commit whose `CHANGELOG.md` has the `## [2026.10.0-rc.1] - date`
section (D5); `release-wizard` (0.7.5) publishes the pre-release with the
notes and the four assets; the check afterwards is on the published
release, not a draft. The game bundle (dump inside) is never an asset.
The CLI's push round goes first when b3's `[cli]` pin moves, so the
pinned sha exists publicly.

**Cross-reference.** A "Progress" section near the top of each README:
b3's replaces upstream's "Status" paragraph (software NV2A, 2-4 fps) with a
Platforms table in cat's shape (Deck/Linux Proton on D3D11, macOS Metal,
Windows: untested natively beyond P2's count), the mode table kept, and a
"Sister project" line to `apexJCL/blinx2-recomp`; cat's README gains the
same line to `apexJCL/burnout3` under "Platforms", exported in the same
push round (U6). Both say what is and is not published (no game files,
frames, audio or saves) and link each other's releases.

**Images (U8).** Upstream's `docs/screenshots/*.png` stay as they are.
Our build gets a small curated set of fresh README images in
`docs/images/` the way cat does: taken fresh for the README under the
CLAUDE.md exception (not copied from test runs; the only images an agent
produces, and only for this purpose), e.g. the title screen and a race on
D3D11 at scale 1 and 2, with the user approving the final set before the
push. The audit allow-lists that path.

**The upstream PR (U8).** After the first public push, a PR to
`sp00nznet/burnout3` cut from `origin/main`, opened only on the user's
go, offering what fits upstream: the README "Progress" augmentation with
`docs/images/`, the one-window fix (D1's `WIN32` + `BO3_CONSOLE` +
`FB_WINDOW` rule, which is inert on upstream's toolkit), `src/game/
CMakeLists.txt`'s MinGW block, and the `RECOMP_GAME_FILES`/`RECOMP_HDD_DIR`
reading (inert without the env table). Not offered: `game.toml`, the
bootstrap, `packaging/`, the wizard (they need the CLI and the fork
toolkit). Tested per CLAUDE.md's upstream-PR rule on the exact PR head:
Burnout 3 on native Windows (MSVC, upstream's toolkit), under Proton and
on macOS (Metal), with the hosts named in the PR. The plan goes in
TASKS.md; opening it is its own change.

**The Deck lesson.** `packaging/windows/README.part` and the steamos part
say: fullscreen is off by default; in the Deck's Desktop Mode add the game
to Steam with the Gamepad template, and quit from the pad through Steam.

### D8. Phases, branches, identities

| Phase | Repo | Branch (worktree) | Identity | Review |
|---|---|---|---|---|
| P0 bookkeeping | cat | `b3pub/close-b3-on-xbr` (`wt/b3-public/cat`) | local | Opus |
| P1 window guard + rows + template | toolkit, then CLI | `fix/fb-window-guard` (`wt/b3-public/xboxrecomp`), `feat/scaffold-title-test` (`wt/b3-public/xboxrecomp-cli`, off `main` at P1's start, 489196b or later) | toolkit local; CLI noreply | Opus |
| P2 one window | b3 | `feat/one-window` (`wt/b3-public/b3`, off `main` d6eab90, rebased after `fix/race-gate-aniso`) | noreply (U1) | Opus |
| P3 scaffold + wizard + changelog + audit | b3 (CLI only on a gap) | `feat/scaffold-wizard` | noreply | Opus |
| P4 publish round | b3, cat | no branch: remotes, rewrite, audit, push, tag, release check, upstream PR plan | noreply | the user's go at each outward step |

Implementation waits for xb-upstream's word on live b3 branches: given
(2026-10-15), `fix/race-gate-aniso` edb0835 is the only one, and it does
not touch P2's or P3's files. The pins move together: P1 lands in the
toolkit and the CLI first; b3's `[toolkit]` pin moves in P2 (with its two
env rows dropped) and `[cli]` to 0.7.5 or later in P3; cat's `[toolkit]`
pin moves with its two rows dropped at its next pin move (nothing else in
P1 changes what cat runs).

## Decisions for the user

All answered by the user on 2026-10-15 (relayed by the orchestrator); the
answers below are as given, and the spec is written to them.

| # | Question | Recommendation | Answer (user, 2026-10-15) |
|---|---|---|---|
| U1 | b3's commit identity: switch to the noreply address (as the CLI), with a one-time rewrite of `origin/main..main` before the first push? | Yes: a fork keeps history, so the history must be publishable. The alternative, squashing into one "fork" commit on 79b578d, loses the per-fix record the cross-reference is for. | Resolved: "rewrite the 19 commits once to the noreply identity before first push, keep per-fix history, plan the sha map." |
| U2 | GUI subsystem by default for b3 (`BO3_CONSOLE=ON` for a terminal), which changes upstream's MSVC build's console behaviour? | Yes: it is the template's and cat's shape, and the cause of the third window. TESTING.md's runs set `RECOMP_STDIO_LOG` or `BO3_CONSOLE=ON`. | Resolved: "GUI subsystem by default, BO3_CONSOLE=ON restores the console." |
| U3 | Wizard icon: the generic gamepad glyph, or an original SVG (no Burnout/EA/Criterion marks, no game art)? | Generic for the first release; an original icon is a one-file follow-up. | Resolved: "generic glyph for the first release." |
| U4 | `BLX-nn` tracker ids in the public commit messages: keep or strip at the rewrite? | Keep: they are not private and the audit does not flag them; stripping rewrites the messages by hand. | Resolved: "keep BLX ids." |
| U5 | The Deck as the Proton gate host for b3 (`scripts/bench.env` BENCH_HOST), with the exe built on the Mac | Yes, as the deck-proton-bench note already does for cat. | Resolved: "Deck as Proton gate host." |
| U6 | cat's README cross-link goes out in the same push round as b3's first push | Yes; both links work the day they appear. | Resolved: "cat cross-link in the same round." |
| U7 | Where were the three windows seen, so P2 measures where it was reported? | (asked in the handback) | Resolved: "Three windows were seen on BOTH the native Windows PC and the Deck: measure before/after on both (Windows under the Windows PC's work folder, with the user's go at run time)." Both counts are P2 gates, as text (the user: do not take screenshots). |
| U8 | Upstream's `docs/screenshots/*.png` and our relation to upstream | (asked in review) | Resolved: "Keep upstream but also augment the way we do with Blinx wherever possible. Ideally this gets sent to upstream." So: upstream's screenshots stay; fresh curated `docs/images/` of our build, user-approved; an upstream PR planned (D7), opened on the user's go. |

## Risks

- **Rewriting b3 `main`** moves every sha the notes and TASKS cite
  (d6eab90 and earlier). Mitigation: the rewrite happens once, right
  before the first push, after P2 and P3 merge; the old tip stays as
  `archive/main-pre-public`, and `notes/b3-public/sha-map.txt` maps the
  shas.
- **GUI subsystem under upstream's toolkit:** a `printf`-only boot with no
  `RECOMP_STDIO_LOG` goes nowhere and `cmd` does not wait. Mitigation:
  `BO3_CONSOLE`, the TESTING.md and `docs/running.md` lines, and the fatal
  dialog (D4) for failed boot steps.
- **The toolkit guard and other backends:** only `d3d11` is suppressed;
  `metal` on Windows falls back to the CPU rasteriser and keeps its GDI
  window; `null` keeps it too. The ctest pins all three.
- **Window count on the Deck** depends on Wine's console behaviour; P2
  measures before and after on both hosts, and the gate is the count, not
  the theory.
- **Upstream moving past 79b578d** before the first push: handled before
  the fork's first push (D7), never with a force push.
- **The fork name** is fixed by GitHub's fork button (`burnout3`); the
  product stays `burnout3-recomp` in `[package]`. The README says both.
