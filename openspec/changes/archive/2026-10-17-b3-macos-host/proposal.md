# b3-macos-host: Burnout 3 boots and plays on macOS (Metal)

Status: written by Fable (2026-10-09, the dispatchable twin of the
macOS-host item), so no separate spec review. Facts were checked against the code at
these commits:
- b3 `main` d2c4335 (`b3/fork-glue` through xbr; `src/game/main.c` is
  Win32-only: `windows.h`, VEH, dbghelp, toolhelp);
- toolkit `posix-host/portability` 0988b9c, which has everything the game
  needs on a POSIX arm64 host already: `xbox_PosixMmioFault` (APU, AC'97
  and OHCI traps through the A64 decoder), `OHCI_TRAP_POSIX_A64`,
  `RECOMP_PAD_SCRIPT` in `usb_gamepad.c`, `xbox_HostWindowMain` (SDL,
  CAMetalLayer for `RECOMP_PB_BACKEND=metal`), and image-backed
  `\Device\Harddisk0\Partition0` on both hosts;
- cat `main` f30f12f, whose `src/main.c` is the reference for how a game
  drives that host;
- xboxrecomp-cli f476bfa (b3's pin): `build macos` needs only
  `build.targets` to list it.

The spec lives in cat because b3 has no `openspec/` (same as b3-on-xbr).

## Why

Burnout 3 is the reference title for upstream toolkit PRs and the second
game the CLI serves, but it runs only under Proton. The Mac is where the
toolkit's POSIX host is developed, and one gate is open on exactly this
run: TASKS "Open gate (tasks 8.6)", the Burnout 3 route on the Mac, the
only run that exercises the toolkit's POSIX PATH_NOT_FOUND change
cfb3be3 on Burnout 3.

`b3/TESTING.md` (2026-10-03) recorded a POSIX attempt on toolkit a374605:
it needed a local `main_posix.c` plus a CMake switch, a pseudo-handle fix
(T3) and the utility-drive workaround (T4), and stalled at about 10 s with
no draws because that toolkit lacked the Burnout 3 stack. None of that
work was committed. The toolkit has since gained the stack (70c893b and
after), the POSIX OHCI trap, the POSIX APU trap and the partition images,
so what is left is the game-side glue: a `main.c` that builds and runs on
a POSIX host, the CMake switches, the manifest's `macos` target and the
docs.

## What changes

Game side only (b3, branch `b3/macos-host`). No toolkit change is
expected; if one proves necessary it gets its own worktree off
`posix-host/portability` and is named in design.md.

- `src/game/main.c`: one file, one boot sequence, with the host-specific
  parts under `#if defined(_WIN32)` / `#else`, the way `cat/src/main.c`
  does it. POSIX gets: a SIGBUS/SIGSEGV crash handler on an alternate
  stack that hands trapped MMIO to `xbox_PosixMmioFault` first and prints
  the same `[FAULT]` report (the bench's `crash_tag`), `dladdr` symbols,
  a frame-pointer native stack, the SDL window through
  `xbox_HostWindowMain` with the guest on its own thread, `--headless`
  as `RECOMP_HEADLESS=1`, `--record` with POSIX paths and `ffmpeg`, and
  the environment defaults through `setenv`. `--seconds N` ends the run
  on every host, recording or not (today it is read only by `--record`).
  The Win32-only diagnostics (`RECOMP_HANG_WATCHDOG`, `BO3_TRAP_NAN`)
  stay Win32 and say so once when set elsewhere.
- `src/game/CMakeLists.txt`, `CMakeLists.txt`: the Win32 import libraries
  only on Windows; the OpenSSL hoist cat needs off Windows; Clang's
  `-Wno-parentheses-equality` on the generated sources.
- `src/game/recomp/recomp_manual.c`: `<stddef.h>` for `ptrdiff_t`
  (TESTING.md T1; MSVC gets it through stdio.h).
- `game.toml`: `build.targets = ["windows", "macos"]`, so
  `./burnout3 build macos` works. Packaging targets are unchanged
  (no macOS bundle).
- `docs/running.md`: a macOS section (build, run, the Metal default,
  `SDL_AUDIODRIVER=dummy`, what is Windows-only).

## Non-goals

- A macOS bundle, launcher or `package macos` target. The developer build
  (`build/burnout3`) is the deliverable.
- Sound on the Mac. Runs use `SDL_AUDIODRIVER=dummy`; title and menu music
  is silent anyway (deprioritised).
- Keyboard or host-pad mapping for Burnout 3 (`RECOMP_PAD_SCRIPT` and
  `RECOMP_PAD_LIVE` drive the pads; the input spike is parked).
- The on-demand thread dump cat has (`RECOMP_THREAD_DUMP`); the toolkit's
  `RECOMP_WATCHDOG_SECS` covers hangs here.
- Fixing anything the toolkit draws wrong on Metal. Visual issues seen in
  the frame dumps are recorded as follow-ups, not fixed here.

## Success criteria

1. `./burnout3 build macos` builds `build/burnout3` on the Mac with
   `DEVELOPER_DIR=/Library/Developer/CommandLineTools` and the toolkit at
   0988b9c, with no toolkit edit; the Windows build is unchanged apart
   from the shared helpers.
2. Headless Metal, the game boots past the old 10 s stall, plays the EA
   and Criterion intros and reaches "Press START" (`ovid\_FEMain.xmv`
   opened, frames with the title screen).
3. The r3 race route (TASKS's `RECOMP_PAD_SCRIPT`, 900 s, an empty hard
   disk) runs to the end with no `[FAULT]`, no `[CRASH]` and no
   `HalReturnToFirmware`, reaches the race (`tracks\US\...`, `crash1.rws`,
   `E_djrace.xwb` opened) and dumps frames of it.
4. Pacing (flips/s from `RECOMP_TRACE=flip` or the `[GPU]` report) and any
   visual issue visible in the dumps are reported; dumps live under
   `runs/b3mac/`.
5. The cfb3be3 A/B (`runs/missing-files/b3cmp.py BASE NEW`) is run when a
   base build without cfb3be3 can be made on the same tree; otherwise the
   new run's `[FILE]` lines are reported and the A/B is listed as owed.
