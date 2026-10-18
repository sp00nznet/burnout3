# b3-on-xbr: design

## Context

Read at these commits:
- xboxrecomp-cli `main` 4c8d361 (the steamos installer is merged);
- cat `main` e13f817 (pins CLI 4c8d361);
- b3 `main` 79b578d (an upstream mirror; push URL disabled) and
  `b3/fork-glue` 4788d52;
- toolkit `posix-host/portability` eb38e51.

cli-extraction's design D2 drafted Burnout 3's manifest and named three
things to check. Here is how each one turned out:

1. **The `abi_analysis` flags.** On the fork, `tools/abi_analysis` takes
   both `--functions/--identified` and `--disasm-dir/--func-id-dir`
   (`__main__.py` lines 71-81). The two forms point at the same files, so
   the CLI's `abi_cmds` works for Burnout 3 unchanged. Phase 1's gen
   comparison confirms it.
2. **`build.exe_dir = "bin"` does not fit.** `Game.exe()` joins `exe_dir`
   to the build dir, but `src/game/CMakeLists.txt` lines 48-52 put the exe
   in `${CMAKE_SOURCE_DIR}/bin`, in the source tree. A source-tree output
   is also wrong on its own terms: `build-win/`, `build-pkg-win/` and a
   bench build would all write one `bin/burnout3.exe`. D5 fixes this on the
   game side.
3. **The space in `Burnout 3 Takedown`.** Every path the CLI builds on the
   Mac is an argv list, so the Mac side is safe. On the bench host the
   scripts hard-code `game_files/` today, and `bench/sync.py` builds host
   commands by string (`realpath -m %s`, `mkdir -p %s`, `chmod -R`, and
   `dst=%s/game_files`) with the path unquoted. Phase 2 passes the folder
   as a quoted `GAME_FILES` value, quotes every host path, and the parity
   tests gain a case with a space.

More findings from the code:

- **`parse` writes into the dump.** `Game.analysis_json` is
  `<xbe dir>/default_analysis.json` (`manifest.py` line 349). For
  Burnout 3, the xbe dir is the link `Burnout 3 Takedown` →
  `../burnout_3`, the user's dump, which is read-only by rule (the
  untracked `b3/TESTING.md`). `regen.sh` writes `build/xr/analysis.json`
  instead, after `mkdir -p`; the CLI's `stage_parse` makes no directory,
  since cat's analysis sits beside the XBE.
- **The default target ignores `build.targets`.** `default_build_target()`
  returns `macos` on a Mac (`build.py` line 35). `native_target()`
  (`main.py` line 241) does the same for the package default that runs
  when no arguments are given. On the Mac, `./burnout3 build` and
  `./burnout3` would therefore fail with "build.targets has no macos".
- **The stock rule is a constant.** `package/lib.py` `cache_problems`
  refuses any cache with `XBOXRECOMP_ENHANCE` OFF, and `package --help`
  names `XBOXRECOMP_ENHANCE=OFF`. The toolkit's option defaults to OFF
  (`xboxrecomp/CMakeLists.txt` line 43), so a game whose
  `build.stock_cmake` leaves the layer out can never package. That
  constant is cat's choice living in the CLI; D7.0 moves it into the
  manifest.
- **`NOTICE` is required.** `write_common` copies `LICENSE` and `NOTICE`
  without checking for them first. b3 has no `NOTICE`.
- **The packaging launchers fix part of the game's runtime interface.**
  `launch.sh` (the CLI's steamos template) and cat's `launcher.c` set:
  - `RECOMP_GAME_FILES` and `RECOMP_HDD_DIR`;
  - `RECOMP_ENHANCE_CONFIG` and `RECOMP_STDIO_LOG`.

  They also start the exe from the logs folder. b3's `main.c` reads
  `Burnout 3 Takedown\` and `saves\` relative to the working directory
  (lines 55-57, 584-585). The two folder keys are game keys in cat
  (`src/env/recomp_env_game.h`); `RECOMP_STDIO_LOG` is a toolkit key
  (`recomp_env.h` line 88), but the redirect itself is done by the game
  (`cat/src/main.c` lines 992-1027): the toolkit only defines the key.
  b3's `main.c` does neither, so under the bench its output is lost
  (Proton drops the game's stdout and stderr; the local Proton-host script worked
  around that with `cmd /c ... > Z:...`), and a packaged Burnout 3 would
  write empty logs.
- **The crash tag is cat's.** `bench/checks.py` `check_run_end` fails a run
  on a `[CRASH]` line, and `symbolize.sh` parses the `RIP=` and native
  stack lines cat's crash handler prints. Both come from `cat/src/main.c`,
  not the toolkit. b3's `veh_handler` prints `[FAULT] code ... at host ...`
  and names the host symbol itself. Without a change the bench's "no
  crash" verdict on Burnout 3 is vacuous.
- **The bench scripts name cat's files.**
  - `run_game.sh`, `doctor.sh`, `integrate_show.sh` and `symbolize.sh`
    name `build-win/cat_recomp.exe`; `doctor.sh`'s hints say
    `blinx2 bench setup`.
  - `run_game.sh` checks for `game_files/default.xbe` and calls
    `scripts/running_game.py` in the game tree.
  - `gen_digest.sh` changes to `src/recomp/gen`.
  - `build.sh` and `tests.sh` require `cmake/llvm-mingw-x86_64.cmake` in
    the game tree.
  - `game_files.sh` and `sync.py` link the game files as `game_files`.

  This is exactly cli-extraction task 2.3, which is not done yet.
- **`--seconds` and `BENCH_TIMEOUT` race.** `check_run_end` passes a
  limited run only when the exit code is 124 (the bench's SIGINT). b3's
  `--seconds N` makes the game exit 0 at N seconds, which the bench then
  reports as "exit 0 before the limit". Bench runs use `BENCH_TIMEOUT`
  alone.
- **The steamos bundle takes the game's `scripts/running_game.py`** when
  there is one (`steamos.py` lines 71-74), else `install.sh status` falls
  back to `pgrep`. Once `running_game.py --exe` moves into the CLI, the
  bundle gets the CLI's copy, with the exe baked in.
- **The gen banner name.** `recomp` stamps `--game-name` into every
  generated file; without the flag it stamps the XBE certificate's title
  (`tools/recomp/config.py` `banner_name`). The dump's certificate title
  is `Burnout 3`, so `pipeline.game_name = "Burnout 3"` keeps gen/
  byte-identical to `regen.sh`'s.

## Goals / Non-Goals

**Goals**
- `./burnout3 setup | doctor | analyze | recomp | build | bench … |
  package windows | package steamos` work from a fresh b3 checkout. Each
  one reads only `game.toml` and the b3 tree.
- No Burnout 3 constant enters the CLI, and one BLiNX 2 constant leaves it
  (the stock rule). Every CLI change is a manifest key or a parameter, and
  each has a test that uses a synthetic second game.
- The CLI never writes inside the user's dump.
- cat does not change in any way a test can see:
  - its help output and its gen key stay the same;
  - the parity record changes only on the lines phase 2 reviews;
  - its packaged payloads stay byte-identical, except `manifest.json`,
    `SHA256SUMS` and the exe's timestamps.
- Burnout 3 runs on the bench under the standard rules: `bench-logs/`,
  `run-info.txt`, the run lock taken inside the host script, and BLiNX 2
  first.

**Non-Goals**
- A macOS or POSIX Burnout 3, Burnout 3 goldens, Burnout 3 enhancements,
  any public push, any upstream PR.
- Rewriting upstream's `regen.sh` and `Setup.cmd`. They stay as upstream's
  MSVC path (D6).
- Symbolizing Burnout 3 crash reports.

## Decisions

### D1. b3's branch model: the glue goes on b3 `main`

b3 `main` is upstream 79b578d today, tracking `origin/main`. The state that
actually runs is `b3/fork-glue` 4788d52, which has not been merged.

**Recommendation:**
1. Fast-forward b3 `main` to `b3/fork-glue`.
2. Squash-merge this change's `feat/xbr` onto it, as cat does
   (`area: what changed`, the local identity CLAUDE.md names).
3. Upstream stays `origin/main`. b3's own `main` is where the work is
   integrated, and `bench.main_branch = "main"`.

Why: it is cat's model, so `bench integrate` and the post-merge routine
read the same for both games, and there is no second "main" to remember.
The clone can never push (push URL `DISABLED-no-push`), so a diverging
`main` cannot leak. A later upstream PR to `sp00nznet/burnout3` would be
cut from `origin/main`, as the toolkit PRs are; the A/B/M baselines in
the local Proton-host script check out upstream shas in their own worktrees and do
not depend on `main`.

**Alternative:** keep `main` as a pure mirror and integrate on `fork/main`
with `bench.main_branch = "fork/main"`. That is cleaner for upstream
diffs, but it means a second "main" to remember, and `git log origin/main..`
gives the same diff either way.

**User decision U1:** decided (user, 2026-10-06), b3 `main`; see the table.

### D2. cli-extraction phase B is carried out here (U2: yes)

Burnout 3 cannot use the bench or packaging until the host scripts and the
windows templates stop naming cat. That work is cli-extraction tasks 2.1
to 2.4. Doing it here gives phase B what it lacked: a second consumer that
proves the parameters work.

- Phase 2 here = tasks 2.2 and 2.3: the toolchain file, the prologue
  values and the host scripts.
- Phase 3 here = task 2.1 for windows and `README.txt.in`. The steamos
  part is done (4c8d361).
- `running_game.py --exe` (from task 2.3) moves into the CLI in phase 2,
  because `run_game.sh` needs it to name the game's exe; the steamos
  bundle then ships the CLI's copy.
- Task 2.4: `test_launchers.py` moves in phase 3.

cli-extraction's tasks 2.x are then ticked with a pointer to this change.
Its gates G2, G4 and G6 become this change's cat gates.

**Orchestrator decision (2026-10-06): yes.**

### D3. Burnout 3's manifest

```toml
# game.toml: what xboxrecomp-cli needs to know about Burnout 3. See the CLI's
# docs/manifest.md for every key.
schema = 1

[game]
name = "Burnout 3: Takedown"
slug = "burnout3"
env_header = "src/env/recomp_env_game.h"
env_doc = "docs/running.md"

[xbe]
path = "Burnout 3 Takedown/default.xbe"
title_id = 0x4541005B
# The user's dump (burnout_3/, 2026-10-04). The Proton host's copy is checked
# against it in task 1.9.
sha256 = ["9f497acd82adb8edfb72cc31aeeb35010b45d32b13a3f1fc6540717ff24d13f9"]

[cli]
commit = "<the CLI commit that lands each phase>"
url = "https://github.com/apexJCL/xboxrecomp-cli.git"

[toolkit]
url = "https://github.com/apexJCL/xboxrecomp.git"
branch = "blinx2/portability"
commit = "6d51fa2d2b9eae4a31b22022925c6e72ad14dd04"   # cat's pin (D4)

[toolchain]
llvm_mingw = "20260922"
nsis = "3.13"

[pipeline]
# The XBE certificate's title, which recomp stamps into the banners when no
# --game-name is given. Passing it explicitly keeps gen/ byte-identical to
# what regen.sh writes.
game_name = "Burnout 3"
gen = "src/game/recomp/gen"
out = "build/xr"                            # regen.sh's intermediates dir
analysis_json = "build/xr/analysis.json"   # new key (D7.1); never inside the dump
seeds = ["config/seed_functions.json"]
# The shared toolkit's icall_feedback database is BLiNX 2's; regen.sh has
# no icall feedback.
icall_seeds = false
spin_waits = ""
exclude_manual = "src/game/recomp/recomp_manual.c"
split = 1000
names_hooks = []
ghidra = false

[pipeline.disasm]
text_only = false        # regen.sh: every code section; never --text-only
extra_sections = []

[build]
exe = "burnout3"
targets = ["windows"]
windows_dir = "build-win"
macos_dir = "build"      # unused (no macos target); the default
toolchain_file = "cmake/llvm-mingw-x86_64.cmake"   # phase 1: a copy of cat's; phase 2: key removed, the CLI's own
# No enhancements layer: the build every Proton-host run used. With D7.0 the
# stock rule is this list, so a cache without XBOXRECOMP_ENHANCE is stock.
stock_cmake = []
nonstock_vars = []
icon_var = ""

[data]
game_files = "Burnout 3 Takedown"
# dashupdate.xbe is the 58 MB dashboard update on the disc; the title never
# runs it (U6).
game_files_exclude = ["default_analysis.json", "dashupdate.xbe"]
# dir_env, windows, steamos and macos take their defaults from package.app:
# BURNOUT3_DATA_DIR, %LOCALAPPDATA%\Burnout3, ~/Games/Burnout3.

[input]
script_env = "RECOMP_PAD_SCRIPT"
presets = []

[golden]
json = ""                # none yet (D9)

[package]                # from phase 3
app = "Burnout3"
product = "burnout3-recomp"
targets = ["windows", "steamos"]
content = "packaging"
templates = ""           # the CLI's own (phase 3)
icon = "xbe"

[bench]                  # from phase 2
remote_name = ""         # the checkout's basename: <host-tree>/b3
main_branch = "main"
toolkit_branch = "posix-host/portability"
toolkit_tests = false
pacing_scenario = ""
crash_tag = "[FAULT]"    # new key (D7.6): what main.c's veh_handler prints
sync_excludes = ["/bin/", "/runs/", "/_local/", "/saves/", "/Play Burnout 3.cmd", "/setup.log", "/TESTING.md"]   # new key (D7.4)
```

Notes on the choices:
- **The names (U4, decided: `burnout3` / `Burnout3`).** The slug matches
  the exe and the upstream repository's name. The command is
  `./burnout3`. `package.app = "Burnout3"` puts the data in
  `%LOCALAPPDATA%\Burnout3` and `~/Games/Burnout3`, and the steamos
  installer's root variable is `BURNOUT3_ROOT`. Neither name has a space
  or a colon, so it is safe on every target.
- **`stock_cmake = []`.** Every Burnout 3 run so far was built without
  `XBOXRECOMP_ENHANCE`. Turning it on is the enhancements follow-up's job.
  Today the CLI would refuse to package that build (Context); D7.0 makes
  the manifest the rule.
- **`env_doc = "docs/running.md"`.** That file already has b3's
  environment table. CLAUDE.md requires a row for every new key, so the
  new keys get rows there.
- **`sync_excludes`.** `_local/` holds the A/B/M probe results (frames,
  WAVs); `bin/`, `runs/`, `saves/`, `Play Burnout 3.cmd` and `setup.log`
  are upstream-flow output; `TESTING.md` is the local note. rsync does
  not read `.gitignore`, so each is named.

### D4. The toolkit pin: the same as cat's (U3: yes)

Burnout 3 pins the same toolkit commit as cat: the public fork,
`blinx2/portability` 6d51fa2 (= private a6d1d28). It contains the
port-IO and OHCI fixes (70c893b) that the Burnout 3 gate passed on.

- On developer machines, `../xboxrecomp` (the integration checkout) is
  used as it is, exactly as for cat.
- Both games then move their pins together whenever `pins refresh` is run.
  A toolkit change that breaks one game shows up on the next integrate of
  either.
- Pinning separately is possible, since the key is per game. It is only
  worth doing if a fork change ever has to wait on Burnout 3.

**Orchestrator decision (2026-10-06): the same pin.**

### D5. b3 glue (game side, small and upstream-compatible)

1. **`RECOMP_GAME_FILES` and `RECOMP_HDD_DIR`.** They are new rows in
   `src/env/recomp_env_game.h` (`RECOMP_ENV_GAME_KEYS`, the same format as
   cat's). `main.c` reads them through `recomp_env()` when
   `BO3_TOOLKIT_ENV_TABLE` is defined, with defaults `BO3_GAME_DIR` and
   `BO3_SAVE_DIR`. It creates the hard-disk folder with all its parents.
   The XBE path defaults to `<game files>\default.xbe`.
   - On upstream's toolkit (no `recomp_env` target), behaviour is exactly
     what upstream ships.
   - Fork code reads no raw environment variables (`getenv`): the existing
     upstream `getenv` calls in `main.c` stay as upstream wrote them, and
     the new keys go only through the table.
2. **`RECOMP_STDIO_LOG`.** Under the same `#ifdef`, `main.c` does what
   cat's does (`cat/src/main.c` lines 992-1027): when the toolkit key is
   set, truncate the file, `freopen` stdout and stderr onto it in append
   mode, before the first `printf`. The bench's `game-stdio.log`, its
   crash check and the packaged launchers' `logs/` all read that file.
   Upstream's `--record` and the `cmd /c` redirects keep working.
3. **The MinGW block** in `src/game/CMakeLists.txt`:
   - `-Wl,--stack,16777216`, which the local Proton-host script passed by hand and
     MSVC's `/STACK` already sets;
   - `-Wl,--pdb=…` as cat has, so a crash's host symbols resolve.
4. **Where the exe goes.** `RUNTIME_OUTPUT_DIRECTORY` is `bin/` only
   under a Visual Studio generator, which is upstream's documented path
   (README step 5, `Setup.cmd`, `regen.sh`'s last line). With any other
   generator the exe stays in its build dir, so `build.exe_dir` stays empty
   and each build dir owns its own exe.
   - The upstream flow is unchanged.
   - the local Proton-host script's `ls bin/*.exe` no longer applies. The bench
     replaces that script (D8).
5. **`.gitignore`** adds the CLI's outputs, beside upstream's `build/`,
   `bin/`, `saves/`, `*.exe`, `*.pdb`, `*.log` and `.venv/`:
   - `/.venv-ghidra/`, `/external/`, `/third_party/`, `/build-*/`,
     `/dist/`, `/.ruff_cache/`;
   - `/bench-logs`, `/build-logs/`;
   - `src/game/recomp/gen.key.json`, `src/game/recomp/.gen-regenerating`;
   - `/scripts/bench.env`, `/.xbr-pin`.
6. **Packaging content** (`packaging/`, phase 3):
   - **`windows/README.part`:** how to install and play, the pad note
     (XInput only, no keyboard) and the speed note. `steamos/README.part`
     is the CLI's own.
   - **`launch.env.default.windows`:**
     - `RECOMP_PB_BACKEND`, whichever value U5 picks;
     - no `RECOMP_KEYBOARD` (the title reads only the pad) and no
       `RECOMP_USB_PADS`, so the game default (2) applies;
     - `PROTONPATH=GE-Proton`;
     - `LOG_KEEP=10`.
   - **`enhance.toml.default`:** required, since `launch.sh` and
     `launcher.c` copy it on every launch. A comment-only file: this build
     has no enhancements layer, so the keys the toolkit documents do
     nothing here.
   - **`README.intro`** (D7.5): the PRIVATE paragraph for Burnout 3.

### D6. regen.sh, Setup.cmd and setup.ps1 stay as upstream's

They are upstream's documented Windows/MSVC path (README "Quick start" and
"Step by step"), and the upstream gate (TESTING.md) runs upstream as
shipped. Turning them into one-line wrappers would fork the README from
upstream for no gain.

The README gains one section, "Building with ./burnout3 (xboxrecomp-cli)":
- it is the Proton/llvm-mingw path that this fork tests;
- it covers setup, analyze/recomp/build, bench and package.

`regen.sh` and the CLI write the same `gen/` and both keep their
intermediates in `build/xr`. Phase 1's gate B1 proves that, and `regen.sh`
is not changed.

### D7. CLI changes (every one game-neutral, every one defaulting to cat's behaviour)

0. **The stock rule comes from `build.stock_cmake`.** `cache_problems`
   stops naming `XBOXRECOMP_ENHANCE`. Instead, for every `-DVAR=VALUE` in
   `stock_cmake`, a cache whose `VAR` is not `VALUE` is non-stock
   (`VAR=actual (want VALUE)`); `-UVAR` entries are already covered by
   `nonstock_vars`. `package --help`'s `--allow-nonstock` line is rendered
   from the same list: `-DVAR=ON` reads `VAR=OFF`, any other value
   `VAR!=VALUE`. For cat (`-DXBOXRECOMP_ENHANCE=ON`) the refusal text and
   the help are byte-equal to today's; for b3 (`[]`) only
   `CMAKE_BUILD_TYPE` is judged.
1. **`pipeline.analysis_json`** (string, a path in the game; default `""`,
   which means beside the XBE, cat's behaviour today).
   - `Game.analysis_json` uses it; `stage_parse` makes its directory
     (`xbe_parser` does not).
   - The key also feeds the gen key's commands. cat's default leaves the
     command unchanged, so cat's `gen.key.json` does not move (gate C2).
2. **Default targets.**
   - `default_build_target()` returns the host's native target if
     `build.targets` lists it, else the first listed target.
   - The no-argument package default (`native_target()`) returns the
     host's native target if `package.targets` lists it, else the first
     listed target the host can package.
   - `build`'s argparse help names the target picked; the top-level help
     (helptext) is unchanged for cat.
   - On the Mac, cat's results are unchanged: `macos` is listed in both.
3. **`NOTICE` is optional.** It is copied when present. `LICENSE` stays
   required.
4. **`bench.sync_excludes`** (a list, default `[]`). These patterns are
   appended to `GAME_EXCLUDES` after the existing ones. cat's rsync argv is
   therefore unchanged (the parity record has no diff).
5. **`README.txt.in` moves to the CLI** (phase 3). Its game-specific text
   is the PRIVATE paragraph and the "built ... by the blinx2-recomp
   project" sentence. Those become `@NAME@` and `@PRODUCT@`, plus an
   optional `<content>/README.intro` that is inserted verbatim when
   present. cat's rendered README must stay byte-equal, so cat's wording
   moves into `packaging/README.intro`.
6. **Phase 2, the prologue values.** The prologue gains these values,
   filled from the manifest and shell-quoted with `shell_quote` (the folder
   name has a space):
   - `SLUG` (for the hints in `doctor.sh`: `<slug> bench setup`);
   - `EXE` (`burnout3.exe`);
   - `EXE_REL` (`<windows_dir>/<exe_dir>/<exe>.exe`);
   - `GEN_DIR` (`pipeline.gen`);
   - `GAME_FILES` (`data.game_files`);
   - `XBE` (`xbe.path`);
   - `TOOLCHAIN` (the path of the CLI's toolchain file on the host);
   - `CRASH_TAG` (`bench.crash_tag`, default `[CRASH]`).

   The host scripts and the controller change as follows:
   - `run_game.sh`, `doctor.sh`, `integrate_show.sh` and `symbolize.sh`
     use `$EXE_REL` and `$EXE` (the PDB is `$EXE_REL` with `.pdb`);
   - `run_game.sh` checks `"$XBE"`;
   - `gen_digest.sh` changes to `"$GEN_DIR"`;
   - `game_files.sh` links `"$REMOTE_GAME/$GAME_FILES"`; `sync.py` builds
     that `dst`, and every host command it builds by string (`realpath`,
     `mkdir`, `chmod`, the rsync target) quotes its path;
   - `build.sh` and `tests.sh` take `-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"`.
     The CLI rsyncs its tree to `$BENCH_DIR/xboxrecomp-cli/`
     (cli-extraction 2.3), and that is where the toolchain file and
     `running_game.py` live;
   - `run_game.sh` calls `python3 "$BENCH_DIR/xboxrecomp-cli/…/running_game.py"
     --exe "$EXE"`;
   - the `hog` awk filter in `run_game.sh` matches `$EXE`, not
     `cat_recomp`;
   - `checks.check_run_end` and `symbolize.sh` look for the manifest's
     crash tag instead of `[CRASH]`. The address parsing in `symbolize.sh`
     stays cat's report format: for a game with another format the report
     holds the tagged lines and no symbolized frames, and says so;
   - `bench --help`'s `BENCH_GAME_FILES` line renders `data.game_files`.

   `BENCH_GAME_FILES`' default becomes
   `<host-tree>/<remote_name>/<data.game_files>`, which for cat is
   unchanged.
7. **Phase 2, the toolchain file.** `cmake/llvm-mingw-x86_64.cmake` moves
   into the CLI, and `build.toolchain_file` defaults to it. cat's copy and
   its manifest key are deleted once gate C3 passes. Only the header
   comment's mention of `cat_recomp` changes.
8. **Phase 3, the windows templates.**
   - `templates/windows/{launcher.c, installer.nsi.in, app.rc.in}` move
     into the CLI and are filled with `@APP@`, `@NAME@`, `@EXE@` and
     `@DATA_DIR_ENV@`.
   - `launcher.c`'s `BLINX2_NAME` macro becomes `APP_NAME`.
   - `steamos.template()`'s override lookup is generalised to every
     target: `<package.templates>/<target>/<name>` wins over the CLI's
     copy.
   - The steamos bundle ships the CLI's `running_game.py` with the exe
     baked in, instead of the game's `scripts/` copy.
   - The macos templates stay with cat for now (no second consumer). Their
     move is a follow-up.

### D8. The bench for Burnout 3 replaces the local Proton-host script

After phase 2, Burnout 3 runs on the Proton host like this:
- **Configuration** goes in `b3/scripts/bench.env` (untracked):
  - `BENCH_HOST`;
  - `BENCH_GAME_FILES='<host-tree>/burnout_3'`, the dump already on
    the host. The tree then gets a link named `Burnout 3 Takedown`;
  - `BENCH_PREFIX='<host-tree>/prefix-b3'`, the prefix the S6 runs
    used, so cat's prefix is never touched.
- **The three scenes** of the local Proton-host script and `s6-M-r3.md` become
  documented commands in `docs/running.md` and in tasks 2.6. The limit is
  the bench's (`BENCH_TIMEOUT`, SIGINT, exit 124 is the pass), never
  `--seconds` (Context: the two race). For example:

      BENCH_ENV="RECOMP_PAD_SCRIPT=<menu script> RECOMP_FB_DUMP=Z:<host log dir>/frames/f RECOMP_FB_DUMP_FLIPS=300 RECOMP_SAVE_DIR=@run" \
      BENCH_TIMEOUT=240 ./burnout3 bench run --headless

  `RECOMP_SAVE_DIR=@run` (a toolkit key the host script rewrites to a
  fresh `bench-logs/<stamp>/save/`) gives every run a new profile, as
  the local Proton-host script's `saves/` did not. They are not goldens, so the
  bench gives no verdict beyond its run checks: the crash tag, an early
  exit, and a present mismatch (D3D11 only).
- **Frames.** On the CPU backend `RECOMP_FB_DUMP` is the frame source and
  takes a Win32 path (the bench's `Z:` prefix, as the local Proton-host script
  did); `BENCH_FRAMES=1` (`d3d11_dump`) applies to the D3D11 backend only.
  `run-info.txt`'s `flips:` line counts `[D3D11] flip` lines, so it reads 0
  on the CPU backend: a CPU run's frame count is the dumped frames.
- **Logs** land in b3's `bench-logs/`. Per CLAUDE.md there is one store:
  `b3/bench-logs` is a symlink to `xbox-recomp/bench-logs/` (U7). Stamps
  are unique, and `run-info.txt` names the exe.
- **The toolkit on the host** is shared at `<host-tree>/xboxrecomp`,
  since both games pin the same commit (D4). A b3 integrate syncs the
  same `../xboxrecomp` as cat's.
- **The run order** stays as before: one agent at a time, BLiNX 2 first.

the local Proton-host script stays as a local file for the A/B/M upstream
baselines, which use other toolkit trees. The integrated tree uses the
bench.

### D9. Follow-ups (not done here)

- **`b3-golden`:** scenarios for boot, menu and race in a `golden.json`.
  - Pad scripts are inline: Burnout 3 has no `@presets`, so the
    `[input] presets` check must allow an inline script for a game with
    none.
  - Frames come from `d3d11_dump` or `RECOMP_FB_DUMP`, depending on U5.
  - `golden.py` gains an `fb_dump` frame source if the scenes are CPU.
- **macOS Burnout 3**, once `main_posix.c` (TESTING.md baseline B) exists:
  `build.targets += macos` and the macos templates move to the CLI.
- **Burnout 3 enhancements:** `stock_cmake = ["-DXBOXRECOMP_ENHANCE=ON"]`
  and a real `enhance.toml.default`, as the enhancements layer decides.
- **The integrate routine:** whether `burnout3 bench integrate` joins the
  post-merge routine after cat (CLAUDE.md step 5). This is a user decision
  once the bench works.
- **Symbolizing other crash formats**, if a Burnout 3 crash ever needs
  more than its own handler prints.

## Decisions for the user (summary)

| # | Question | Status | Recommendation |
|---|---|---|---|
| U1 | b3's integration branch | decided (user, 2026-10-06): b3 `main`, fast-forwarded to `b3/fork-glue` 4788d52 | b3 `main`, fast-forwarded to `b3/fork-glue`, then the squash merges; upstream stays `origin/main`. It is cat's model, so the bench and the post-merge routine read the same for both games. |
| U2 | Is cli-extraction phase B done here? | decided (orchestrator, 2026-10-06): yes | As phases 2 and 3 (D2). |
| U3 | b3's toolkit pin | decided (orchestrator): the same as cat's | Public `blinx2/portability` 6d51fa2. |
| U4 | Names | decided (orchestrator): `burnout3` / `Burnout3` | Command `burnout3`, app `Burnout3`, product `burnout3-recomp`. |
| U5 | The renderer in the bundle (`RECOMP_PB_BACKEND`) | decided (user, 2026-10-06): `d3d11`, with gate B4 (boot, menus and a race correct on D3D11 under Proton) a must-pass before packaging lands; if D3D11 is broken, stop and report, no silent fallback to `cpu` | Was: `cpu` for the first bundle: it is the only backend any Burnout 3 run has used, and D3D11 on this title is untested. Re-decide after gate B4 (a one-line change in `launch.env.default.windows`): if boot, menu and race are correct on D3D11, ship `d3d11`, since the software raster races at 2-4 fps and D3D11 speed is what matters. |
| U6 | Leave `dashupdate.xbe` out of bundles | decided (orchestrator): yes | 58 MB the title never runs. |
| U7 | bench-logs for Burnout 3 | decided (orchestrator): one shared store | `b3/bench-logs` → `../bench-logs`. benchlog-retention runs from cat only, and its age rule treats Burnout 3 runs like any run without a golden reference. |

## Risks

- **Phase 2 changes a parity record that cat's bench depends on.**
  Mitigation: the record is re-recorded once. Its diff is reviewed line by
  line and may contain only the prologue assignments and the lines D7.6
  names. Then cat runs `integrate --golden` (gate C4) before the cat pin
  moves.
- **D7.0 changes cat's packaging gate.** The rule is the same for cat by
  construction (`-DXBOXRECOMP_ENHANCE=ON` is in its `stock_cmake`), and
  gates C1 (help) and C3 (payloads) prove it. A unit test keeps a cache
  with the layer OFF refused for cat's manifest and accepted for b3's.
- **D3D11 is untested on Burnout 3.** The bench's `[D3D11] flip` checks
  and `BENCH_FRAMES` were made on cat. B4 is the first D3D11 run on this
  title; its result decides U5 and nothing else depends on it.
- **Burnout 3 race audio is intermittently static** (TASKS, b3audio). It
  is unrelated to this change, but a bench run can show it. A run's verdict
  here is boot, menu and race reached. Audio is out of scope.
- **The capstone version.** The b3 lock takes cat's `capstone==5.0.9`,
  while the earlier `regen.sh` runs used the toolkit venv's 5.0.7. Gate
  B1 compares the two drivers with the same venv, so the comparison
  isolates the driver. Any gen difference between capstone versions is
  recorded, not fixed.
- **A shared host toolkit.** A b3 sync from a worktree with a different
  toolkit overwrites cat's host toolkit. The rule stays as for cat's
  agents: a worktree run sets its own `BENCH_DIR`.
- **`remote_name` defaults to the checkout's basename.** `wt/<x>/b3` and
  the main `b3/` both map to `<host-tree>/b3`, as cat's worktrees do;
  the same `BENCH_DIR` rule applies.
