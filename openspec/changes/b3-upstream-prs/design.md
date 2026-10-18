# b3-upstream-prs: design

## Context

Upstream `sp00nznet/burnout3` is 79b578d. Its route is `Setup.cmd` →
`tools/setup.ps1`: it clones `sp00nznet/xboxrecomp` `main` to
`../xboxrecomp` (`$ToolkitRef = 'main'`), runs `regen.sh` (`py -3`, Git
bash) and builds with VS 2022 into `bin\burnout3.exe`, launched by the
generated `Play Burnout 3.cmd`. Its CONTRIBUTING asks for:
- imperative commit subjects with the why in the body;
- C that builds with MSVC;
- `needs xboxrecomp#NN` on a commit that waits for a toolkit change;
- a race and a menu path run before a PR, and nothing generated or
  extracted committed.

There is one other open PR (#2, djultra64, research docs and tools); it
touches only `.gitignore` among our files.

Our fork `main` (4c85e50) builds through xboxrecomp-cli on the toolkit
fork `blinx2/portability` (tag blinx2-v0.4.0). Upstream xboxrecomp v0.14.0
already has the POSIX host pieces our `main.c` uses:
- `xbox_HostWindowMain` and the SDL2 framebuffer window;
- `xbox_PosixMmioFault`;
- `RECOMP_TITLE_KEVENTS`;
- `RECOMP_APU_DSP_ACK` defaulting to `auto` (`apu_dsp.c:78`).

It has none of these (`git grep` on eb0aa1d):
- `recomp_env`;
- `xbox_enhance`;
- `xbox_HostWindowSetTitle`;
- `RECOMP_STDIO_LOG`, `RECOMP_GAME_FILES`;
- `RECOMP_PB_BACKEND` and the D3D11 and Metal backends;
- recomp's `--spin-waits`;
- `fb_window_wanted`.

The rules of `cat/analysis/upstream/pr-stack-plan.md` apply to every PR
here:
- the section 3 template, adapted (D6);
- no `BLX-` anywhere and no private references;
- the noreply author;
- few commits, built fresh from `origin/main`;
- gates on the exact head;
- the description under about 60 lines.

## Goals / Non-Goals

Goals: each PR applies to `origin/main` on its own, or says what it is
stacked on. Upstream's route builds and plays on every head. The CLI is
one optional merge that degrades cleanly on upstream's toolkit. The index
tells the owner what to review first and what Burnout 3 does today.

Non-goals: changing upstream's route; offering fork publishing tools;
toolkit PRs (xb-upstream's).

## Decisions

### D1. Shape: three small PRs, one optional CLI PR on top, one index

A, B and C each branch from 79b578d and touch disjoint hunks. D is
stacked: its branch is A + B + C + its own two commits. Merging D alone
merges everything. Merging A, B or C first only shortens D (it rebases on
`main`, and the duplicated commits drop). This is how "one merge" holds
for the CLI without D duplicating A to C.

Why D stacks instead of standing alone: D's host commit rebuilds `main.c`
around `host_main`, which conflicts with B's `main.c` hunks and A's
doorbell line. Re-authoring D against each order of merges would cost more
than stating "Depends on #A, #B, #C".

Commit count: A 1, B 1, C 1, D 2 of its own (5 on the branch while A to C
are open). The index is the fork's `main`, which has no PR commits of its
own.

### D2. Trial results (2026-10-16, `wt/b3-prs/scratch/`)

- **Fork `main` against upstream v0.14.0, llvm-mingw** (`git archive
  eb0aa1d`, `-DXBOXRECOMP_ENHANCE=ON`): configures. `main.c` and
  `recomp_manual.c` compile with no warning from b3's sources. The link
  misses only gen's symbols (`recomp_dispatch_init`, `recomp_lookup`,
  `xbe_entry_point`). `build.ninja` has no `BO3_TOOLKIT_ENV_TABLE` or
  `RECOMP_ENV_HAVE_ENHANCE_KEYS`: the `if(TARGET ...)` guards keep
  every fork-only block out. The link line has `-lxinput`. In llvm-mingw
  20260922, `libxinput.a` imports `XINPUT1_3.dll` and `libxinput1_4.a`
  imports `XINPUT1_4.dll`.
- **The same on macOS** (Apple clang, CommandLineTools): configures and
  compiles. The link again misses only gen's symbols. So D's host commit
  builds on upstream's SDL host.
- **xinput rewrite** (C's hunk: `get_target_property` on `xbox_input`
  `INTERFACE_LINK_LIBRARIES`, `list(TRANSFORM ... REPLACE "^xinput$"
  "xinput1_4")` under `MINGW`): the link line becomes `-lxinput1_4`. The
  fork's genex entry `$<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>` does not
  match `^xinput$`, so the rewrite does nothing on the fork.
- **Upstream b3 plus C's CMake hunks, llvm-mingw, v0.14.0**: 79b578d's
  `main.c` compiles under Clang. All 74 warnings are the toolkit's. The
  link misses only gen. The `ptrdiff_t` include in `recomp_manual.c` is
  needed on Apple clang, not on llvm-mingw, so it goes in D's host commit.
- **B and C's CMake hunks** (`prs/try-one-window`, `prs/try-build`):
  `git merge-tree --write-tree` is clean.
- Earlier evidence (`runs/b3-public/windows-after.txt`, `pc/`): on the PC
  with MSVC and upstream's toolkit 193e299 (v0.13.1), the GUI exe and the
  `BO3_CONSOLE=ON` exe both link (PE subsystem 2 and 3) and boot.
  Upstream's MSVC route on Windows 11 therefore loads XInput fine.

### D3. Toolkit dependencies

| Need | Upstream v0.14.0 | Consequence |
|---|---|---|
| 0x00014C80 and five more functions found | only with #186 | A seeds all six. Once #186 merges the seeds are redundant and harmless (same starts). A's gate checks that the fork pin's gen is unchanged. |
| A second Windows window suppressed | upstream has no D3D11 backend, so its only Windows presenter is the GDI window, which `RECOMP_FB_WINDOW` opens | No guard is needed. B keeps `RECOMP_FB_WINDOW=1` whenever not headless, as 79b578d does. D's "every backend but d3d11" rule sits under `BO3_TOOLKIT_ENV_TABLE` (fork pin only). The toolkit guard goes upstream with the D3D11 backend (G6), not before. |
| `XINPUT1_4.dll` under MinGW | no | C's rewrite (D2) fixes it from b3. If xb-upstream sends the fork's d4b7028 upstream, the rewrite matches nothing and can go. |
| `recomp_env`, `xbox_enhance`, Metal, D3D11, spin waits | no | D carries them behind `if(TARGET ...)` and `#ifdef` guards. On upstream's pin they do nothing (D4). |
| #192 (pacing) | open, being refreshed | Nothing here depends on it. |
| TitleCache (`E:\CACHE`) | on v0.14.0 the game writes `LocalCache*.bin` into its game folder; sp00nznet/xboxrecomp#206 (head c18d741) moves them to `<save>/TitleCache` | Not a b3 PR. A's body and the index cite it as a known toolkit issue. The fork has it too (70c4371, not public yet), so b3's next fork pin moves the files to `<save>/TitleCache` with no migration. |
| Windows audio gaps | toolkit | Do not block any PR here. |

### D4. What each PR carries

Sources are our `main` commits. Each PR re-authors those hunks onto
79b578d in upstream's style; none is a cherry-pick of a fork commit
(their subjects carry tracker ids and fork context).

**A `pr/boot-v0.14`: "Boot on xboxrecomp v0.14.0: six seeds, auto DSP
ack"** (1 commit, new work).
- `config/seed_functions.json`: six entries, each with a note: 0x00014C80
  ("called through a pointer; packed after a switch table, which v0.14.0's
  disassembler does not follow; the intro movie stalls without it"),
  0x000229E0, 0x00022B30, 0x00022D30, 0x00067D10, and 0x000782C0 (the
  main menu stops advancing once the profile has loaded; found on the
  Deck gate, 2026-10-16).
- `main.c`: drop the `RECOMP_APU_DSP_ACK` default. The comment says the
  toolkit derives the doorbell (`auto`, since xboxrecomp #193) and that
  the value is still settable.
- `docs/boot.md`: rows 10 and 11 (the two stalls and the fix). `CHANGELOG.md`
  `[Unreleased]`: a Fixed entry naming v0.14.0 and xboxrecomp #186.
- It needs v0.14.0: v0.13.1 has no `auto` and hangs at DirectSound start,
  and Setup never updates an existing `../xboxrecomp`. The commit body,
  CHANGELOG and README say so, with `git -C ../xboxrecomp pull`, and cite
  #189 (the allocator change that moved the address). Upstream
  references are written `sp00nznet/xboxrecomp#NN` so they do not
  autolink to burnout3's own issues (Opus review, 2026-10-16).

**B `pr/windows-one-window`: "Build as a Windows GUI program: one window,
a dialog when boot fails"** (1 commit; from f3bd1a9 and 590a93c).
- `src/game/CMakeLists.txt`: `option(BO3_CONSOLE ... OFF)`, `WIN32` on
  `add_executable` unless set, `/ENTRY:mainCRTStartup` under
  `MSVC AND NOT BO3_CONSOLE` (so `main` and `--headless --record
  --seconds` stay).
- `main.c`:
  - `host_report_fatal(step, detail)`: stderr, plus `MessageBoxA` unless
    `--headless` or `RECOMP_HEADLESS` (as the fork), for the XBE load,
    the memory layout failures and an unknown option;
  - closing the window ends the run (Opus review B1): v0.14.0's GDI
    presenter only stops its thread on `WM_CLOSE`, so a GUI build would
    run on invisibly. A thread watches this process's
    `XboxRecompFramebuffer` window and, once it has gone, calls
    `record_finish()` then `ExitProcess(0)`, as the fork's `run_end`. The
    toolkit-side gap is stated in the body and passed to xb-upstream;
  - the load message names the folder;
  - `RECOMP_STDIO_LOG=<path>`, read with plain `getenv`: truncate, then
    reopen stdout and stderr in append mode before anything prints. A GUI
    exe's output otherwise reaches nothing.
- `docs/running.md`: the `BO3_CONSOLE` and `RECOMP_STDIO_LOG` rows, and
  "from a terminal, use one of the two". README "Building": one line.
  `CHANGELOG.md`: Changed.
- `tools/setup.ps1`: the launcher it writes runs `start "" bin\burnout3.exe %*`.
  A batch file waits for a GUI program, so without `start` its console
  stays open and empty beside the game. Setup rewrites the launcher on
  every run.
- Behaviour change, stated in the PR: from a terminal the prompt comes
  back at once and the log goes nowhere unless `RECOMP_STDIO_LOG` or
  `-DBO3_CONSOLE=ON` is used (U1).

**C `pr/portable-build`: "Build with llvm-mingw and Clang"** (1 commit;
from 590a93c and f3bd1a9).
- `src/game/CMakeLists.txt`:
  - `if(MINGW)`: the 16 MB stack and `--pdb` beside the exe;
  - the `xinput1_4` rewrite under `MINGW`;
  - Clang: `-Wno-parentheses-equality` on gen only;
  - `bin/` for every MSVC build (any generator, so VS Open Folder keeps
    it); a MinGW build puts the exe in its build dir;
  - `--pdb` under Clang only (GNU ld has none); a configure WARNING if an
    xinput entry survives the rewrite.
- README: the recipe uses `-B build/mingw` (gitignored) and says gen/
  comes first and that `regen.sh`'s `py -3` needs a shim off Windows.
- README "Building from source": the Ninja recipe (`-DCMAKE_SYSTEM_NAME=Windows
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang`). `CHANGELOG.md`: Added.
- Not in C, since only a non-Windows host needs them and upstream's
  `main.c` is Windows-only before D: the SDK libs under `if(WIN32)`,
  `find_package(OpenSSL)` off Windows, `recomp_manual.c`'s `<stddef.h>`.
  They go in D's host commit. Also not in C: `.gitattributes` and the
  `.gitignore` lines (the CLI's, in D).

**D `pr/xbr-cli` (optional): two commits on A + B + C.**
1. "Run on macOS and Linux through xboxrecomp's SDL host" (from
   00fba7f's `main.c`, and the POSIX build pieces left out of C: SDK libs
   under `if(WIN32)`, `find_package(OpenSSL)` off Windows,
   `recomp_manual.c`'s `<stddef.h>`):
   - `host_main` split out;
   - the POSIX SIGBUS/SIGSEGV report with `dladdr` and the arm64 frame
     walk;
   - `xbox_HostWindowMain`; `env_set`; `BO3_SEP` and `BO3_PATH_MAX`;
   - `--record` and `--seconds` on POSIX;
   - `RECOMP_TITLE_KEVENTS=1` off Windows;
   - `RECOMP_PB_BACKEND=metal` on Apple. Upstream's toolkit ignores it and
     draws with the CPU rasteriser in the SDL window, at the "few fps"
     upstream's README states.
   - `docs/running.md`: the macOS section.
   - Building on a Mac without the CLI needs a Python for `regen.sh`
     (`py -3` is Windows-only), so the PR names the CLI as the Mac route.
     `regen.sh` stays untouched.
2. "Build, package and release through xboxrecomp-cli (optional)":
   - `game.toml` (D5), `burnout3`, `burnout3.cmd`, `burnout3.py`,
     `pyproject.toml`, `uv.lock`, `config/setup-pins.json`;
   - `packaging/**` (README parts, `launch.env.default.*`,
     `config.default.toml`, `macos/`);
   - `.github/workflows/release-wizard.yml`, `.gitattributes`, the
     `.gitignore` block;
   - `scripts/race_gate.py`, `scripts/test_race_gate.py`,
     `scripts/bench.env.example`;
   - `config/spin_waits.json`, its note reworded with no reference to our
     private specs;
   - README "Building with ./burnout3" plus "Players: the wizard";
     `docs/running.md`: the CLI, bench and race gate sections;
     `CHANGELOG.md`: Added.
   - Fork-pin extras, with their guards:
     - `src/env/recomp_env_game.h`;
     - the `if(TARGET recomp_env)` and `if(TARGET xbox_enhance)` CMake
       blocks, `XBOXRECOMP_ENHANCE`, the `config.default.toml` copy;
     - `main.c`'s `BO3_TOOLKIT_ENV_TABLE` blocks (`recomp_env_reload`,
       `recomp_env_flush_notes`, `xbox_HostWindowSetTitle`, the d3d11
       window rule) and `host_enhance_init`.
   - New: `RECOMP_GAME_FILES` and `RECOMP_HDD_DIR` read with plain
     `getenv` when there is no table (with `make_dirs`). Without that, a
     bundle on upstream's pin cannot find its XBE (proposal).

Never offered: `scripts/audit-public.sh`, `scripts/test_audit_public.py`,
`docs/images/`, the fork README's Progress section, the fork `[wizard]`
repo and credits, and the CalVer CHANGELOG text.

### D5. The CLI PR: pin, layout, degradation, CLI changes, feature list

**Pin.** `game.toml` `[toolkit]` names upstream: `url =
"https://github.com/sp00nznet/xboxrecomp.git"`, `tag = "v0.14.0"`,
`branch = "main"`, `commit = "eb0aa1d..."` (full sha at write time).
The CLI clones at a tag and refuses a moved one (`gitpin.resolve`), so
any repo URL works. A commented second `[toolkit]` block gives the fork
pin (`apexJCL/xboxrecomp`, `blinx2/portability`, the current
`blinx2-v*` tag): swapping the two blocks switches on D3D11, Metal, the
enhancements, the env table and spin waits. `[bench] toolkit_branch =
"main"`. `[cli]` is the tag that carries D5's CLI changes. `[wizard]`
`repo` is `sp00nznet/burnout3`, and the one `own = true` credit is
sp00nz's repo. The CLI adds its own group (CLI, toolkit fork, upstream
toolkit) itself.

**Testing with the fork pin** (U2). D's body and `docs/running.md` carry
this recipe, so the owner can check the index's figures:
1. In `game.toml`, comment out the upstream `[toolkit]` table and
   uncomment the fork one:
   ```toml
   [toolkit]
   url = "https://github.com/apexJCL/xboxrecomp.git"
   tag = "blinx2-v0.4.0"
   branch = "blinx2/portability"
   commit = "4e5f603..."   # full sha at write time
   ```
2. Put the fork toolkit where the CLI looks before `../xboxrecomp`, so the
   upstream checkout stays untouched: `git clone --branch blinx2-v0.4.0
   https://github.com/apexJCL/xboxrecomp.git external/xboxrecomp`
   (`toolkit_dir()` takes `external/xboxrecomp` first, and uses a checkout
   it did not clone as it is). Then run `./burnout3 setup`,
   `./burnout3 analyze`, `./burnout3 recomp` (gen is regenerated: the
   fork's lifter and `--spin-waits` differ) and `./burnout3 build windows`
   (or `macos`). Removing `external/xboxrecomp` and restoring the table
   goes back to upstream.
3. What lights up: `RECOMP_PB_BACKEND=d3d11` (Windows, Proton) and `metal`
   (macOS), the enhancements layer (`config.toml`: render scale, filter,
   fullscreen), the env table and spin waits, and the race gate
   (`uv run scripts/race_gate.py`).
4. What to see: one window titled "Burnout 3: Takedown"; the D3D11 log
   line `[d3d11] device up`; Press START, then a race. The race gate
   passes at about 52 flips/s on a Steam Deck under Proton (D3D11) and
   57 on an M-series Mac (Metal), against about 10 fps on
   upstream's GDI presenter (Deck, earlier runs).

The recipe is checked as written on D's head (task 5.6).

**Upstream's layout stays.** `toolkit.toolkit_dir()` takes
`$XBOXRECOMP_DIR`, else `external/xboxrecomp`, else `../xboxrecomp`.
`clone_toolkit()` uses any of them with a `tools/` dir "as it is" and
clones only when none exists. A tree set up by `Setup.cmd` therefore
builds through the CLI on its own `../xboxrecomp`. That checkout is
unmarked, so `doctor` shows no pin note and the pin is not enforced.
`regen.sh`, `Setup.cmd`, `tools/setup.ps1` and the VS generator path are
unchanged in all four PRs. The one exception is B's subsystem (U1).

**How the fork-only parts degrade on upstream's pin:**

| Part | On upstream v0.14.0 |
|---|---|
| env glue, D3D11 window rule, window title | compiled out (no `recomp_env` target) |
| enhancements layer, `config.default.toml`, wizard options `scale` and `fullscreen` | compiled out. The launcher's `RECOMP_CONFIG` is ignored. The wizard options do nothing until CLI change 2 |
| `launch.env.default.windows`'s `RECOMP_PB_BACKEND=d3d11`, the Mac's `metal` | ignored: the CPU rasteriser in the GDI or SDL window (slow) |
| spin waits | recomp has no `--spin-waits`: fails today; skipped after CLI change 1 |
| race gate | needs `RECOMP_DEBUG=audio_wav`, `RECOMP_TRACE=flip` and the `[ENHANCE]` lines: fork pin only, said in `docs/running.md` (upstream's tap is `RECOMP_AUDIO_WAV`; a port is follow-up F1) |
| packaging windows / steamos / macos, the wizard, the release workflow | work. Needs D's plain-getenv folder keys and B's stdio log, and C's xinput rewrite for stock Windows |

**CLI changes** (xboxrecomp-cli `fix/upstream-toolkit-pin`, noreply,
tagged v0.7.6 and pushed in a CLI round before D opens):
1. Probe recomp for `--spin-waits`, as `toolkit_has_seeds()` does for
   `--seeds`. Without it, skip the flag and warn once. Upstream-pin gen
   then equals `regen.sh`'s byte for byte.
2. The wizard leaves out `[[wizard.option]]` entries whose `config` key
   needs the enhancements layer when the toolkit has no
   `src/enhance/CMakeLists.txt`, and `wizard config --json` says why.
3. `doctor` names the toolkit's capabilities (env table, enhancements,
   D3D11, Metal, spin waits), so a degraded build says so.
4. README "Supported games": Burnout 3's link becomes
   apexJCL/burnout3-recomp releases (it says "apexJCL/burnout3 (not
   published yet)").

Anything else the gates find is added to this branch, never worked around
in b3.

**Shared feature list** (used in D's body and in xb-upstream's xboxrecomp
issue; every item has a command):
- `./burnout3` (Unix) and `burnout3.cmd`: a bootstrap that runs the CLI at
  the commit `game.toml` locks. Python 3.9+, nothing installed globally.
- `setup`: pinned, sha256-checked llvm-mingw and NSIS, plus a uv-managed
  venv for the toolkit's tools.
- `analyze` and `recomp`: the toolkit pipeline in `regen.sh`'s order. A
  generation key marks gen/ stale when the XBE, the seeds or the lifter
  change. With the same toolkit and seeds, gen/ is byte-identical to
  `regen.sh`'s.
- `build windows|macos`: cross-builds the Windows exe from macOS or Linux
  with llvm-mingw, and builds natively on the Mac.
- `package windows|steamos|macos`: an NSIS installer, a Steam Deck/Linux
  `.tar` with `install.sh --steam` (Proton through umu), and a `.dmg`.
  The game files stay out of every artifact the CLI publishes.
- `wizard`: a one-download Go/Fyne build wizard per platform for players.
  It checks and installs the toolchain and builds from the player's own
  dump. `release-wizard.yml` publishes it on a version tag with the
  CHANGELOG section and `SHA256SUMS`.
- `bench`: syncs, builds and runs on a remote Proton host and collects
  logs. `scripts/race_gate.py` is a scripted race with checks.
- `doctor`, `import` (a disc image or folder into `Burnout 3 Takedown/`),
  `new` (a scaffold for another title).

### D6. The index draft PR

From `apexJCL/burnout3-recomp:main` to `sp00nznet/burnout3:main`, draft,
never merged (as #184). Title: "Tracking: Burnout 3 changes from
apexJCL/burnout3-recomp (index, not for merge)". Body outline, about 60
lines:
1. One paragraph: what the fork is (sp00nz's project on the CLI and the
   toolkit fork, MIT, credited) and that the index splits it into PRs
   that apply to `main`.
2. **Index table:** PR, topic, depends on, toolkit needed (`v0.14.0` /
   `none`), commits, tested with (hosts, head sha).
3. **Review order:** A (it fixes boot on today's `main`), C, B, then D
   (optional; merging D alone takes A to C with it).
4. **Where Burnout 3 is**, with the run dates (refreshed at open time):
   - **On the toolkit fork** (pin `blinx2-v*`): races on the Steam Deck
     under Proton on D3D11 (51.8 flips/s, D's gates of 2026-10-16) and
     on macOS on Metal (57.1 flips/s, D's re-run). One window on
     Windows 11 and Proton. Release
     v2026.10.0-rc.1 (pre-release, wizard for three platforms).
   - **On upstream xboxrecomp v0.14.0:** a black screen at `englis30.xmv`
     without A or #186. With A, B and C merged (earlier runs, before the
     2026-10-16 rule) it reaches a race on Windows 11 and under Proton
     (GDI presenter, about 10 fps on the Deck, `RECOMP_WIN_PLACEHOLDER=1`
     under Proton).
   - What the fork adds over upstream's toolkit (GPU backends, macOS
     host, pacing) links #184.
   - The README images of the fork build (`docs/images/` on the fork),
     linked, not offered.
5. **The CLI:** two lines and a link to xb-upstream's xboxrecomp issue
   (filled in once it exists).
6. **Provenance and testing:** AI-assisted (Claude Code), our own code,
   no game data. Every PR is tested on its exact head on the hosts it
   names.

Per-PR bodies follow the section 3 template: What and why, Changes,
Tests, Tested with (hosts and exact head sha), Risk and how to back it
out, Provenance. Each starts "Part of #I." and, for D, "Depends on #A,
#B, #C".

### D7. Gates on each exact head

Hosts:
- **PC** (native Windows 11): wait for the orchestrator's PC-free signal.
  Wrap each ssh session in `flock .../pc-run.lock` (Mac helper
  `wt/up14-186/scratch/flock.py`). Work only in `b3prs` in the Windows PC's work folder. Reuse the
  MSVC tools under its `hgpr\tools` or `BuildTools` without
  changing them. Copy the game files from its `hgpr\b3-dump` without
  CACHE. Windows show in the desktop session through the scheduled task
  of `notes/b3-p2/windows-pc-steps.md`. Windows are counted as text; no
  screenshots.
- **Deck** (Proton): exes are built on the Mac only. Use `flock -w 3600
  ~/.recomp-run.lock` and a `b3prs` scratch folder. Never `bench build`, `all` or
  `integrate` there; never touch `~/blx-bench` or
  `~/Games/Burnout3Takedown`.
- **Mac:** `DEVELOPER_DIR=/Library/Developer/CommandLineTools`,
  `SDL_AUDIODRIVER=dummy`, `notes/mac-run-lock.sh`. The dump is used only
  as an APFS clone under `runs/b3-upstream-prs/`.

**Rule (the user, 2026-10-16): Burnout 3 is no longer runtime-tested on
upstream xboxrecomp; runtime always relies on our fork.** On the upstream
route only build checks run: `Setup.cmd`'s MSVC configure and link on
v0.14.0, the llvm-mingw cross build, and the `regen.sh` gen comparison.
Every runtime gate (boot, Press START, race, ICALL counts, one window,
close ends the process, the dialog, `BO3_CONSOLE` output,
`RECOMP_STDIO_LOG`, XINPUT1_4 with a pad, the Deck boot) runs on an exe
built against the fork toolkit (apexJCL/xboxrecomp `blinx2-v0.4.0`, the
pin b3 `main` uses, 4e5f603) with the branches' b3 changes. Upstream
runtime results gathered before the rule stay recorded in tasks.md and
are not re-run. Each body says: "built on xboxrecomp v0.14.0 (MSVC and
llvm-mingw); runtime tested on the xboxrecomp fork (apexJCL/xboxrecomp
blinx2-v0.4.0)".

| Gate | A | B | C | D |
|---|---|---|---|---|
| Build, upstream route on the head (v0.14.0, VS 2022, `Setup.cmd`'s CMake configure and build) | links; `bin\burnout3.exe` | links; GUI exe (subsystem 2) and `BO3_CONSOLE=ON` (3) | links; `bin\burnout3.exe` | unchanged on the full stack |
| Build, Mac llvm-mingw cross build on v0.14.0 | links | links | links; `XINPUT1_4.dll` (`llvm-objdump -p`), PDB, no warnings from b3 sources | `./burnout3 build windows` links |
| gen | `regen.sh` on v0.14.0: the seeds' functions are the only new ones, 0 failed; fork-pin gen unchanged | n/a | n/a | upstream-pin `./burnout3 analyze`/`recomp` gen byte-identical to `regen.sh`'s (`py -3` shim) |
| Runtime, PC (Windows 11): the Mac-built A+B+C exe on the fork toolkit, desktop session | boot to a race with the scripted pad, 0 unresolved | GUI exe: 1 window from `Play Burnout 3.cmd` and started directly; closing the window ends the process; the dialog on a missing XBE and a bad option, none with `--headless`/`RECOMP_HEADLESS`; the console build prints; `RECOMP_STDIO_LOG` and a redirect keep the log; also with `RECOMP_PB_BACKEND=d3d11` | `XINPUT1_4.dll` loads; a race with a pad | installer from `./burnout3 package windows` under `b3prs` in the Windows PC's work folder: 1 window, Press START, log in the data folder |
| Runtime, Deck (Proton): the same exe | race reached, 0 unresolved | 1 window | boots | steamos bundle in a scratch root: 1 window, race reached |
| Mac | n/a (no POSIX glue before D; said in the PR) | n/a | n/a | macOS build; boot to Press START (frames/s recorded); `package macos` makes the DMG |
| D's fork pin (the commented block swapped in, gen regenerated) | n/a | n/a | n/a | Deck race gate on d3d11 within 25% of 51.8 flips/s (race 55.1); Mac Metal race gate within 25% of 57.1 (race 54.4); one window on each |
| scripts | n/a | n/a | n/a | `uv run pytest scripts`, `uv run ruff check . && uv run ruff format --check .`; `./burnout3 wizard config --dev --json`, `wizard build --dev --only macos` runs `--dry` |

D's runtime rows run on the fork pin as well under the rule above; its
upstream pin gets the build checks only.

Also on every head:
- `git merge-tree --write-tree` of A, B and C pairwise against each other
  and against upstream PR #2: clean, or the conflict stated in the body;
- `git log`, diff and body grepped for `BLX-`, `openspec`, `cat/`, host
  names, private paths, the personal email and assistant session links;
- `git log --format='%ae %ce' origin/main..` shows only the noreply
  address.

A gate re-runs after any force-push.

### D8. Branches, identity, order, the user's go

- Branches live in `wt/b3-prs/b3` (later one worktree per writing agent
  under `wt/b3-prs-*`), built from `origin/main` 79b578d after a
  `git fetch origin`. If upstream moved, rebase onto it first.
- Author and committer `carlos <4037632+apexJCL@users.noreply.github.com>`.
  The only trailer is `Co-Authored-By: Claude Opus 5.5
  <noreply@anthropic.com>`: no session line, no session link.
- Subjects in upstream's imperative style (D4). No `BLX-`.
- Order: the CLI tag (D5) public first. Then push the four branches to
  apexJCL/burnout3-recomp, open I as a draft with the table rows marked
  "opening", open A, C, B and D, and fill the PR numbers into I and the
  bodies. Each branch is deleted from the fork when its PR closes.
- The user asked for the PRs and the index. Still needed before opening:
  - U1-U5 answered;
  - an Opus review of the four branches and five bodies;
  - the user's go for the branch pushes to the fork (outward), given once
    for the batch after the gates pass;
  - PC runs only through the PC lock rules above.

## Decisions for the user


| # | Question | Recommendation |
|---|---|---|
Answered by the user on 2026-10-16:

| # | Question | Answer |
|---|---|---|
| U1 | B's default on upstream: GUI subsystem, or console by default with an opt-in? | GUI by default; `-DBO3_CONSOLE=ON` brings the console back. B also makes `Play Burnout 3.cmd` use `start`, so its console closes (D4). |
| U2 | D's `[toolkit]` pin: upstream v0.14.0 or the fork? | Upstream. D's body and `docs/running.md` also give a short exact recipe for testing with the fork pin, so the owner can check that it runs as the index says (D5, "Testing with the fork pin"). |
| U3 | macOS: a commit inside D, or its own PR later? | Inside D. |
| U4 | Offer the fork README's Progress section and `docs/images/`? | No. The index links the fork's README, images and releases. |
| U5 | xinput under MinGW: C's b3-side rewrite, or a toolkit PR? | Both: C now; the coordinator passes d4b7028 to xb-upstream. |

## Risks

- **Upstream moves** (79b578d today): fetch before cutting and before
  opening. A rebase re-runs the gates.
- **#186 lands first:** A's seeds still apply (no-ops). The body says so.
- **The CPU rasteriser is slow on upstream's pin.** The race and boot
  gates check that a scene is reached, not its speed. Only the fork-pin
  gates measure flips/s.
- **The owner merges B or C with squash:** D rebases onto `main`, and its
  duplicated commits drop. The gates re-run.
- **Fork `main` and the PRs diverge** (re-authored hunks, plain getenv for
  the folder keys): fork `main` takes upstream's merged result through a
  merge of `origin/main` once the PRs land (task 6.3), and is not
  rewritten.

## Follow-ups

- F1: `scripts/race_gate.py` on upstream's keys (`RECOMP_AUDIO_WAV`,
  `RECOMP_FB_DUMP`), so the gate runs on either pin.
- F2: once the toolkit links `xinput1_4` under MinGW, drop C's rewrite.
- F3: `main.c` passes `RECOMP_GAME_FILES` and `RECOMP_HDD_DIR` from
  `getenv`, which on Windows reads them in the ANSI code page, while the
  toolkit's `kernel_path.c` converts `game_dir`, `save_dir` and
  `RECOMP_SAVE_DIR` with `MultiByteToWideChar(CP_UTF8)`. A non-ASCII
  Windows path therefore breaks. Fix: read them with `_wgetenv` or
  `GetEnvironmentVariableW`, or convert with `CP_ACP`. Not part of B.
- F4: upstream's GDI presenter stops only its own thread on `WM_CLOSE`,
  and the pushbuffer executor reopens the window on its next clear. B's
  watcher covers it from the game side; a toolkit fix (`WM_CLOSE` ending
  the run, as the fork's presenters already do) makes the watcher
  redundant. Passed to the xb-upstream agent.
