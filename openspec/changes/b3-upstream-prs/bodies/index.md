Title: Tracking: Burnout 3 changes from apexJCL/burnout3-recomp (index, not for merge)
Draft, from apexJCL/burnout3-recomp:main. Not meant to be merged; it closes when the PRs below settle.

[apexJCL/burnout3-recomp](https://github.com/apexJCL/burnout3-recomp) is a fork of this
repo. It builds through [xboxrecomp-cli](https://github.com/apexJCL/xboxrecomp-cli) on the
toolkit fork ([apexJCL/xboxrecomp](https://github.com/apexJCL/xboxrecomp) `blinx2/portability`),
and it credits this project as its origin (MIT). This draft splits the fork's changes into
PRs that apply to `main` on their own. Each one builds through `Setup.cmd` on xboxrecomp
v0.14.0; runtime is tested on the toolkit fork.

| PR | Topic | Depends on | Toolkit | Commits | Head | Tested with |
|---|---|---|---|---|---|---|
| #5 | Boot on v0.14.0: six seeds, DSP doorbell left to the toolkit | none | v0.14.0 | 1 | 67771a2 | Win 11 (MSVC build, fork runtime), Proton |
| #6 | Build with llvm-mingw and Clang (`xinput1_4`) | none | v0.14.0 | 1 | abd7379 | Win 11 (MSVC and mingw build, fork runtime), Proton |
| #7 | Windows GUI program: one window, a dialog when boot fails | none | any | 1 | b7633c5 | Win 11 (MSVC build, fork runtime), Proton |
| #8 | Optional: macOS/Linux host plus xboxrecomp-cli (build, package, wizard, release) | #5, #7, #6 | v0.14.0 pin; fork pin optional | 2 | 6545333 | Win 11, Proton, macOS |

**Review order:** #5 first, because without it `main` stays on a black screen on today's
toolkit. Then #6 and #7, then #8 if you want it. #8 carries A to C, so merging it
alone takes all four. #8 stacks on the exact #5, #7 and #6 heads through merge
commits, so once they merge it shows only its own two commits.

### Where Burnout 3 is (runs of 2026-10-15 and 16)
- **On upstream xboxrecomp v0.14.0:**
  - without #5, no movie on Windows 11: `main`'s fixed DSP doorbell address went
    stale with sp00nznet/xboxrecomp#189. With the doorbell right, a black screen at
    `englis30.xmv`, and later a main menu that stops once the profile loads (until
    sp00nznet/xboxrecomp#186 lands);
  - with #5, #7 and #6 merged (earlier runs, before testing moved to the fork),
    the race on native Windows 11 and under Proton (the GDI presenter, about 10 fps on
    the Deck; under Proton it needs `RECOMP_WIN_PLACEHOLDER=1`).
  - known toolkit issue: the game writes `LocalCache*.bin` into its game folder;
    sp00nznet/xboxrecomp#206 moves them to `<save>/TitleCache`.
- **On the toolkit fork (blinx2-v0.4.0):**
  - races at 51.8 flips/s on a Steam Deck under Proton (D3D11) and 57.1 on macOS (Metal);
  - one window on Windows 11 and Proton;
  - release [v2026.10.0-rc.1](https://github.com/apexJCL/burnout3-recomp/releases), with a
    build wizard for Windows, SteamOS and macOS.
  - What the fork toolkit adds over upstream (GPU backends, the macOS host, pacing) is
    tracked in sp00nznet/xboxrecomp#184.
- Screenshots of the fork build: the fork's [README](https://github.com/apexJCL/burnout3-recomp#readme)
  and [docs/images](https://github.com/apexJCL/burnout3-recomp/tree/main/docs/images). They are
  linked here, not offered.

### The CLI
#8 adds a `./burnout3` (and `burnout3.cmd`) front end. It pins xboxrecomp by tag and
commit, cross-builds Windows from macOS and Linux, packages an installer, a SteamOS bundle
and a DMG, and publishes a player wizard. Upstream's own route keeps working as it is.
Proposing the CLI to xboxrecomp itself is sp00nznet/xboxrecomp#223.

### Provenance and testing
AI-assisted (Claude Code), own work, no game data. Each PR is built on xboxrecomp v0.14.0
(MSVC and llvm-mingw) at its exact head; runtime is tested on the xboxrecomp fork
(apexJCL/xboxrecomp blinx2-v0.4.0), on the hosts each PR names.
