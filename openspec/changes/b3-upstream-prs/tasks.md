# b3-upstream-prs: tasks

Worktrees: `wt/b3-prs/cat` on `spec/b3-upstream-prs` (this spec, off cat
`main`). Implementation, one agent per worktree:
- `wt/b3-prs/b3`, one branch at a time (`pr/boot-v0.14`, then
  `pr/portable-build`, `pr/windows-one-window`, `pr/xbr-cli`), or one
  worktree per branch under `wt/b3-prs-<x>/b3` if agents run in parallel.
  All are cut from `origin/main` after a fetch (79b578d today), with the
  noreply identity from the first commit. The scratch branches
  `prs/try-*` are trial material, never pushed.
- `wt/b3-prs/xboxrecomp-cli` on `fix/upstream-toolkit-pin` (off CLI
  `main` e18f129 or later; noreply identity).

Rules for every task:
- Mac builds with `DEVELOPER_DIR=/Library/Developer/CommandLineTools`.
  Mac runs go through `notes/mac-run-lock.sh` with `SDL_AUDIODRIVER=dummy`.
- The dump `burnout_3/` is read-only; runs use an APFS clone (`cp -c -R`)
  under `runs/b3-upstream-prs/`.
- The PC and Deck rules are design D7's.
- No `BLX-` and no private references in any branch, commit or body.
- No push, PR or GitHub write without the user's go (D8). Read-only `gh`
  is fine.
- Evidence goes under `runs/b3-upstream-prs/<pr>/`.

## 0. Spec

- [x] 0.1 Proposal, design, tasks and the host-build delta on
  `spec/b3-upstream-prs`; `openspec validate b3-upstream-prs --strict`
  passes.
- [x] 0.2 The user answers U1-U5 (design's table, 2026-10-16): GUI default; upstream pin plus a fork-pin test recipe; macOS inside D; no Progress or images; xinput both ways.
- [ ] 0.3 Opus review of the spec. Its fixes go back to this branch.

## 1. CLI: `fix/upstream-toolkit-pin` (xboxrecomp-cli)

- [x] 1.1 Probe recomp's `--help` for `--spin-waits` (as
  `toolkit_has_seeds`). Skip the flag with one warning when it is
  missing. Add a test with a fake toolkit that lacks it.
- [x] 1.2 The wizard leaves out options whose `config` key needs
  `src/enhance/` when the toolkit has none. `wizard config --json` gives
  the reason. Add tests.
- [x] 1.3 `doctor` prints the toolkit's capabilities (env table,
  enhancements, D3D11, Metal, spin waits, seeds). Add a test.
- [x] 1.4 README: Burnout 3's row links apexJCL/burnout3-recomp releases.
- [x] 1.5 Gates: `uv run pytest tests`, ruff check and format,
  `wizard/check.sh` if wizard Go code changed. BLiNX 2 `blinx2 golden`
  on Metal is unchanged on the fork pin (cat's CLI pin moved in a
  worktree). Done 1.1-1.5: CLI `main` 2a4de72 ("release: 0.7.6").
- [ ] 1.6 (the gate before D opens) Opus review, squash merge to CLI `main` with the noreply author
  and committer, tag v0.7.6 with its CHANGELOG section, then a CLI push
  round (audit, fast-forward), all with the user's go. The GitHub
  Release goes with the tag.

## 2. A: `pr/boot-v0.14`

- [x] 2.1 One commit, "Boot on xboxrecomp v0.14.0: six seeds, auto DSP
  ack", as design D4. Done: b3 `pr/boot-v0.14` d3d7b52, amended after the
  Opus review (S1, S2, N9, N10) to f8350cb, then to 67771a2 with the sixth
  seed (0x000782C0: the Deck gate on f8350cb stopped at the main menu once
  the profile loaded, 10,251 failed calls); body draft `bodies/A.md`.
- [ ] 2.2 gen: on v0.14.0, `regen.sh` finds the six, with 0 failed. On the
  fork pin, gen is unchanged (`git diff --stat` of gen empty). Done for
  v0.14.0 on the Mac (2026-10-16, `runs/b3-upstream-prs/mac/A6.regen.txt`):
  23,655 functions against 23,648 without the seeds, 0 failed, the same 3
  unresolved stubs; the new ones are the six and 0x000780D0, which
  0x000782C0 calls. Open: the fork-pin check. Fork `main` does not seed
  the six; it relies on the fork lifter finding them.
- [x] (upstream runtime, recorded before the 2026-10-16 rule) 2.3 PC: the upstream route on the head (D7): race reached, 0
  `[ICALL] Failed to resolve` in a 360 s log. One run with
  `RECOMP_APU_DSP_ACK=0x825F8810` for comparison, recorded.
  Done (2026-10-16, `runs/b3-upstream-prs/pc/`, MSVC builds on Windows
  11): A 67771a2 headless (`a-auto6`): the four movies, the main menu,
  `auto` ack, 0 unresolved, 0 faults in 420 s (A alone with its window
  was dropped under the rule below). The
  A+B+C exe (`abc-race6`, sha 10c005e6): race (`tracks\US\C2_V1\
  streamed.dat`), 0 unresolved, 0 faults. With 0x825F8810 (`a-old`, on
  f8350cb) and on origin/main (`o-base`): no movie, stuck at DirectSound
  start with `[XA2] underrun` repeating. On f8350cb the GUI runs logged
  0x000782C0 thousands of times (`b-boot` 2,828, `abc-race` 6,372).
- [x] (upstream runtime, recorded before the 2026-10-16 rule) 2.4 Deck: Mac-built MinGW exe (C's xinput hunk not needed under
  Proton) on v0.14.0, `auto`: race reached, 0 unresolved. Record whether
  `RECOMP_WIN_PLACEHOLDER=1` was needed.
  Done (2026-10-16, GE-Proton 11-9, `runs/b3-upstream-prs/deck/`): `abc4`
  (A's six-seed gen, exe 55ca09ea, which differs from 10c005e6 only in
  B's watcher): the four movies, race (C2_V1 `streamed.dat`), 0
  unresolved, 0 faults, `auto` ack. `RECOMP_WIN_PLACEHOLDER=1` is needed:
  without it (`abc2-noplaceholder`) mirrors 26/27 fail (error 487), the
  tiled aperture at 0xF0000000 cannot map, and the run faults writing
  0xF0AFE000. `abc3` (five seeds) stopped at the main menu on 0x000782C0.
- [ ] 2.5 Body (section 3 template; Mac "not run, no POSIX glue before
  D"). Audit (D7).

## 3. C: `pr/portable-build`

- [x] 3.1 One commit, "Build with llvm-mingw and Clang", as design D4.
  Done: `pr/portable-build` abd7379; `bodies/C.md`. The POSIX-only pieces
  (`stddef.h`, OpenSSL, SDK libs under WIN32) moved to D.
- [x] (upstream runtime, recorded before the 2026-10-16 rule) 3.2 PC: `Setup.cmd` on the head still gives `bin\burnout3.exe`,
  which reaches Press START. Done: Setup's CMake configure and build
  (MSVC) on abd7379 give `bin\burnout3.exe` (CUI, XINPUT1_4, 16 MB stack);
  `c-boot` reaches the main menu, 0 faults.
- [x] 3.3 Mac: the llvm-mingw cross build against v0.14.0 links;
  `llvm-objdump -p` shows `XINPUT1_4.dll` and no `XINPUT1_3.dll`. PC:
  that exe reaches Press START on Windows 11 with a pad. Deck: it boots.
  Mac part done (2026-10-16) with the README recipe: links;
  XINPUT1_4; 16 MB stack; PDB written; 0 warnings from b3 sources.
  B alone, built the same way, imports XINPUT1_3. PC and Deck parts done
  with the A+B+C exe (XINPUT1_4): a race on both.
- [x] 3.4 The `merge-tree` checks against A, B and upstream #2. Body.
  Audit. Done: A+B, A+C, B+C and each with #2 (93017586) are clean. The
  private-pattern and `BLX-` grep of log, diff and bodies is empty.

## 4. B: `pr/windows-one-window`

- [x] 4.1 One commit, "Build as a Windows GUI program: one window, a
  dialog when boot fails", as design D4. Done: `pr/windows-one-window`
  b7633c5; `bodies/B.md`. The Mac mingw build is PE subsystem 2 (GUI),
  with no warnings in `main.c`.
- [x] (upstream runtime, recorded before the 2026-10-16 rule) 4.2 PC with MSVC on the head: 1 top-level window from `Play Burnout
  3.cmd` and from Explorer (scheduled-task desktop run, windows listed as
  text). Closing the window ends the process. A missing XBE shows the
  dialog and exits non-zero; `--headless` and `RECOMP_HEADLESS` show none.
  `-DBO3_CONSOLE=ON` prints to cmd. `RECOMP_STDIO_LOG` writes the log.
  Done (2026-10-16, `runs/b3-upstream-prs/pc/` and `pc/second/`): direct
  launch 1 window (B b7633c5 and the A+B+C exe); the launcher's console
  only until `start` returns; the console build 2. WM_CLOSE: the first
  watcher (40a8d1b) missed it on the A+B+C exe, because the pushbuffer
  executor reopens the window on its next clear; b7633c5 also treats a
  new handle as closed, and B and A+B+C exit in 0.2-0.3 s, code 0 (at 40 s
  and at 120 s). origin/main keeps running. Dialog: missing XBE exit 1,
  `--bogus` exit 2 with the usage; none with `--headless` or
  `RECOMP_HEADLESS=1`. `RECOMP_STDIO_LOG` and a cmd redirect both keep
  the log. MSVC adds one C4702 (the `return 0` after `ExitProcess`, as
  the two existing exits in main.c already have).
- [x] (upstream runtime, recorded before the 2026-10-16 rule) 4.3 Deck: 1 window (Mac-built exe). Done: `abc4`, at most 1
  viewable window in 249 samples through to the race.
- [ ] 4.4 Body (it states the console change). Audit.

## Host gate exe

The Mac-built A+B+C exe (merge 24af25c, tree 195150c, of A 67771a2, B
b7633c5 and C abd7379; gen from A's six seeds on v0.14.0) is
`runs/b3-upstream-prs/mac/burnout3.exe` (sha256 10c005e6…, GUI,
XINPUT1_4, 16 MB stack) with its PDB. Earlier exes are in
`mac/superseded/` (7f49808e; 6343f51a on the five-seed gen; 55ca09ea). The Deck
gates ran 55ca09ea (six-seed gen, B at 40a8d1b). Proton and
Windows gates for A, B and C run this one exe where a PR's own change
alone is not what is being tested (A's boot, B's window count). C's
XINPUT gate also uses it. Exes built per branch are only needed if a gate
fails.

## Runtime on the fork toolkit (rule of 2026-10-16)

The user, 2026-10-16: Burnout 3 is no longer runtime-tested on upstream;
runtime relies on our fork (design D7). The upstream runs above stay as a
record and are not re-run; on v0.14.0 only the build checks count.

- [x] R.1 Exe: the A+B+C merge (24af25c, tree 195150c) regenerated
  (`regen.sh --disasm`, 24,001 functions, 0 failed, 15 stubs) and
  cross-built with llvm-mingw on the Mac against the fork toolkit
  4e5f603 (`blinx2-v0.4.0`, b3 `main`'s pin): GUI 4381643b… (XINPUT1_4,
  d3d11, 16 MB stack, PDB), and `BO3_CONSOLE=ON` c6737755… (CUI). Logs in
  `runs/b3-upstream-prs/mac/fork/`.
- [x] R.2 PC (Windows 11, desktop task, `runs/b3-upstream-prs/pc/fork/`):
  1 window started directly and from `Play Burnout 3.cmd`, and with
  `RECOMP_PB_BACKEND=d3d11` (class `RecompNv2aD3D11`); the console build 2
  (its console and the game). Closing the window ends the process with
  code 0 in 0.0-0.3 s (40 s, 120 s, d3d11). Dialog: missing XBE exit 1,
  `--bogus` exit 2 with the usage; none with `--headless` or
  `RECOMP_HEADLESS=1`. A cmd redirect keeps the log for both builds
  (exit 1, `[FATAL]` line). fk-race (no save fixture): the four movies
  and the main menu, 0 unresolved, 0 faults, doorbell auto.
- [x] R.2b PC: fk-race-save (the profile save in `saves\`, 300 s,
  session 5): the four movies, then a race (C3_V1 `streamed.dat`), 0
  unresolved, 0 faults, doorbell auto. Logs in
  `runs/b3-upstream-prs/pc/runs/fk-race-save/`. A's and C's bodies say race.
- [x] R.3 Deck (GE-Proton 11-9, `runs/b3-upstream-prs/deck/fk-*`): with the
  save fixture under `saves\` (where the fork keeps UDATA/TDATA),
  `fk-cpu-save`: the four movies, race (C2_V1 `streamed.dat`), 0
  unresolved, 0 faults, `auto` ack, at most 1 window. Without the fixture
  (`fk-cpu`, `fk-d3d11`) the game stays at `englis30.xmv` with no
  profile, 0 faults, 1 window: a fixture gap, not a PR result.
  `RECOMP_WIN_PLACEHOLDER` is not needed on the fork.

## 5. D: `pr/xbr-cli` (optional)

- [x] 5.1 Branch from A + B + C (merged locally in that order). Commit 1,
  "Run on macOS and Linux through xboxrecomp's SDL host" (D4).
- [x] 5.2 Commit 2, "Build, package and release through xboxrecomp-cli
  (optional)" (D4, D5). `game.toml` is pinned to upstream v0.14.0 with
  the fork block commented and `[cli]` on v0.7.6. The folder keys and
  `make_dirs` work without the table. The spin_waits note is reworded.
- [x] 5.3 gen: on the upstream pin, `./burnout3 analyze` then `recomp`
  gives gen byte-identical to `regen.sh`'s (a `py -3` shim on the Mac).
- [x] 5.4 Mac: `./burnout3 build macos` boots to Press START on the CPU
  rasteriser (frames/s recorded). `package macos` makes the DMG.
- [x] 5.5 `./burnout3 build windows` and `package windows`. PC: install
  under `b3prs` in the Windows PC's work folder, 1 window, Press START, the log in the data
  folder. `package steamos`. Deck (scratch root): 1 window, race reached.
- [x] 5.6 Fork pin (block swapped in a scratch copy, gen regenerated):
  Deck race gate on d3d11 and Mac race gate on Metal, flips/s within 25%
  of the 2026-10-15 figures. One window on each. Deck now 51.8.
- [x] 5.7 PC: `Setup.cmd` and MSVC on the head are unchanged (Press
  START). `uv run pytest scripts`, ruff check and format; `./burnout3
  wizard config --dev --json`; `wizard build --dev --only macos --dry`.
  Done 5.1-5.7: `pr/xbr-cli` 1dd467b (commit 1) and 6545333 (commit 2),
  stacked on A, B and C through the local merges b08ee20 and 24af25c.
- [ ] 5.8 Body: the shared feature list (D5), the degradation table, "one
  merge takes A to C". Audit.

## 6. Index, opening, afterwards

- [x] 6.1 Opus review of the four branches and five bodies. Fixes go to
  the branches' agent. Gates re-run on any changed head.
- [x] 6.2 With the user's go: push the four branches to
  apexJCL/burnout3-recomp. Open I as a draft (D6, run dates refreshed),
  then A, C, B, D. Put the PR numbers in I and the bodies. Tell
  xb-upstream I's number and the feature list for its xboxrecomp issue,
  and add that issue's link to I and D when it exists.
  Done 2026-10-16: sp00nznet/burnout3 #4 (I, draft), #5 (A), #6 (C),
  #7 (B), #8 (D). The bodies here match the published ones (the Title and
  Branch lines are stripped there). The xboxrecomp CLI issue is
  sp00nznet/xboxrecomp#223, linked from I. B cites
  sp00nznet/xboxrecomp#215 as the toolkit-side `WM_CLOSE` fix.
- [ ] 6.3 As PRs merge or close: delete their fork branches. Once all
  are settled, merge upstream `main` into fork `main` in a worktree (no
  rewrite) and close I. The fork release follows the usual rounds.
- [ ] 6.4 Copy notes to `notes/b3-upstream-prs/` and the gate evidence
  to `runs/b3-upstream-prs/`. Remove the worktrees through
  `notes/cleanup-derived.py` and keep the branches. Update
  `cat/TASKS.md` (Burnout 3 line, F1-F4).
