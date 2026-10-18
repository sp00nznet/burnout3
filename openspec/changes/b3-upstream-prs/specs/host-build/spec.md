## ADDED Requirements

### Requirement: Burnout 3 builds on the upstream toolkit

Every Burnout 3 change offered to sp00nznet/burnout3 SHALL build on
upstream xboxrecomp's release that its PR names (v0.14.0 today), through
upstream's own route (`Setup.cmd`, `regen.sh`, MSVC, the toolkit at
`../xboxrecomp`), which stays unchanged. Its runtime SHALL be verified on
the toolkit fork (apexJCL/xboxrecomp `blinx2-v0.4.0`), not on upstream
(the user's rule of 2026-10-16). Code that needs the toolkit fork
(the env table, the enhancements layer, the D3D11 and Metal backends,
spin waits) SHALL sit behind a build-time guard that compiles it out when
the toolkit lacks it.

#### Scenario: Upstream route on a PR head

- **WHEN** `Setup.cmd` runs on the head of any of the offered PRs with
  xboxrecomp v0.14.0
- **THEN** `bin\burnout3.exe` configures and links with MSVC

#### Scenario: Runtime on the toolkit fork

- **WHEN** the offered PRs' heads, merged, are built against the fork at
  `blinx2-v0.4.0` and run on Windows 11 or under Proton
- **THEN** the game plays the four boot movies and the log has no
  `[ICALL] Failed to resolve` line

#### Scenario: Fork-only code on the upstream toolkit

- **WHEN** the CLI PR's head is configured against upstream v0.14.0
- **THEN** `build.ninja` defines neither `BO3_TOOLKIT_ENV_TABLE` nor
  `RECOMP_ENV_HAVE_ENHANCE_KEYS`, and the build links with no fork symbol

### Requirement: MinGW builds load the system XInput

A Windows executable linked with llvm-mingw SHALL import `XINPUT1_4.dll`,
not `XINPUT1_3.dll`, whichever toolkit it builds against, so it starts on
a stock Windows 11.

#### Scenario: Cross build against upstream v0.14.0

- **WHEN** the exe is built on the Mac with llvm-mingw against upstream
  v0.14.0
- **THEN** `llvm-objdump -p` lists `XINPUT1_4.dll` and no `XINPUT1_3.dll`

### Requirement: The CLI is optional and pinned by commit

The CLI files SHALL come in one PR that can be merged or left alone
without affecting the other PRs' route. Its `game.toml` SHALL pin the
toolkit by URL, tag and commit, and switching to the toolkit fork SHALL
mean swapping one `[toolkit]` table. With the upstream pin the CLI's
generated code SHALL be byte-identical to `regen.sh`'s for the same
toolkit and seeds. An existing `../xboxrecomp` checkout SHALL be used as
it is.

#### Scenario: Generated code matches regen.sh

- **WHEN** `./burnout3 analyze` and `./burnout3 recomp` run with the
  upstream pin, and separately `regen.sh` runs with the same toolkit
- **THEN** the two `src/game/recomp/gen/` trees are byte-identical

#### Scenario: Toolkit without spin waits

- **WHEN** the pinned toolkit's recomp has no `--spin-waits` option
- **THEN** the CLI skips the option with one warning and the pipeline
  succeeds

#### Scenario: Packaged bundle on the upstream pin

- **WHEN** the Windows installer or the SteamOS bundle built on the
  upstream pin is launched by its launcher
- **THEN** the bundle has no `config.default.toml` and its README says the
  game draws in software

#### Scenario: Packaged bundle on the fork pin

- **WHEN** the Windows installer or the SteamOS bundle built on the fork
  pin is launched by its launcher
- **THEN** the game finds its XBE through `RECOMP_GAME_FILES`, writes its
  log to `RECOMP_STDIO_LOG`'s path and shows one window
