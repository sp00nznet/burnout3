# b3-on-xbr: Burnout 3 builds, tests and packages through xboxrecomp-cli

Status: drafted by Opus, read and revised by Fable (2026-10-06, CLAUDE.md
workflow step 1). Implementation starts from this revision. Facts were
checked against the code at these commits:
- xboxrecomp-cli `main` 4c8d361 (`feat/steamos-installer` 268b2c6 squashed
  onto 8079d86; the branch's later 33b0214 is not merged and is not part of
  the baseline);
- cat `main` e13f817, which pins CLI 4c8d361;
- b3 `main` 79b578d (upstream `sp00nznet/burnout3`, push URL disabled) and
  `b3/fork-glue` 4788d52;
- toolkit `posix-host/portability` eb38e51. The public pin 6d51fa2 is
  private a6d1d28, which contains the Burnout 3 port-IO and OHCI fixes
  (70c893b);
- the dump `burnout_3/`: `default.xbe` sha256 `9f497acd…`, title ID
  0x4541005B, certificate title `Burnout 3` (read, never written).

The spec lives in cat because b3 has no `openspec/` and is a clone of
upstream's game repository.

## Why

BLiNX 2 builds, tests and packages through one command, `./blinx2`. That
command is a bootstrap that runs `xbr` at the commit `cat/game.toml` pins.
Burnout 3 still uses five separate tools:
- upstream's `regen.sh` (bash, `py -3`, hard-coded paths);
- `Setup.cmd` and `tools/setup.ps1` (winget, MSVC, the toolkit clone at
  upstream `main`);
- an MSVC-shaped CMake that writes `bin/burnout3.exe` into the source tree;
- the untracked the local Proton-host script, which has its own ssh, rsync and flock,
  copies cat's llvm-mingw toolchain file by hand and redirects the game's
  output through `cmd /c`;
- no packaging at all.

Burnout 3 is the reference game for upstream toolkit PRs
([[upstream-burnout3-gate]]), so it is run often under Proton. Each of those
runs today takes hand-made steps, unrecorded provenance and a local script
that does not follow the bench rules (`bench-logs/`, `run-info.txt`, the
check for an early exit, the run lock taken inside the host script).

cli-extraction promised that "a third game is a manifest". Burnout 3 is the
test of that promise. It shows what the CLI still assumes about BLiNX 2:
- the bench host scripts name `cat_recomp.exe`, `game_files/`,
  `src/recomp/gen` and `cmake/llvm-mingw-x86_64.cmake` (cli-extraction
  phase B, not done); `bench/sync.py` builds the host's `game_files` link
  name and leaves host paths unquoted;
- the windows templates are cat's (`BLiNX2` is hard-coded in `launcher.c`
  and `installer.nsi.in`), and `README.txt.in` names BLiNX 2;
- `package` judges a build "stock" with a constant: `XBOXRECOMP_ENHANCE=OFF`
  is refused whatever `build.stock_cmake` says, and the toolkit's option
  defaults to OFF;
- `build` defaults to `macos` on a Mac, even for a game that lists only
  `windows`;
- `parse` writes `default_analysis.json` into the dump folder, but
  Burnout 3's dump is a read-only link to the user's `burnout_3/`;
- the bench reads the game's log, crash tag and crash report in the shape
  cat's `main.c` writes them (`RECOMP_STDIO_LOG` redirect, `[CRASH]`), and
  Burnout 3's `main.c` writes neither.

## What changes

**b3** (branch `feat/xbr`, from b3 `main` once U1 fast-forwards it to
`b3/fork-glue` 4788d52, else from `b3/fork-glue`; see D1):
- New files:
  - `game.toml`;
  - the bootstrap `burnout3.py`, a copy of the CLI's `wrapper/game.py`;
  - the `burnout3` and `burnout3.cmd` wrappers;
  - `pyproject.toml` and `uv.lock` (the tools environment: cmake, ninja,
    capstone, pefile);
  - `config/setup-pins.json`;
  - `.gitignore` lines for the CLI's outputs.
- Small glue changes, each inert on upstream's toolkit (no `recomp_env`
  target there):
  - `main.c` reads the game-files and hard-disk folders from
    `RECOMP_GAME_FILES` and `RECOMP_HDD_DIR`, and sends its output to
    `RECOMP_STDIO_LOG`: the launcher contract cat already follows and the
    bench depends on. The two folder keys are new rows in
    `src/env/recomp_env_game.h`; `RECOMP_STDIO_LOG` is a toolkit key.
  - The CMake build gains a MinGW block (a 16 MB stack and a PDB).
  - The exe goes to `bin/` only for upstream's Visual Studio generator.
  - Packaging content: the README parts, `launch.env.default.windows`, a
    stock `enhance.toml.default` and `README.intro`.
- `regen.sh`, `Setup.cmd` and `tools/setup.ps1` stay as upstream ships
  them.

**xboxrecomp-cli** (branches off `main` 4c8d361):
- **Phase 1, small and neutral for cat:**
  - a `pipeline.analysis_json` key, and the pipeline makes its directory;
  - `build.targets` drives the default build target and the no-argument
    package target;
  - the stock-build rule comes from `build.stock_cmake`, not a constant;
  - `NOTICE` becomes optional;
  - a `bench.sync_excludes` key;
  - a test game manifest in the shape of Burnout 3 in the tests.
- **Phase 2, the bench half of cli-extraction phase B:**
  - the prologue carries `SLUG`, `EXE`, `EXE_REL`, `GEN_DIR`, `GAME_FILES`,
    `XBE` and `TOOLCHAIN`;
  - the host scripts and `bench/sync.py` use those values instead of cat's
    names, with every host path quoted;
  - a `bench.crash_tag` key, the line prefix the bench treats as a crash;
  - the llvm-mingw toolchain file and `running_game.py --exe` move into the
    CLI;
  - the parity record is re-recorded, and its diff is only those lines.
- **Phase 3, the packaging half of phase B:**
  - the windows templates (`launcher.c`, `installer.nsi.in`, `app.rc.in`)
    and `README.txt.in` move into the CLI and are filled from `game.toml`;
  - they follow the override pattern the steamos templates set in 4c8d361;
  - cat's `packaging/` keeps only its content.

**cat:** cat moves its `[cli]` pin forward after each CLI phase. It also
deletes its copies of the files that moved (the toolchain file, the windows
templates, `scripts/running_game.py` and `test_running_game.py`), but only
once the byte-equality gates pass. TASKS.md and cli-extraction's tasks 2.x
are marked as carried out here (D2).

## Commands that apply to Burnout 3

| Command | Burnout 3 | Note |
|---|---|---|
| `setup`, `doctor`, `pins refresh` | yes | uv venv, llvm-mingw, NSIS, the toolkit at the pin |
| `parse disasm funcid abi recomp`, `analyze`, `all` | yes | Phase 1 gate B1: gen/ is byte-identical to `regen.sh`'s output |
| `ghidra`, `names` | no | `pipeline.ghidra = false` |
| `build` | `windows` only | A cross build on the Mac, native in the Proton host. No macos target until Burnout 3 has a POSIX host (out of scope). |
| `bench setup/sync/build/run/logs/symbolize/shell/all/doctor` | yes, after phase 2 | The boot, menu and race pad scripts become documented `bench run` lines. |
| `bench integrate` | yes, after phase 2 | `main_branch = "main"` (D1) |
| `bench golden`, `bench pacing`, `golden`, `pacing-stats` | no | Burnout 3 has no golden.json. That is a follow-up (D9). |
| `bench tests` | no | `toolkit_tests = false`: cat already gates the toolkit's Proton tests |
| `package windows`, `package steamos` | yes, after phase 3 | Both need phase 3: `README.txt.in` is read for every target. `package macos`: no |
| `audio-check` | yes | Already game-neutral |

## Out of scope

- A POSIX or macOS host for Burnout 3. It does not boot on the Mac.
- Golden scenarios for Burnout 3 (D9 drafts them as a follow-up change).
- Enhancements for Burnout 3. The bundle is built without the layer, as
  every Proton-host run has been.
- Any public push. b3's push URL stays disabled. The CLI's next push round
  is batched as always.
- Upstream PRs to `sp00nznet/burnout3`.
- Symbolizing Burnout 3's crash reports. Its `main.c` names the host symbol
  itself (dbghelp); the bench's `symbolize` parses cat's report format.

## Impact

- **xboxrecomp-cli:**
  - `manifest.py`, `pipeline.py`, `build.py`, `main.py`,
    `package/lib.py` (the stock rule);
  - `bench/{__init__,config,sync,checks}.py` and `bench/host/*.sh`;
  - `package/{__init__,windows,steamos}.py` and the new
    `package/templates/{windows,README.txt.in}`;
  - the new `cmake/llvm-mingw-x86_64.cmake` and `bench/running_game.py`;
  - `docs/manifest.md` and the tests, including a re-recorded
    `bench_parity.json`.
- **b3:** the new files above, plus `src/game/main.c`,
  `src/env/recomp_env_game.h`, `CMakeLists.txt`, `src/game/CMakeLists.txt`,
  `docs/running.md`, and the README section "Building with ./burnout3".
- **cat:** `game.toml`'s `[cli]` pin; `packaging/` (the moved templates
  are deleted); `cmake/llvm-mingw-x86_64.cmake` and
  `scripts/{running_game,test_running_game,test_launchers}.py` (deleted
  after their phase); `TASKS.md`; cli-extraction's `tasks.md` section 2.
- **Hosts:**
  - the Proton host: a new tree `<host-tree>/b3`, which shares the toolkit at
    `<host-tree>/xboxrecomp` and links the dump already at
    `<host-tree>/burnout_3`;
  - the Steam Deck: one bundle install, with the user's approval.
