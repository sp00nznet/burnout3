Title: Build as a Windows GUI program: one window, a dialog when boot fails
Branch: apexJCL/burnout3-recomp:pr/windows-one-window (1 commit, b7633c5)

Part of #4. Depends on: none. Toolkit: any (no toolkit change).

### What and why
- `burnout3.exe` was a console program. Started with no console attached
  (Explorer, a shortcut, Proton's launcher), it got a console window of its
  own beside the game's.
- Linked as a GUI program, it opens only the game's window. `main()` stays the
  entry point (`/ENTRY:mainCRTStartup`), so the options parse as before. What
  changes is where the output goes and how a run ends.
- A GUI program has nowhere to print and no console to close, so:
  - **closing the window ends the run.** On v0.14.0 the GDI presenter's
    `WM_CLOSE` only stops its own thread, and the game keeps running. With a
    GUI build that would be invisible: audio playing, the CPU busy, `saves\`
    held, and the next launch a second instance. A thread watches this
    process's `XboxRecompFramebuffer` window and, once it has gone or been
    replaced (the pushbuffer executor reopens it on its next surface clear),
    encodes any `--record` and exits. sp00nznet/xboxrecomp#215 (`WM_CLOSE`
    ending the process in the toolkit) would make it redundant;
  - a failed startup step (loading the XBE, mapping the Xbox address range,
    an unknown option) shows a dialog that names it, unless `--headless` or
    `RECOMP_HEADLESS`, and still goes to stderr as `[FATAL]`;
  - `RECOMP_STDIO_LOG=<file>` keeps the log; a redirect from cmd
    (`burnout3.exe > bo3.log 2>&1`) works too;
  - `-DBO3_CONSOLE=ON` builds the console program for terminal debugging.
- `Play Burnout 3.cmd` now starts the exe with `start`. Otherwise the batch
  file's console stays open and empty until the game exits.

### Tests and tested with (head b7633c5)
Built on xboxrecomp v0.14.0 (MSVC and llvm-mingw); runtime tested on the
xboxrecomp fork (apexJCL/xboxrecomp blinx2-v0.4.0).
- Build on v0.14.0: MSVC (VS 2022, Windows 11) and llvm-mingw on macOS link
  the GUI exe (PE subsystem 2) and the `BO3_CONSOLE=ON` exe (3).
- Runtime on the fork, A, B and C merged and cross-built with llvm-mingw
  against it. Windows 11 desktop, windows listed every 2 s for 60 s:
  - started directly (as Explorer does), from `Play Burnout 3.cmd`, and
    with `RECOMP_PB_BACKEND=d3d11`: 1 window, the game's. The console
    build: 2, its console and the game's;
  - closing the window (`WM_CLOSE`) ends the process with exit code 0
    within 0.3 s, 40 s and 120 s in;
  - the dialog with no `Burnout 3 Takedown\` (exit 1) and with `--bogus`
    (exit 2); none with `--headless` or `RECOMP_HEADLESS=1`;
  - a cmd redirect and `RECOMP_STDIO_LOG` keep the log for both builds.
- Steam Deck under Proton (GE-Proton 11-9): at most 1 window to a race.

### Risk and how to back it out
- From a terminal, the prompt now comes back at once and the output goes
  nowhere unless `RECOMP_STDIO_LOG` is set, the command redirects it, or the
  build uses `-DBO3_CONSOLE=ON`. The docs give all three.
- Ctrl+C reaches only the console build, so a GUI `--record` run encodes
  after `--seconds` or when its window closes.
- `Play Burnout 3.cmd` no longer passes the game's exit code on.
- `-DBO3_CONSOLE=ON` gives the old program back. Nothing changes off Windows.

### Provenance
AI-assisted (Claude Code), own work, checked on the hosts above; no game data.
