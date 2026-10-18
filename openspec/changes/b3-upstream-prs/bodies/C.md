Title: Build with llvm-mingw and Clang
Branch: apexJCL/burnout3-recomp:pr/portable-build (1 commit, abd7379)

Part of #4. Depends on: none. Toolkit: v0.14.0 as is. The xinput line
works today; sp00nznet/xboxrecomp#221 links `xinput1_4` under MinGW in the
toolkit itself, and once that is merged the line does nothing and can go.

### What and why
- The build only worked with MSVC.
  llvm-mingw (Clang, MinGW ABI) builds the same game on Windows, or
  cross-compiled from macOS or Linux. Four things were missing:
  - the 16 MB stack MSVC gets from `/STACK`, and (with Clang's lld) a PDB
    beside the exe, so fault reports still name the generated function;
  - **XInput:** MinGW's `libxinput.a` imports `XINPUT1_3.dll`, which
    Windows 10 and 11 don't ship, so the exe would not start on a stock
    install. The build links `xinput1_4` instead, as MSVC's `xinput.lib`
    does, and configure warns if an `xinput` entry survives;
  - `-Wparentheses-equality`: off for the generated sources, where Clang
    warns on every `if ((a == b))` (tens of thousands of lines);
  - `bin\` stays the layout of every MSVC build, whatever the generator (VS
    Open Folder and VS Code use Ninja). A MinGW build puts the exe in its own
    build dir, so a cross build and a native one never overwrite each other.

### Changes
- `src/game/CMakeLists.txt`: the four items above.
- `README.md` (the Ninja recipe, with `-B build/mingw`, gen/ first, and the
  `py` shim `regen.sh` needs off Windows), `CHANGELOG.md` (Added).

### Tests
- On macOS, the README recipe cross-compiles against v0.14.0 with
  llvm-mingw 20260922. `llvm-objdump -p burnout3.exe` lists `XINPUT1_4.dll`
  (without this change, built the same way, it imports `XINPUT1_3.dll`), a
  16 MB stack reserve,
  and `burnout3.pdb` beside it. All 74 warnings are the toolkit's; none come
  from this repo's sources.

### Tested with (head abd7379)
Built on xboxrecomp v0.14.0 (MSVC and llvm-mingw); runtime tested on the
xboxrecomp fork (apexJCL/xboxrecomp blinx2-v0.4.0).
- Build on v0.14.0: `Setup.cmd`'s CMake configure and build (MSVC, VS
  2022, Windows 11) still gives `bin\burnout3.exe`. The llvm-mingw cross
  build on macOS links, with a PDB, imports `XINPUT1_4.dll` and no
  `XINPUT1_3.dll`, and has no warnings from b3's sources.
- Runtime on the fork, the A, B and C branches merged and cross-built
  with llvm-mingw against it:
  - Windows 11, with a profile save in `saves\`: the four movies, then a
    race, 0 `[ICALL] Failed to resolve`, 0 faults, the doorbell on `auto`.
  - Steam Deck under Proton (GE-Proton 11-9), with a profile save in
    `saves\`: boots and reaches a race.

### Risk and how to back it out
- MSVC and `Setup.cmd` are unchanged.
- A MinGW build now writes the exe to its build dir instead of `bin\`; MSVC
  builds keep `bin\`.

### Provenance
AI-assisted (Claude Code), own work, checked on the hosts above; no game data.
