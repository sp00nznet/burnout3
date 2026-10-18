# b3-upstream-prs: our Burnout 3 changes offered to sp00nznet/burnout3

Status: written 2026-10-16 from b3-public-release design D7 and tasks 5.7,
with the xb-upstream session's toolkit facts of the same day. Facts checked
against:
- b3 `main` 4c85e50 = `public/main` (apexJCL/burnout3-recomp, a GitHub
  fork of sp00nznet/burnout3), 24 commits on `origin/main` 79b578d (the
  upstream tip, unchanged on GitHub today; merge base 79b578d);
- upstream xboxrecomp `origin/main` = v0.14.0 eb0aa1d; our open PRs #186
  (head c9c9701) and #192; the central PR #184;
- xboxrecomp-cli `main` e18f129, latest tag v0.7.5 948657e;
- `runs/b3-public/` (window counts, the MSVC runs on the PC against
  upstream's toolkit 193e299) and #186's body draft
  (`wt/up14-186/notes/PRBODY-186.md`);
- trial builds of 2026-10-16 (design D2), in `wt/b3-prs/scratch/`.

The spec lives in cat because b3 has no `openspec/`.

## Why

The user (2026-10-16) wants the Burnout 3 work offered upstream:
- self-contained PRs, each as few commits as possible;
- a draft index PR that lists them, their purpose and review order, and
  says where Burnout 3 stands with our changes;
- the CLI as one optional PR that brings everything it offers and that the
  owner can take or leave with a single merge, with upstream's own setup
  (`Setup.cmd`, `regen.sh`, MSVC, the toolkit at `../xboxrecomp`) still
  working unchanged. Proposing the CLI to xboxrecomp itself is the
  xb-upstream session's part (an issue linked from #184); this change only
  supplies the shared feature list.

What stands in the way today:
- **Upstream Burnout 3 does not boot on upstream's toolkit.** `Setup.cmd`
  clones xboxrecomp `main` (v0.14.0). There 79b578d stalls on a black
  screen at `englis30.xmv`: it calls 0x00014C80, a function packed after
  a switch table, which v0.14.0's disassembler does not find and #186
  finds statically. The same runs log 0x000229E0, 0x00022B30, 0x00022D30
  and 0x00067D10 as unresolved once each (the #186 base runs on the PC
  and the Deck, and gate 2.3's run of 79b578d). `main.c` also forces
  `RECOMP_APU_DSP_ACK=0x825F8810`, the old allocator's address. Since #193
  the toolkit derives the address (`auto` is its default) and the gates
  run with `auto`.
- **Our delta is one stream, not reviewable pieces.** The 24 commits mix
  topics: f3bd1a9 alone touches CMake, `main.c`, the env header, the
  README and game.toml. Several hunks only work with the fork toolkit:
  the env table, the enhancements layer, the D3D11 window rule, spin
  waits and Metal.
- **The packaged launchers need code that only the env-table path has.**
  The CLI's launchers set `RECOMP_GAME_FILES`, `RECOMP_HDD_DIR` and
  `RECOMP_STDIO_LOG`. `main.c` reads all three only under
  `BO3_TOOLKIT_ENV_TABLE`, so on upstream's toolkit a bundle cannot find
  its XBE and writes no log.
- **A MinGW exe built on upstream's toolkit does not start on stock
  Windows 11.** Upstream's `xbox_input` links `xinput`. Under llvm-mingw
  that imports `XINPUT1_3.dll`, which Windows 11 does not ship. The fork
  fixes this in the toolkit (d4b7028); b3 itself does nothing about it.
  MSVC's `xinput.lib` imports `XINPUT1_4.dll`, so `Setup.cmd` is not
  affected.

## What changes

Four topic PRs to `sp00nznet/burnout3` `main` and one draft index PR,
written in upstream's style: imperative commit subjects, a Keep a Changelog
`[Unreleased]` entry, MSVC first (design D1-D4):

| # | Branch | Topic | Commits | Toolkit |
|---|---|---|---|---|
| A | `pr/boot-v0.14` | Seed the six functions v0.14.0 misses; leave the DSP doorbell to the toolkit's `auto` | 1 | v0.14.0 as is (the seeds are redundant but harmless once #186 lands) |
| B | `pr/windows-one-window` | GUI subsystem, `BO3_CONSOLE`, a dialog for failed boot steps, `RECOMP_STDIO_LOG`, `Play Burnout 3.cmd` through `start` | 1 | none |
| C | `pr/portable-build` | llvm-mingw and Clang builds: MinGW stack and PDB, `xinput1_4` under MinGW, no `-Wparentheses-equality` flood from gen, a per-build exe dir off Visual Studio | 1 | none (the xinput rewrite does nothing once the toolkit links 1_4 itself) |
| D | `pr/xbr-cli` (optional) | macOS/Linux host, plus everything the CLI brings: game.toml, `./burnout3`, packaging, the wizard and release workflow, the race gate, and fork-pin extras that do nothing on upstream's toolkit | 2 own (stacked on A, B, C) | pinned to upstream v0.14.0; the fork pin is a one-table switch |
| I | `main` of apexJCL/burnout3-recomp | Draft index: the table, the review order, Burnout 3's status | 0 | n/a |

Not offered: the publish tooling (`scripts/audit-public.sh` and its test),
our CalVer CHANGELOG, the fork README's Progress section and
`docs/images/`. The index links the images on the fork instead. Also not
offered: the TitleCache fix (a toolkit PR from xb-upstream) and the Windows
audio gaps (toolkit side).

The CLI gaps the upstream pin exposes (design D5) go to xboxrecomp-cli as
one small branch, tagged and pushed before D opens.

## Out of scope

- Opening the xboxrecomp CLI issue and any toolkit PR (xb-upstream's
  stack). The fb-window guard is not needed upstream (design D3).
- The Metal and D3D11 backends upstream (#184's GPU series).
- Re-shaping our fork's `main` to match the PRs. It stays as it is; it
  merges upstream's `main` once the PRs land (task 6.3).

## Impact

- **b3:** local branches `pr/boot-v0.14`, `pr/windows-one-window`,
  `pr/portable-build`, `pr/xbr-cli`, built fresh from `origin/main`
  79b578d with the noreply identity. Files: `config/seed_functions.json`,
  `src/game/main.c`, `src/game/CMakeLists.txt`,
  `tools/setup.ps1` (B's launcher line), `docs/running.md`,
  `docs/boot.md`, `README.md`, `CHANGELOG.md`, and for D
  `src/game/recomp/recomp_manual.c`, `CMakeLists.txt` and the CLI files
  listed in design D4.
- **xboxrecomp-cli:** a branch `fix/upstream-toolkit-pin` (design D5) and
  its tag.
- **GitHub (outward, the user's go):** the four branches pushed to
  apexJCL/burnout3-recomp while their PRs are open; four PRs and one
  draft PR on sp00nznet/burnout3.
- **Hosts:** the Windows PC (`b3prs` in its work folder, MSVC and the Mac-built
  MinGW exe), the Deck (Proton, Mac-built exe only), the Mac.
