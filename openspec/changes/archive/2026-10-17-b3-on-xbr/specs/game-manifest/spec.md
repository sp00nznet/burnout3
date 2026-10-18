## ADDED Requirements

### Requirement: A second game needs only its manifest and its own glue
xboxrecomp-cli SHALL build, bench and package a second game, Burnout 3, from that game's `game.toml` and its own tree, with no Burnout 3 constant in the CLI. Every CLI change made for it SHALL be a manifest key or a parameter. Each key SHALL default to the behaviour BLiNX 2 had before, and each SHALL be covered by a test that uses a synthetic second game. The test game has a game-files folder whose name contains a space, a different exe, a different gen dir, a different crash tag, an empty `build.stock_cmake` and a windows-only target list.

#### Scenario: Burnout 3 builds on the Mac
- **WHEN** `./burnout3 setup`, `./burnout3 analyze`, `./burnout3 recomp` and `./burnout3 build` run in a fresh b3 checkout on the Mac
- **THEN** `build-win/burnout3.exe` is produced, nothing is written inside the dump folder, and `gen/` is byte-identical to what `regen.sh --disasm` writes with the same Python environment

#### Scenario: BLiNX 2 is unchanged
- **WHEN** the CLI with the new keys runs cat's commands
- **THEN** cat's help output, its `gen.key.json` and its bench parity record are unchanged, except for the parity lines phase 2 reviews, and its packaged payloads are byte-equal apart from `manifest.json`, `SHA256SUMS`, the exe's timestamps and the `running_game.py` the CLI now ships

### Requirement: The default target comes from the manifest
The default build target SHALL be the host's native target when `build.targets` lists it, and otherwise the first target listed there. The package target used when the command has no arguments SHALL be the host's native target when `package.targets` lists it, and otherwise the first listed target this host can package.

#### Scenario: A windows-only game on a Mac
- **WHEN** `./burnout3 build` runs on a Mac and `build.targets` is `["windows"]`
- **THEN** it builds the windows target with llvm-mingw instead of failing on the macos target

### Requirement: The pipeline never writes into the dump
`pipeline.analysis_json` SHALL name where `parse` writes the XBE analysis. When the key is empty, the analysis SHALL be written beside the XBE, as BLiNX 2 has it today. When the key is set, `parse` SHALL create the file's directory, and the CLI SHALL write nothing inside `data.game_files`.

#### Scenario: Read-only dump
- **WHEN** Burnout 3's `parse` runs and its game-files folder is a link to the user's read-only dump
- **THEN** the analysis is written to `build/xr/analysis.json` and the dump is untouched

### Requirement: The stock build is what the manifest says
`package` SHALL judge a packaging build stock by `CMAKE_BUILD_TYPE=Release`, by `build.nonstock_vars` being empty in the cache, and by every `-DVAR=VALUE` in `build.stock_cmake` holding `VALUE` in the cache. The CLI SHALL hold no fixed variable name in that rule. `package --help` SHALL name the same variables.

#### Scenario: A game built without the enhancements layer
- **WHEN** `./burnout3 package windows` runs with `build.stock_cmake = []` and a Release cache that has no `XBOXRECOMP_ENHANCE`
- **THEN** the build is stock and packaging proceeds without `--allow-nonstock`

#### Scenario: BLiNX 2 keeps its rule
- **WHEN** `./blinx2 package` finds `XBOXRECOMP_ENHANCE=OFF` in its packaging cache and `build.stock_cmake` holds `-DXBOXRECOMP_ENHANCE=ON`
- **THEN** it refuses with the same text as before this change, and `./blinx2 package --help` is byte-equal

### Requirement: Bench host scripts take the game's names from the manifest
The bench host scripts SHALL take the exe, its path in the build dir, the gen dir, the XBE path, the game-files folder, the toolchain file and the crash tag from prologue values that the manifest fills. Each value SHALL be shell-quoted, and every host command the controller builds from a path SHALL quote it. No host script SHALL name a game's file. `bench.crash_tag` SHALL default to `[CRASH]`.

#### Scenario: Burnout 3 on the Proton bench
- **WHEN** `./burnout3 bench sync --game-files`, `build` and `run` run against the bench host
- **THEN** the host tree has a `Burnout 3 Takedown` link to the host's single dump copy, the build makes `build-win/burnout3.exe` under the run lock, the run's `game-stdio.log` holds the game's output, and `run-info.txt` records that exe's sha256 with the b3, toolkit and CLI provenance

#### Scenario: A crash in the game's own format
- **WHEN** a Burnout 3 run's `game-stdio.log` holds a `[FAULT]` line and `bench.crash_tag` is `"[FAULT]"`
- **THEN** `bench run` fails the run as crashed, as it fails a BLiNX 2 run on `[CRASH]`

### Requirement: A packaged game follows the launcher contract
A game that the CLI packages SHALL read its game files from `RECOMP_GAME_FILES` and its emulated hard disk from `RECOMP_HDD_DIR`, and SHALL send its standard output and error to `RECOMP_STDIO_LOG` when that is set. When the launcher sets none of them, the game SHALL fall back to its own defaults. The windows and steamos launchers SHALL set all three, and the game SHALL NOT depend on its working directory to find them.

#### Scenario: Installed Burnout 3
- **WHEN** the steamos bundle is installed into a scratch root and launched
- **THEN** the game boots from `<root>/game_files`, writes its saves under `<root>/hdd`, and its output is in the launch's log under `<root>/logs`
