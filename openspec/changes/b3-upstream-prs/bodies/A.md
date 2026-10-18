Title: Boot on xboxrecomp v0.14.0: six seeds, auto DSP ack
Branch: apexJCL/burnout3-recomp:pr/boot-v0.14 (1 commit, 67771a2)

Part of #4. Depends on: none. Toolkit: xboxrecomp v0.14.0 as is (needed:
v0.13.1 has no automatic doorbell ack).

### What and why
- `Setup.cmd` builds against xboxrecomp `main`, which is v0.14.0. There the
  game stays on a black screen at the language movie (`englis30.xmv`). It
  calls `0x00014C80` through a pointer that the dispatcher can't resolve,
  and logs `[ICALL] Failed to resolve` thousands of times a minute.
- That function is packed straight after a switch table, which v0.14.0's
  disassembler doesn't walk past. The same runs log `0x000229E0`,
  `0x00022B30`, `0x00022D30` and `0x00067D10` as unresolved, once each.
- Past the boot, the main menu stops advancing once the profile has loaded:
  every call to `0x000782C0` fails to resolve.
- sp00nznet/xboxrecomp#186 finds all six statically. Until it merges, this seeds
  them so today's toolkit boots and reaches a race. After it merges, the
  seeds are redundant.
- `main.c` forced `RECOMP_APU_DSP_ACK=0x825F8810`. The doorbell is a contiguous
  allocation, so the address moved when the allocator changed
  (sp00nznet/xboxrecomp#189), and since sp00nznet/xboxrecomp#193 the toolkit
  derives it when the variable is unset (`auto`). Setting it still overrides.
- That makes v0.14.0 a requirement. `Setup.cmd` never updates an existing
  `..\xboxrecomp`, so the README and the changelog say to run
  `git -C ..\xboxrecomp pull`, then regenerate and rebuild.

### Changes
- `config/seed_functions.json`: the six entries, each with a note.
- `src/game/main.c`: no `RECOMP_APU_DSP_ACK` default; the comment says why.
- `docs/boot.md` (rows 10, 11, a note under row 5), `docs/running.md`,
  `README.md` (v0.14.0 or later, how to update), `CHANGELOG.md` (Fixed).

### Tests and tested with (head 67771a2)
Built on xboxrecomp v0.14.0 (MSVC and llvm-mingw); runtime tested on the
xboxrecomp fork (apexJCL/xboxrecomp blinx2-v0.4.0).
- `regen.sh --disasm` on v0.14.0: 23,655 functions, 0 failed, 3 stubs (23,648
  without the seeds): the six plus `0x000780D0`, which `0x000782C0` calls.
- Build on v0.14.0: `Setup.cmd` (MSVC, VS 2022, Windows 11) and the llvm-mingw
  cross build on macOS both link.
- Runtime on the fork, the A, B and C branches merged and cross-built
  with llvm-mingw against it:
  - Windows 11, with a profile save in `saves\`: the four movies, then a
    race, 0 `[ICALL] Failed to resolve`, 0 faults, the doorbell on `auto`.
  - Steam Deck under Proton (GE-Proton 11-9), with a profile save in
    `saves\`: the four movies, then a race, with the same three results.
- macOS: not run. `main.c` is Windows-only before #8.

### Known toolkit issue (not changed here)
On v0.14.0 the game writes `LocalCache*.bin` into its game folder;
sp00nznet/xboxrecomp#206 moves them to the save folder's `TitleCache\`.

### Risk and how to back it out
Seeds only add function starts. If one turned out wrong, removing its entry
and running `regen.sh` puts gen back. A run that needs the old doorbell can set
`RECOMP_APU_DSP_ACK=0x825F8810`.

### Provenance
AI-assisted (Claude Code), own work, checked on the hosts above; no game data.
