## ADDED Requirements

### Requirement: One window at boot

A windowed run SHALL show exactly one top-level window on every host: the
backend's presenter on Windows and Proton (the D3D11 swap chain window, or
the GDI framebuffer window for every other backend, since on Windows the
CPU rasteriser is what any backend but `d3d11` draws with) and the SDL
window on POSIX. The Windows executable SHALL link as a GUI-subsystem
program, so a launch with no console attached opens no console window; a
console build is an explicit configure option for terminal debugging. The
toolkit SHALL open no GDI framebuffer window while `RECOMP_PB_BACKEND` is
`d3d11` (compared without case), whatever `RECOMP_FB_WINDOW` says, and
SHALL log why once; any other backend value keeps the GDI window.

#### Scenario: Packaged game under Proton on D3D11

- **WHEN** the bundle's launcher starts the game with
  `RECOMP_PB_BACKEND=d3d11` and the player's `launch.env` sets nothing else
- **THEN** one top-level window appears, the D3D11 one (the log has its
  `device up: feature level` line), and the run's log has no `[FBWIN]
  framebuffer window open` line

#### Scenario: RECOMP_FB_WINDOW set with the D3D11 backend

- **WHEN** `RECOMP_FB_WINDOW=1` and `RECOMP_PB_BACKEND=d3d11` (or `D3D11`)
  are both set
- **THEN** the GDI window is not opened, the log says
  `[FBWIN] RECOMP_FB_WINDOW ignored: the d3d11 backend presents in its own window`,
  and the D3D11 window shows the game

#### Scenario: CPU rasteriser on Windows

- **WHEN** the game runs windowed under Proton with `RECOMP_PB_BACKEND`
  unset or `cpu`
- **THEN** the GDI framebuffer window is the one window (one `[FBWIN]
  framebuffer window open` line), as on upstream's toolkit

#### Scenario: A backend Windows has no presenter for

- **WHEN** `RECOMP_FB_WINDOW=1` and `RECOMP_PB_BACKEND` is `metal` or
  `null` on Windows
- **THEN** the GDI window opens as before: nothing else would present, and
  a run with no window is never the result of the guard

#### Scenario: Metal on the Mac

- **WHEN** the game runs windowed on macOS
- **THEN** the SDL window is the one window and Metal blits into its layer;
  nothing about the Windows rule changes the Mac

#### Scenario: Terminal debugging

- **WHEN** the game is configured with the console option on and run from a
  terminal
- **THEN** its output reaches the terminal, and the window count is still
  one
