# Playing Burnout 3: Takedown

The player's reference: what you need, building by hand, installing each
bundle, the options and the in-game menu, and what to do when something
goes wrong. The [README](../README.md) has the short route; the
developers' side is in [developers.md](developers.md).

## What you need

| Item | Why |
|---|---|
| Your own, legally obtained copy of Burnout 3: Takedown: a disc image (`.iso`) or the extracted folder with `default.xbe` | The game's code and assets come from it. This project does not provide or link to them, and refuses other titles. |
| A build machine: a Mac (Apple silicon), a Linux PC or a Windows PC, with about 15 GB free | The bundle is built from your dump on your machine. A Mac can build every target; Linux builds the Windows, SteamOS and native Linux bundles, Windows the Windows and SteamOS ones. The native Linux bundle needs Docker for its first build (the Steam Runtime sysroot). |
| The build wizard, or [uv](https://docs.astral.sh/uv/getting-started/installation/), git and Python 3.9 or newer | The wizard fetches what it needs itself. By hand, `./burnout3` fetches everything else (CMake, Ninja, the compiler, the toolkit), each download checked against a pinned sha256. |
| A controller (two for split-screen), or the keyboard on a Mac | Pad 1 and pad 2 are players 1 and 2. |

## Platforms

| You play on | Bundle | Graphics | Audio | Status |
|---|---|---|---|---|
| Steam Deck, Linux (Proton) | `steamos` (the Windows build, run under Proton through `umu-run`) | Direct3D 11 (DXVK) | XAudio2 (FAudio) | Plays: a scripted race runs at about 57 frames a second on the Deck, the regular test machine. |
| Steam Deck, or an x86-64 Linux with glibc 2.31+ and Vulkan (testing) | `linux` (the native build, no Proton) | Direct3D 11 over Vulkan (DXVK-native) | SDL3 | Testing; boots to the title and the attract movie; not in the wizard yet. Shader presets work as on Windows, except that in Steam's Game Mode they draw at the game's 640x480 and gamescope scales the result, so CRT presets show moiré there; `RECOMP_PRESENT_SURFACE=window` in `config/launch.env` draws them at the window's size instead. The keyboard is off by default, as on Windows; `RECOMP_KEYBOARD=1` in `config/launch.env` turns it on. |
| Mac (Apple silicon) | `macos` (`.dmg`) | Metal | SDL3 (CoreAudio) | Plays: the same race runs at about 55 frames a second. The title and menu music is silent. |
| Windows 10 or 11 | `windows` (a setup program) | Direct3D 11 | XAudio2 | Builds and installs; checked to start and reach Press START on Windows 11. Played and tested under Proton. |

The game draws at 640x480 and 4:3, as on the Xbox; a wide window or screen
is pillarboxed, never stretched. The render scale draws it sharper, and a
wide aspect ratio shows more at the sides (see [Options](#options)).

## Build the game

**The short route: the build wizard.** From the first release on, each
[release](https://github.com/apexJCL/burnout3-recomp/releases) has a small
program for Windows, macOS and Linux (Steam Deck included): download yours,
point it at your disc image, pick the options, and it builds and installs
the game for you. It runs the same commands as below, and offers a small
update build for a device that already has one. Windows SmartScreen and
macOS Gatekeeper warn about it (it is not signed): on Windows, More info >
Run anyway; on macOS, right-click > Open once.

**By hand.** Get this project, then go into its folder (in Terminal on macOS
and Linux, or in PowerShell on Windows):

```sh
git clone https://github.com/apexJCL/burnout3-recomp.git
cd burnout3-recomp
./burnout3 /path/to/your/disc.iso
```

In PowerShell that is `.\burnout3 C:\path\to\disc.iso`. On Windows, keep the
path short (`C:\g\burnout3`) and run `git config --global core.longpaths true`
once before the clone.

It checks that the disc is Burnout 3: Takedown, puts its files in
`Burnout 3 Takedown/` in this folder (git ignores it), fetches the toolchain, lifts
the game and builds a bundle for this computer: macos on a Mac, steamos on Linux, windows on Windows.
The first run takes a while; later runs only rebuild what changed. The
bundle lands in `dist/`.

```sh
./burnout3 package windows|steamos|macos|linux   # the same, for another target
./burnout3 doctor      # what this machine has, what it can package, and what to install first
```

Copy the bundle to the machine you play on and install it there:

| Target | Install | Saves, settings and logs |
|---|---|---|
| **Steam Deck / Linux** (`dist/Burnout3Takedown-<v>-steamos.tar`) | In Desktop Mode: `tar -xf Burnout3Takedown-<v>-steamos.tar`, `cd Burnout3Takedown-<v>-steamos`, `./install.sh --steam` (Steam open: it adds "Burnout3Takedown" as a non-Steam game). A stock Deck has no `umu-run`; the installer offers to download it, or run `./install.sh --fetch-umu`. The first launch downloads the Steam Linux Runtime and GE-Proton, which takes a few minutes. Later versions: `./install.sh`; `./install.sh rollback` goes back one. | `~/Games/Burnout3Takedown/{hdd,config,logs}` |
| **macOS** (`dist/Burnout3Takedown-<v>.dmg`) | Open the `.dmg` and drag `Burnout3Takedown.app` to Applications (replace the old one to update). The app is signed ad hoc, not notarized: on another Mac, right-click > Open once, or `xattr -dr com.apple.quarantine /Applications/Burnout3Takedown.app`. | `~/Library/Application Support/Burnout3Takedown/{hdd,config,logs}` |
| **Windows** (`dist/Burnout3Takedown-<v>-windows/`) | Keep the folder together and run `Burnout3Takedown-<v>-setup.exe`. It installs for your user only, with a Start Menu entry. SmartScreen will warn (unsigned, built on your machine): More info > Run anyway. | `%LOCALAPPDATA%\Burnout3Takedown\{hdd,config,logs}` |
| **Native Linux, testing** (`dist/Burnout3Takedown-<v>-linux.tar`) | `tar -xf Burnout3Takedown-<v>-linux.tar`, `cd Burnout3Takedown-<v>-linux`, `./launch.sh`. It runs where you unpack it, with no Proton; `lib/` holds DXVK, vkd3d-shader (LGPL-2.1; its source is named in `LICENSES/BUNDLED-LIBRARIES.md`), SDL3, libepoxy and OpenSSL 1.1. For Game Mode, add `launch.sh` as a non-Steam game with no compatibility tool forced. Shader presets work as on Windows (`librashader.so` beside the game, its licence and source link in `shaders/`). The keyboard is off by default, as on Windows; `RECOMP_KEYBOARD=1` in `config/launch.env` turns it on. The steamos bundle stays the one to play with until this one is the default. | the unpacked folder's `{hdd,config,logs}`, or `$BURNOUT3_ROOT` |

Saves (`hdd/`), settings (`config/`) and logs (`logs/`) live outside the
program files and survive every update, rollback and uninstall.

**Updating to a new release.** Run the new release's wizard on the same build folder, or by hand `git pull` and then `./burnout3` in this folder.

**A bundle is private.** It contains your copy of the game and code generated
from it. Keep it on your own machines; never share, upload or publish it.

## Options

The game reads its options from `config/config.toml` in the data folder
above. `config.default.toml` (beside the executable,
`packaging/config.default.toml` here) lists every key at its default; copy
a line into your `config.toml` to change it, for example `scale = 2` under
`[render]` to draw at 1280x960. The wizard writes the options you pick
there. Defaults that differ from the Xbox:

- `storage.ram_cache = true`: the game's cache partition lives in memory
  (up to 750 MB of RAM) instead of being rewritten on your disk.
  `ram_cache = false` under `[storage]` puts the files back on the disk.
- `present.pacing`: the macOS app sleeps in the game's waits (`"sleep"`,
  which frees a CPU core); the Windows build, on a Deck under Proton too,
  keeps the Xbox's timing (`"spin"`). `pacing = "spin"` or
  `pacing = "sleep"` under `[present]` picks either on any build.
- `present.surface = "auto"`: in SteamOS Game Mode the game hands gamescope
  the frame to scale instead of a window (`"window"` restores that).

### The in-game menu

F10, Guide+A or Back+L3 held 0.4 s (and, on macOS, an Elite controller's
P1) opens a menu over the game to change these options while it runs,
rebind the host actions and turn mods on or off; Save keeps only what you
changed in `config/config.toml`. The game keeps running behind it and sees
no input. F3 toggles a stats strip (fps, frame ms, pacing, scale, backend)
in the window's top right corner. On a Steam Deck, SteamOS keeps Guide+A
for itself: use Back+L3, or bind R4 to F10 once in Steam Input (the game's
controller settings > Back Grip Buttons > R4 > Keyboard > F10).
`[menu] enabled = false` turns the menu off.

## Game modes

Modes, as upstream recorded them:

| Mode | Players | Status |
|---|---|---|
| Race (Single Event) | 1 | plays to the finish |
| Time Attack | 1 | plays |
| Road Rage | 1 | plays |
| Crash | 1 | plays: launch, pile-up, score |
| Race | 2, split-screen | plays |
| Road Rage | 2, split-screen | plays |
| Double Impact | 2, split-screen | plays |
| Team Crush | 2, split-screen | plays |
| Party Crash | 2–8, pass the pad | plays |
| Xbox Live / System Link | — | not attempted |

## Known issues

- The title screen and menu music is silent on the Mac.
- No profile saving, so the tutorial video plays at every start.
- Upstream's list for its toolkit: vertical streaks over the menu backdrop
  movie, seams in the sky dome, and bump-mapped texture stages read as zero.
- Xbox Live and System Link are not attempted.
- On the Steam Deck the game starts in a window: add it to Steam with the
  Gamepad template.

## Troubleshooting

- **The wizard or `./burnout3` says "another release".** Your disc is
  another region or revision than the one in the README's supported version box. The build may fail or the game
  misbehave; see the README's supported version box.
- **macOS: "the app is damaged" or cannot be opened.** Gatekeeper: right-click
  the app and pick Open, once, or `xattr -dr com.apple.quarantine /Applications/Burnout3Takedown.app`.
- **Steam Deck: the first launch shows only a notice for a few minutes.**
  It is downloading the Steam Linux Runtime and GE-Proton; the game starts
  when that is done.
- **Windows: SmartScreen warns about the wizard or the setup program.**
  It is not signed: More info > Run anyway.

## Reporting a bug

[Open a bug report](https://github.com/apexJCL/burnout3-recomp/issues/new?template=bug_report.yml) on this repository: the form asks for everything below.
Say which platform and bundle version you run (the version is in the
bundle's name and in `logs/`), what you did, and attach the newest
`logs/game-*.log` and `logs/xbox_kernel.log` from the data folder. Never
attach your game files, a save or the bundle itself. Problems with the wizard or the
`./burnout3` commands themselves go to
[xboxrecomp-cli's issues](https://github.com/apexJCL/xboxrecomp-cli/issues).
