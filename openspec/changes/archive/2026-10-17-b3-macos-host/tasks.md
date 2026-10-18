# b3-macos-host: tasks

Worktrees: `wt/b3mac/b3` on `b3/macos-host` (off b3 `main` d2c4335);
`wt/b3mac/cat` on `b3/macos-host-spec` (this spec); `wt/b3mac/xboxrecomp-cli`
detached at the pin f476bfa (the bootstrap finds it beside the game tree).
Toolkit: the main checkout at 0988b9c, read-only, via `XBOXRECOMP_DIR`;
no toolkit worktree unless a toolkit change is needed.

Rules: Mac builds and runs through `sh notes/mac-run-lock.sh`,
`DEVELOPER_DIR=/Library/Developer/CommandLineTools`, `SDL_AUDIODRIVER=dummy`,
no screen capture, kill by PID only. Dumps under `runs/b3mac/`, notes in
`notes/b3mac/NOTES.md`. Proton is not this change's: shared changes are
listed for the orchestrator.

## 1. Spec and pipeline
- [x] 1.1 Worktrees, the dump link, `./burnout3 setup --no-toolkit` against 0988b9c.
- [x] 1.2 `recomp_manual.c` gets `<stddef.h>`; `./burnout3 analyze && ./burnout3 recomp` regenerate gen/ (the gen key hashes that file). Check the dump is untouched and the gen matches b3deck's apart from nothing (same toolkit, same seeds).
- [x] 1.3 This spec, committed on `b3/macos-host-spec`.

## 2. Game-side glue (b3 `b3/macos-host`)
- [x] 2.1 `src/game/main.c`: host sections per design D1-D6. Windows code paths unchanged apart from `fault_printf`, `env_set` and the `--seconds` timer.
- [x] 2.2 `src/game/CMakeLists.txt` and `CMakeLists.txt` per D7.
- [x] 2.3 `game.toml`: `build.targets = ["windows", "macos"]`.
- [x] 2.4 `./burnout3 build macos` builds `build/burnout3` on the Mac. The Windows cross build still configures and compiles `main.c` (`./burnout3 build windows`, link included, since llvm-mingw is at hand).
- [x] 2.5 `docs/running.md`: the macOS section.

## 3. Runs (Mac, headless Metal)
- [x] 3.1 Boot (120 s: `RECOMP_WATCHDOG_SECS` is an absolute timer), no input: past 10 s, intros, `_FEMain.xmv` at 26 s, "Press START" frame, no `[FAULT]`, no quick-reboot. `TITLE_KEVENTS`: see D6.
- [x] 3.2 Race, the TASKS r3 script, `--seconds 900`, fresh hard disk, `RECOMP_FB_DUMP` every 150th flip, `RECOMP_TRACE=flip`: to the end with no fault; race markers in the log; frames of the race.
- [x] 3.3 Pacing and visuals: flips/s for the front end and the race; look at the dumped frames (title screen, menus, race) and note anything wrong against the Proton frames in `runs/missing-files/b3/` or the s6-M-r3 notes.
- [x] 3.4 cfb3be3 A/B per D8, or the owed note.

## 3b. Added by the user mid-task
- [x] 3.5 A macOS DMG the way cat makes one: `./burnout3 package macos` through the CLI's existing macos flow (no CLI change); `packaging/macos/` launcher, Info.plist and README part, `launch.env.default.macos`, the package keys. Under `wt/b3mac/b3/dist/`.

## 4. Gates and hand-back
- [x] 4.1 Mac build gate (2.4). No Python changed in b3 unless `burnout3.py` was touched (it is not); no toolkit change, so no ctest.
- [x] 4.2 Notes to `notes/b3mac/NOTES.md`; dumps under `runs/b3mac/`; `b3/TESTING.md` is an untracked file in the main checkout, which agents never edit: its run note is in `notes/b3mac/NOTES.md` for the orchestrator to copy.
- [ ] 4.3 Owed to the orchestrator: a Proton run of `b3/macos-host` (the `--seconds` timer, `fault_printf`, `env_set` are shared; `TITLE_KEVENTS` question), the Fable merge review, and the Windows `TITLE_KEVENTS` decision.
