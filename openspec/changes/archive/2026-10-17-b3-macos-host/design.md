# b3-macos-host: design

## Context

`b3/src/game/main.c` (d2c4335) does six things: load the XBE, map the
memory layout, bring up the APU, OHCI and kernel, install diagnostics
(VEH, dbghelp symbols, the hang watchdog, the NaN trap), set the
environment defaults the title needs, and jump to the entry point. Only
the diagnostics and the window are host-specific; the boot sequence is
not. cat's `main.c` is the same shape with both hosts in one file, and its
POSIX half (signal handler, `dladdr`, frame-pointer walk,
`xbox_HostWindowMain`) is what the toolkit's SDL host expects from a game.

What the toolkit at 0988b9c already does on POSIX arm64, so the game does
not have to:
- `xbox_PosixMmioFault(ucv, si_addr)` decodes the faulting A64 instruction
  and dispatches APU, AC'97 bus-master and OHCI accesses
  (`xbox_memory_layout.h` 248-253; `ohci.c` `OHCI_TRAP_POSIX_A64`).
- `RECOMP_PAD_SCRIPT` and `RECOMP_PAD_LIVE` live in `usb_gamepad.c` with
  no `_WIN32` guard.
- `\Device\Harddisk0\Partition0` and the partition devices are backed by
  image files in the save dir on both hosts (`kernel_path.c` 186-192), so
  XAPI's utility-drive mount works and the TESTING.md T4 workaround
  (clearing `XINIT_MOUNT_UTILITY_DRIVE`) is not needed.
- `xbox_HostWindowMain(guest_main)` runs the guest on a pthread under an
  SDL window, a CAMetalLayer when `RECOMP_PB_BACKEND=metal`, and returns
  `guest_main()` directly when `RECOMP_HEADLESS` is on
  (`fb_present_sdl.c` 505-620).
- `xbox_WatchdogStart` (`RECOMP_WATCHDOG_SECS`) is host-independent.

## Decisions

### D1. One `main.c`, host sections, no `main_posix.c`

TESTING.md's baseline B added a second file. A second file duplicates the
boot sequence, and the two then drift (b3's old in-tree runtime copy
drifted from the template by hundreds of lines, which is why
`src/game/CMakeLists.txt` points at the template headers). cat keeps one
file with `#if defined(_WIN32)` around the host parts; b3 does the same.
The Win32 paths keep their code; shared helpers are the only Windows
change:
- `fault_printf`: `vfprintf(stderr)` on Windows (as before), a
  `vsnprintf` into a stack buffer plus `write(2)` on POSIX, where the
  report runs inside a signal handler and the faulting thread may hold
  stdio's lock (Burnout 3 logs heavily from the kernel, so this is not
  theoretical).
- `env_set(name, value)`: `_putenv_s` on Windows, `setenv`/`unsetenv` on
  POSIX. Same call sites, same defaults, same `RECOMP_PB_EXEC=0` ->
  unset rule.

### D2. The crash handler

`sigaction` for SIGBUS and SIGSEGV with `SA_SIGINFO | SA_ONSTACK |
SA_NODEFER` and a `sigaltstack` on the thread that installs it (the guest
thread: `host_main` installs it, so it follows the guest onto the pthread
the window makes). Order inside the handler:
1. `xbox_PosixMmioFault(ucv, si_addr)`: a serviced access returns with pc
   advanced, nothing printed.
2. A re-entry guard, then the report: the `[FAULT]` line (host pc, the
   signal, the guest VA when the address is inside guest memory), the
   guest registers, `print_guest_stack`, `BO3_PEEK`, then the native
   stack from the frame records (Apple arm64 keeps frame pointers).
3. Restore `SIG_DFL` and return, so the instruction re-faults and the
   process dies by the original signal (exit status and core as usual).

The tag stays `[FAULT]`: `game.toml` `bench.crash_tag` is one string for
both hosts, and the Proton bench greps for it.

### D3. Window, headless, backend

- Not headless: `xbox_HostWindowMain(host_main)`, as cat. The process
  main thread runs AppKit's loop; the whole boot and the guest run on
  the toolkit's `guest-main` pthread, and the crash handler and watchdog
  are installed inside `host_main`, so they land on that thread.
- `--headless` sets `RECOMP_HEADLESS=1` (the toolkit then returns
  `host_main()` on the calling thread). On Windows `--headless` still
  means "do not set `RECOMP_FB_WINDOW`"; nothing changes there.
- Backend: on Apple, `RECOMP_PB_BACKEND` defaults to `metal` when unset.
  The CPU rasteriser runs the front end at a few fps and a race at one or
  two, which is not "plays"; `RECOMP_PB_BACKEND=cpu` restores it. This is
  a host default, not an enhancement: the frame is the same stock frame.
  Windows keeps the toolkit default (cpu) because the Proton bench and
  the packaged launcher choose the backend themselves.

### D4. `--seconds` ends the run on every host

Today `seconds` is only read by `record_start`, so `--seconds 900` with
no `--record` runs forever (docs/running.md even says to use
`BENCH_TIMEOUT`, not `--seconds`, on the bench). The headless Mac run has
no window to close and no bench to time it out, so `--seconds` gets a
timer thread that finishes the recording (if any) and exits with 0. The
thread is one function with a Win32 and a pthread body. This is a shared
behaviour change, so it is listed for a Proton run; it cannot change any
run that passes no `--seconds`.

### D5. What stays Win32-only

`RECOMP_HANG_WATCHDOG` (toolhelp thread sampling with dbghelp stack walks)
and `BO3_TRAP_NAN` (MXCSR edits through `SetThreadContext`) have no POSIX
equivalent worth writing for this change: the toolkit's
`RECOMP_WATCHDOG_SECS` dumps the guest stack at a hang, and the NaN hunt
that needed the trap is done. On POSIX each prints one line saying it is
Windows-only when set, so a copied command line is not silently inert.

### D6. Environment defaults on POSIX

Same defaults as Windows: `RECOMP_AC97_READY=1`, `RECOMP_USB=1`,
`RECOMP_USB_PADS=2` (also the env header's default), `RECOMP_PB_EXEC=1`,
and `recomp_env_reload()` after them. Two POSIX-only additions, both
toolkit keys the title needs on this host and both overridable by an
explicit value:
- `RECOMP_TITLE_KEVENTS=1`: D3D8 (XDK 5849, the same D3D8LTCG BLiNX 2
  has) builds the KEVENT its vblank DPC sets without `KeInitializeEvent`.
  Without a host event the guest address goes to the host as a HANDLE; on
  Win32 `SetEvent` fails silently, on POSIX the bridge dereferences it.
  cat sets it on every host for this reason (`cat/src/main.c`, Step 4).
  Set only on POSIX here so the Proton build is byte-for-byte the old
  boot; whether Windows should have it too is a question for the Proton
  run, recorded in tasks.
- `RECOMP_PB_BACKEND=metal` on Apple (D3).

Verified (runs/b3mac/20261009-233856-boot-kevents0): with
`RECOMP_TITLE_KEVENTS=0` the first vblank DPC faults in `SetEvent` on the
guest address 0x0035F45C (caller 0x00356C51, D3D's vblank handler) before
the first flip; with the default the boot runs. The default stays.

### D7. CMake

- `src/game/CMakeLists.txt`: `kernel32 user32 gdi32 dbghelp` under
  `if(WIN32)`; elsewhere the toolkit targets bring SDL2, the Metal
  frameworks and OpenSSL. `-Wno-parentheses-equality` on the generated
  sources under Clang (32k warnings otherwise, as cat found). The MSVC
  and MinGW blocks are untouched.
- `CMakeLists.txt`: `find_package(OpenSSL REQUIRED)` before
  `add_subdirectory(${XBOXRECOMP_DIR})` when not Windows: the kernel links
  `OpenSSL::Crypto` PUBLIC but finds it inside its own directory, where
  the imported target is invisible to this executable (cat's comment).
- No `WIN32` on `add_executable`: b3 is a console program on every host.

### D8. Runs and evidence

- Build: `./burnout3 build macos` with `XBOXRECOMP_DIR` at the toolkit
  checkout (0988b9c, read-only), `DEVELOPER_DIR` set, through
  `notes/mac-run-lock.sh`.
- Boot and race runs: headless, `RECOMP_PB_BACKEND=metal` (the default),
  `SDL_AUDIODRIVER=dummy`, `RECOMP_HDD_DIR` and `RECOMP_FB_DUMP` under
  `runs/b3mac/<stamp>/`, `RECOMP_TRACE=flip` for the flip count, the TASKS
  pad script for the race, `--seconds 900`. A fresh hard disk per run:
  the r3 route goes through the profile flow itself, and no saves pin
  exists on the Mac.
- Pacing from the `[FLIP]`/`[GPU]` lines: flips per second over the front
  end and over the race, with the Proton numbers from the s6-M-r3 notes
  (menus about 14 fps, races 2-4 fps on the CPU backend) for scale.
- cfb3be3 A/B: the base is the same b3 tree built against the toolkit
  with cfb3be3 reverted in a scratch worktree, when that revert applies
  cleanly on 0988b9c; `runs/missing-files/b3cmp.py BASE NEW` compares
  the `[FILE] ... FAILED` pairs. If the revert does not apply, the A/B is
  owed and the new run's `[FILE]` lines are in the report.

## Risks

- The 10 s stall from TESTING.md was diagnosed as the missing B3 stack;
  if it persists on 0988b9c it is a POSIX toolkit gap and the change
  stops there with the stack dump (`RECOMP_WATCHDOG_SECS`) in notes.
- The Mac is short on memory; 100-300 ms host stalls every 5 s are
  expected and are not counted as pacing regressions.
- The A64 MMIO decoder covers the instructions Clang emits for the
  toolkit's own accessors; a guest access the lifter emits through
  another form would surface as an unserviced SIGBUS at an APU or OHCI
  address. That would be a toolkit change (its own worktree).
