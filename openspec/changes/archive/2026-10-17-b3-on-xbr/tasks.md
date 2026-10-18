Worktrees: `wt/b3xbr/cat` holds this spec on `spec/b3-on-xbr` (off cat
`main` 0969414; cat `main` is e13f817 now and pins CLI 4c8d361). The
implementation adds:
- `wt/b3xbr/b3` on `feat/xbr` (off b3 `main` once U1 fast-forwards it to
  `b3/fork-glue` 4788d52, else off `b3/fork-glue`);
- `wt/b3xbr/xboxrecomp-cli` on `feat/b3-p1`, then `feat/b3-p2` and
  `feat/b3-p3` (off `main` 4c8d361, or the merge of the previous phase);
- `wt/b3xbr/cat` on `feat/b3-pins` (off `main` e13f817), for cat's pin
  moves and file deletions.

No toolkit worktree: the toolkit is not touched.

Rules for every task:
- Mac builds use `export DEVELOPER_DIR=/Library/Developer/CommandLineTools`.
- Mac runs: none here. Burnout 3 does not boot on the Mac.
- The dump is read-only: nothing writes inside `burnout_3/`, and every
  pipeline gate checks that (`find ../burnout_3 -newer <stamp>` is empty).
- Proton-host runs go through `<game> bench` (it takes the lock itself): one
  agent at a time, BLiNX 2 first, windows and sound allowed.
- The Steam Deck only with the user's go-ahead.
- Evidence goes under `runs/b3xbr/`. Commit identities:
  - b3 and cat: the local identity CLAUDE.md names;
  - CLI: `carlos <4037632+apexJCL@users.noreply.github.com>` as author
    and committer.

## 0. Spec and decisions

- [x] 0.1 Opus drafts proposal, design, tasks and the game-manifest delta on `spec/b3-on-xbr`. `openspec validate b3-on-xbr --strict` passes.
- [x] 0.2 Fable reads and improves the spec (CLAUDE.md step 1; 2026-10-06). Implementation resumes from this revision.
- [x] 0.3 The user answers U1 (b3 branch) and U5 (bundle renderer, after 2.7's B4): U1 b3 `main`; U5 `d3d11`, B4 must pass before phase 3 (2026-10-06). U2, U3, U4, U6 and U7 are decided (orchestrator, 2026-10-06; design.md's table). Record U1 and U5 in the table when they come.
- [x] 0.4 Prerequisite: `feat/steamos-installer` is merged into the CLI's `main` (4c8d361, 2026-10-06). The CLI branches start there.

## 1. Phase 1: Burnout 3 builds through `./burnout3` (CLI `feat/b3-p1`, b3 `feat/xbr`)

CLI (D7.0 to D7.4):
- [x] 1.1 `pipeline.analysis_json`. Add the schema row, a `_paths()` entry, `Game.analysis_json`, and its use in `parse_cmds` and everywhere the path is read: disasm's `--analysis-json` and the gen key. `stage_parse` makes the file's directory before `xbe_parser` runs. Tests:
  - the default keeps cat's path;
  - a set value is used by parse and disasm, and the directory is made;
  - cat's `gen_key` commands, with placeholders, are byte-equal before and after.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI `feat/b3-p1` 50287ad, in the CLI squash 04d491f.
- [x] 1.2 Default targets.
  - `default_build_target()` returns the native target if `build.targets` lists it, else `build.targets[0]`.
  - `native_target()` for the no-argument package default returns the native target if `package.targets` lists it, else the first listed target this host can package. `build`'s argparse help says which target that is; the top-level help is unchanged.
  - Tests: a windows-only game on a fake macOS host builds `windows` and packages `windows`/`steamos`. cat's answers are unchanged on all three hosts.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI `feat/b3-p1` 50287ad, in the CLI squash 04d491f.
- [x] 1.3 The stock rule from `build.stock_cmake` (D7.0): `cache_problems` judges every `-DVAR=VALUE` of the list and no constant; `package --help`'s `--allow-nonstock` line is rendered from it. Tests:
  - cat's manifest: a cache with `XBOXRECOMP_ENHANCE=OFF` is refused with today's text, and `./blinx2 package --help` is byte-equal;
  - the b3-shaped fixture (`stock_cmake = []`): the same cache is stock; a `CMAKE_BUILD_TYPE=Debug` cache is still refused.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI `feat/b3-p1` 50287ad, in the CLI squash 04d491f.
- [x] 1.4 `NOTICE` optional in `write_common`. Test: a game with no `NOTICE` stages without one. cat's payload is unchanged.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI `feat/b3-p1` 50287ad, in the CLI squash 04d491f.
- [x] 1.5 `bench.sync_excludes`: the schema row, appended after `GAME_EXCLUDES` in `game_excludes()`. Tests:
  - cat's rsync argv in `bench_parity.json` is unchanged (no record diff);
  - a set value appears last.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI `feat/b3-p1` 50287ad, in the CLI squash 04d491f.
- [x] 1.6 Test fixture: `tests/testdata/game2/game.toml`, a windows-only manifest in the shape of D3, with a game-files folder whose name has a space, `exe_dir = ""`, a separate `analysis_json`, `stock_cmake = []` and no `[package]`. `test_manifest.py` and `test_cli.py` cases cover it:
  - `doctor`, `parse` and `build` plans on a fake tree;
  - every argv keeps the spaced folder as one argument.
  `docs/manifest.md` rows for the new keys; `uv run pytest tests` and ruff.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI `feat/b3-p1` 50287ad, in the CLI squash 04d491f.

b3 (D5, D6):
- [x] 1.7 Files from cat's templates:
  - `burnout3.py` (`xbr wrapper --print`);
  - `burnout3` and `burnout3.cmd` (cat's, with the name changed; `.gitattributes` already gives `.cmd` CRLF);
  - `game.toml` (D3, with the `[cli]` commit set to 1.1-1.6's head; `[package]` and `[bench]` follow in phases 2 and 3);
  - `pyproject.toml` (cat's tools environment: `name = "burnout3"`, the same pinned capstone and pefile, ruff config with `src/game/recomp/gen` excluded) and `uv.lock`;
  - `config/setup-pins.json` (from `./burnout3 pins refresh`);
  - `cmake/llvm-mingw-x86_64.cmake`, a copy of cat's (removed in 2.5).
  Merged 2026-10-06 (closed in b3-public-release P0): b3 5cf69e5 (squash of `feat/xbr` a671431).

  `./burnout3 wrapper --check` passes.
- [x] 1.8 `.gitignore` additions (D5.5).
  Merged 2026-10-06 (closed in b3-public-release P0): b3 5cf69e5 (squash of `feat/xbr` a671431).
- [x] 1.9 Glue (D5.1 to D5.4), all under `BO3_TOOLKIT_ENV_TABLE`:
  - `RECOMP_GAME_FILES` and `RECOMP_HDD_DIR` rows in `src/env/recomp_env_game.h`;
  - `main.c` reads them (defaults `Burnout 3 Takedown`, `saves`; XBE default `<game files>\default.xbe`; the hard-disk folder made with parents);
  - `main.c` redirects stdout and stderr to `RECOMP_STDIO_LOG` when set, as cat's does, before its first `printf`;
  - the MinGW link block (stack, PDB);
  - `bin/` only under a Visual Studio generator.
  Merged 2026-10-06 (closed in b3-public-release P0): b3 5cf69e5 (squash of `feat/xbr` a671431).

  Add `docs/running.md` rows for the three keys and a note under "The executable". Comments say why (the launcher contract, the bench's log, upstream's MSVC layout). Build once with upstream's toolkit layout in mind: every addition is inside `#ifdef BO3_TOOLKIT_ENV_TABLE` or `if(TARGET recomp_env)`/`if(MINGW)`.
- [x] 1.10 The dump. Check:
  - `Burnout 3 Takedown` → `../burnout_3` has the sha256 in `xbe.sha256` (`9f497acd…`, verified 2026-10-06);
  - its title ID is 0x4541005B and its certificate title `Burnout 3` (verified);
  - the Proton host's copy matches. The orchestrator or the user checks this with `sha256sum <host-tree>/burnout_3/default.xbe` at the next allowed Proton-host session; no agent ssh here.
  Merged 2026-10-06 (closed in b3-public-release P0): the sha256 and title checks are in b3 5cf69e5 (`xbe.sha256`); the Proton host's copy has served every Burnout 3 bench run since.

  If the Proton host's copy differs (TASKS: "re-extracted dump", S6 round 2), list both hashes after comparing their section tables, as cat's 1.13 did.
- [x] 1.11 The README section "Building with ./burnout3 (xboxrecomp-cli)" (D6): prerequisites (uv, git, Python 3), then setup, analyze/recomp, build, and later bench and package as the phases land, and how this differs from upstream's MSVC path.
  Merged 2026-10-06 (closed in b3-public-release P0): b3 5cf69e5 (squash of `feat/xbr` a671431).

Gates (record evidence in the commit messages):
- [x] 1.12 **B1, gen equality.** In `wt/b3xbr/b3`:
  1. Take a stamp (`touch /tmp/b1-stamp`). Run `./burnout3 setup` then `./burnout3 analyze && ./burnout3 recomp`.
  2. Copy `src/game/recomp/gen/` and `build/xr/` aside (regen.sh writes the same `build/xr`).
  3. Run `regen.sh --disasm` with a `py -3` shim that runs `.venv/bin/python` (the same capstone 5.0.9).
  4. The two `gen/` trees are byte-identical, and the two `analysis.json` files match. The only argv differences are `-v` on func_id, abi and recomp and `--quiet` on xbe_parser, which print only; `--game-name "Burnout 3"` equals the certificate title regen.sh lets recomp read.
  5. Nothing is written inside the dump: `find ../burnout_3 -newer /tmp/b1-stamp` is empty.
  Merged 2026-10-06 (closed in b3-public-release P0): B1 was a merge gate of b3 5cf69e5; b3-public-release re-runs it in P2 (its D6).
- [x] 1.13 **B2, build.** `./burnout3 build` on the Mac picks `windows` and produces `build-win/burnout3.exe` and `build-win/burnout3.pdb`, and nothing appears in `bin/`. `./burnout3 doctor` shows the CLI and toolkit pins, the dump as known, and `env_header`/`env_doc` as present. `./burnout3` with no arguments plans `package windows` (and stops at the missing `[package]` with the manifest's message).
  Merged 2026-10-06 (closed in b3-public-release P0): B2 was a merge gate of b3 5cf69e5.
- [x] 1.14 **C1-C2, cat unchanged.**
  - `./blinx2 --help`, `./blinx2 bench --help` and `./blinx2 package --help` are byte-equal before and after (CLI pinned to 1.1-1.6 through `XBOXRECOMP_CLI_DIR`).
  - cat's `gen.key.json` is byte-identical, and `./blinx2` plans `build, package` only.
  - `uv run pytest tests` passes, and ruff is clean in the CLI.
  Merged 2026-10-06 (closed in b3-public-release P0): C1-C2 were merge gates of the CLI squash 04d491f; cat pinned it in c3b9440 (`feat/b3-pins` dc0a5f3).
- [x] 1.15 A Fable review of both branches. Fixes go back to this change's agent. Then:
  - the orchestrator squash-merges the CLI (`manifest: analysis_json, target defaults, the stock rule and sync_excludes for a second game`);
  - fast-forwards b3 `main` per U1 and squash-merges `feat/xbr` (`xbr: build Burnout 3 through xboxrecomp-cli`);
  - moves b3's `[cli]` pin to the CLI merge sha;
  - moves cat's pin only if cat wants these commits now (none affect cat).
  Merged 2026-10-06 (closed in b3-public-release P0): reviewed per phase; CLI 04d491f (`feat/b3-p1` 50287ad, `feat/b3-p2` f42b8ff, `feat/b3-p3` 216f4de), b3 5cf69e5 on `main`, cat c3b9440 (CLI pin 04d491f), toolkit 4773a01 (`fix/b3-d3d11-present` 5bc5fd7, the D3D11 present fix B4 needed). The spec's "e3275fc..c163f96" are not CLI shas; 04d491f is the pin b3 5cf69e5 and cat c3b9440 carry.

## 2. Phase 2: the bench (CLI `feat/b3-p2`; cli-extraction 2.2 and 2.3)

- [x] 2.1 `cmake/llvm-mingw-x86_64.cmake` moves into the CLI (D7.7), and `build.toolchain_file` defaults to it. The only text change is the header comment's mention of `cat_recomp`. `build.py` passes it.
- [x] 2.2 `running_game.py` moves into the CLI with `--exe NAME`, and `test_running_game.py` moves with it. `sync` rsyncs the CLI tree to `$BENCH_DIR/xboxrecomp-cli/` with the toolkit's excludes. `stage_steamos` ships the CLI's copy with the exe baked in (the game's `scripts/running_game.py` is no longer looked for); cat's steamos payload stays byte-equal apart from that file's provenance, which gate C3 lists.
- [x] 2.3 The prologue values and host scripts of D7.6, `tests.sh` included, plus `bench.crash_tag` (schema row, default `"[CRASH]"`, used by `check_run_end` and `symbolize.sh`). `sync.py` quotes every host path it builds and uses `data.game_files` for the link name. `bench --help` renders the exe, `<game>` and the `BENCH_GAME_FILES` default from the manifest (check every line). Re-record `bench_parity.json`. The reviewed diff may contain only:
  - the prologue assignments;
  - the lines of D7.6;
  - the CLI rsync of 2.2.

  Add a parity case for the Burnout 3-shaped fixture (spaced game files, other exe, gen and crash tag), and a `check_run_end` unit test with a `[FAULT]` log under `crash_tag = "[FAULT]"` (fails) and under the default (passes).
- [x] 2.4 `BENCH_GAME_FILES` default and `sync --game-files` with `data.game_files` (D7.6). Tests: cat's default string is unchanged, and the fixture's has the space quoted in every host command.
- [x] 2.5 cat (`feat/b3-pins`): delete `cmake/llvm-mingw-x86_64.cmake`, the `build.toolchain_file` key, `scripts/running_game.py` and `scripts/test_running_game.py`, and move `[cli]` to `feat/b3-p2`'s head. b3: delete its toolchain copy and its key.
- [x] 2.6 b3: `[bench]` in `game.toml` (D3), `scripts/bench.env.example` (the keys from D8; the real file is untracked), and `docs/running.md` "On the Proton bench" with the three scene commands (boot, menu and race scripts from `b3/_local/results/s6-M-r3.md`: Press START appears at about 76 s on the Proton host, so START lands at 85 s), each with `BENCH_TIMEOUT` and no `--seconds`.
- [x] 2.7 Gates on the Proton host (2026-10-06: C4 pass, B3 pass, B4 FAIL: title screen black and race frozen on D3D11; runs/b3xbr/b3/RESULTS.txt), in order, BLiNX 2 first:
  - **C4:** cat `./blinx2 bench integrate --golden` with cat pinned to `feat/b3-p2`. Same verdict and `run-info.txt` fields as the last integrate.
  - **B3:** `./burnout3 bench setup` (no-op), then `sync --game-files` (linking `<host-tree>/burnout_3` as `Burnout 3 Takedown`), `build`, then the three scenes with `run`. Pass:
    - each reaches its milestone (boot: Press START; menu: the garage; race: the car moves under RT), read from the dumped frames;
    - `bench-logs/<stamp>/game-stdio.log` exists and holds the game's output (the `RECOMP_STDIO_LOG` glue), with no `[FAULT]`;
    - `exit-code` is 124 (SIGINT at `BENCH_TIMEOUT` ends the game under umu-run) and `check_run_end` passes;
    - `run-info.txt` names `build-win/burnout3.exe` with b3, toolkit and CLI provenance;
    - a run with a planted `[FAULT]` line in a copied log fails `check_run_end` (unit test, 2.3; no host run needed).
  - **B4 (feeds U5):** one race with `BENCH_ENV=RECOMP_PB_BACKEND=d3d11` and `BENCH_FRAMES=1`, `--headless` so the CPU window stays off (the D3D11 backend opens its own window, which the Proton host allows). Compare frames at matching times with the CPU race (`RECOMP_FB_DUMP`); note whether boot, menu and race render correctly. Record the D3D11 `flips:` count and the CPU run's dumped-frame count over the same limit.
  Merged 2026-10-06 (closed in b3-public-release P0): B4 answered by U5's outcome: after toolkit 4773a01 (one render target per surface) B4 passed (title, menus, race, results; TASKS 2026-10-06), and the bundle ships `RECOMP_PB_BACKEND=d3d11` (`packaging/launch.env.default.windows`); the Deck installs of 2026-10-12 play on it.
- [x] 2.8 Fable review, fixes, squash merges in the CLI and cat, b3's `[cli]` pin. cli-extraction tasks 2.2 and 2.3 are ticked with a pointer here.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI 04d491f (with `feat/b3-p2` f42b8ff), cat c3b9440, b3 `[cli]` pin 04d491f in 5cf69e5. cli-extraction 2.2 and 2.3 point here.

## 3. Phase 3: packaging (CLI `feat/b3-p3`; cli-extraction 2.1 windows and 2.4)

- [x] 3.1 `README.txt.in` moves into the CLI (D7.5): `@NAME@`/`@PRODUCT@` in the PRIVATE and "built by" paragraphs, plus an optional `<content>/README.intro`. cat gains `packaging/README.intro` with its current wording, and the rendered README is byte-equal.
- [x] 3.2 The windows templates move into the CLI (D7.8): `launcher.c` (`APP_NAME`, `@EXE@`, `@DATA_DIR_ENV@`, `@APP@`), `installer.nsi.in`, `app.rc.in`. The per-target override lookup is generalised from `steamos.template()`. `test_launchers.py` moves into the CLI, with templates rendered from a test manifest.
- [x] 3.3 cat (`feat/b3-pins`): delete `packaging/windows/{launcher.c,installer.nsi.in,BLiNX2.rc.in}`, `packaging/README.txt.in` and `scripts/test_launchers.py`, and move `[cli]`. **C3 (= cli-extraction G4):** `./blinx2 package windows|steamos|macos` on the Mac, and `steamos` plus `windows --no-build` in the Proton host's distrobox, give byte-equal payloads to the pre-move CLI. Only `manifest.json`, `SHA256SUMS`, the exe timestamps and (from 2.2) `running_game.py` may differ. Use `scripts/compare-bundles.py`.
- [x] 3.4 b3: `[package]` in `game.toml`, plus `packaging/{windows/README.part, launch.env.default.windows, enhance.toml.default, README.intro}` (D5.6, the renderer per U5), plus the README's package section.
- [x] 3.5 **B5, Mac:** `./burnout3 package windows` and `./burnout3 package steamos`. Check:
  - the plan is `build (build-pkg-win), package` with no refusal (the stock rule, D7.0: the cache has no `XBOXRECOMP_ENHANCE`);
  - the payload lists `Burnout3.exe` (launcher), `burnout3.exe`, `burnout3.pdb`, `game_files/` (no `dashupdate.xbe`, no `default_analysis.json`), `README.txt` naming Burnout 3 and `%LOCALAPPDATA%\Burnout3` / `~/Games/Burnout3`, and the steamos tar also `running_game.py` naming `burnout3.exe`;
  - `manifest.json` has title 0x4541005B, the b3, toolkit and CLI sources, and no private path (the leak check passes);
  - `./burnout3` with no arguments on the Mac packages `windows` (1.2);
  - nothing was written inside the dump (the stamp check).
- [x] 3.6 **B6, Proton host:** (done 2026-10-06; results and deviations: the app is `Burnout3Takedown`, since `Burnout3.exe` overwrote `burnout3.exe` on a case-insensitive filesystem and CLI 216f4de now refuses that; C3 compared the touched files only (README per target, steamos scripts, launcher exe), as the Mac had 2.5 GB free; `README.intro` is the CLI's default for cat (byte-equal), so cat has none; `BLiNX2.rc.in` and `test_launchers.py` stay in cat (CMake icon, macos tests); the steamos bundle was made on the Proton host from git clones with host python3; the scratch root is `~/b3-pkg-test`, since the installer refuses roots under `~/xbox-recomp`; no profile save was made, but the title's UDATA folder was written in `<root>/hdd`)
  - Install the steamos bundle into a scratch root (`install.sh` with `BURNOUT3_ROOT=<host-tree>/b3-pkg-test`), then launch it: Press START is reached, the save lands in `<root>/hdd`, and the game's log in `<root>/logs` has its output.
  - Run the windows installer under umu into a scratch prefix (`/S`), then the installed launcher: Press START is reached, and the saves are in `%LOCALAPPDATA%\Burnout3\hdd`.
  - Remove both scratch roots afterwards with a reviewed one-line command for the user (CLAUDE.md step 6).
- [x] 3.7 **B7, Steam Deck (user go-ahead):** the same steamos install on stock SteamOS (the user's Steam Deck). Record the result.
  Merged 2026-10-06 (closed in b3-public-release P0): the Deck install of 2026-10-12 (Burnout3Takedown 20261012.1309-c56574b4-te5d407e, `~/Games/Burnout3Takedown`; cat TASKS.md "Deck builds installed 2026-10-12"); the user plays on it.
- [x] 3.8 Fable review, fixes, squash merges, pins. cli-extraction tasks 2.1 (windows and README) and 2.4 are ticked with a pointer here. The macos template move is left to D9.
  Merged 2026-10-06 (closed in b3-public-release P0): CLI 04d491f (`feat/b3-p3` 216f4de), cat c3b9440, b3 5cf69e5. cli-extraction 2.4 points here, and 2.1 for its windows and README part; b3 packages macos since b3-macos-host (b3 dfe602f), while the macos template move into the CLI stays a D9 item.

## 4. After the merges

- [x] 4.1 TASKS.md: the Burnout 3 line becomes "on xbr". Record U1-U7, the D9 follow-ups (`b3-golden`, macOS, enhancements, the integrate routine, crash symbolizing) and that the local Proton-host script stays only for upstream A/B baselines.
  Done in b3-public-release P0 (`b3pub/close-b3-on-xbr`): TASKS.md's Burnout 3 line and the D9 follow-ups.
- [x] 4.2 Copy the agent's untracked notes to `notes/b3xbr/` and the referenced runs to `runs/b3xbr/` (`cp -c`). Remove the worktrees and keep the branches.

  Done in b3-public-release P0: no `wt/b3xbr` worktree remained on the Mac (already removed; its runs are in `runs/b3xbr/`), so nothing was left to copy or remove. Branches kept.