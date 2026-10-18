# Running, recording and driving it headless

## The executable

```
bin\burnout3.exe [--headless] [--record out.mp4] [--seconds N] [path\to\default.xbe]
```

Run it from the repo folder. It reads the disc from `Burnout 3 Takedown\` and
keeps the emulated hard disk (profile, cache partitions) in `saves\`. Delete
`saves\` to go through first boot again.

Built by `./burnout3 build` (llvm-mingw, the Proton path) the exe is
`build-win\burnout3.exe` instead; only upstream's Visual Studio build writes
`bin\`. On the fork toolkit (an environment table), `RECOMP_GAME_FILES` and
`RECOMP_HDD_DIR` move the two folders, so the exe can run from anywhere: the
packaged launchers start it from its logs folder and set both.

- **One window** by default: the CPU rasteriser's framebuffer window, or on
  the fork toolkit the D3D11 renderer's own window (`RECOMP_PB_BACKEND=d3d11`,
  the packaged game's default), never both. The title bar reads
  `Burnout 3: Takedown | FPS: 14 | draws: 212` (with `RECOMP_TRACE=title`
  on the fork toolkit). Upstream's toolkit takes the name from the XBE
  certificate; draws are the draw calls in the last frame.
- **No console window.** The exe is a Windows GUI-subsystem program, so a
  launch from Explorer, the bundle's launcher or Proton opens no console
  beside the game. From a terminal, `cmd` returns at once and the output goes
  nowhere: set `RECOMP_STDIO_LOG=<file>` (the fork toolkit) to keep it, or
  build with `-DBO3_CONSOLE=ON` for the console program, which prints to the
  terminal and keeps it waiting until the game ends.
- **`--headless`** opens no window. Use it for scripts, CI, and over RDP, where a
  window would land on whatever screen the remote session is showing.
- **`--record out.mp4`** writes every presented frame as a BMP into
  `%TEMP%\bo3rec_<pid>\`. When the run ends, after `--seconds N` or on Ctrl+C,
  it encodes them with `ffmpeg` at 30 fps. If ffmpeg is missing or fails, the
  frames stay where they are and the path is printed. The frames are buffered on
  disk rather than piped, because at a few fps there is nothing to gain from
  streaming, and an encoder that fails can't lose the run.

The fps is real frames per wall-clock second. The title steps its simulation a
fixed amount per frame, so at 3 fps a race plays in slow motion but correctly,
and a recording played back at 30 fps looks close to full speed.

## Pads

Two pads are connected by default. Pad 1 is XInput pad 1, pad 2 is XInput pad 2.
There is no keyboard mapping: Burnout 3's menus and races read only the pad.

A **pad script** drives them without hands:

```
set RECOMP_PAD_SCRIPT=20000:start:1200,30000:a:400,90000:rt:60000,90000:p2-rt:60000
```

Each entry is `<ms>:<button>[+<button>...][:<hold ms>]`. Time counts from the
game's first pad read, and the default hold is 150 ms. `RECOMP_PAD_SCRIPT=@file`
reads the entries from a file.

`RECOMP_PAD_LIVE=pad.txt` is the interactive version. Each line appended to the
file, `<button>[+<button>...][:<hold ms>]` with no time, is pressed as soon as it
is read. With frame dumps on, that makes a loop: look at the latest frame,
append a press, repeat.

| Button names | |
|---|---|
| D-pad | `up down left right` |
| Face | `a b x y black white` |
| Triggers | `lt rt` (full pull) |
| Sticks | `lleft lright lup ldown`, `rleft rright rup rdown` (full deflection) |
| Other | `start back lthumb rthumb` |
| Second pad | prefix with `p2-`, e.g. `p2-a`, `p2-rt:60000` |

Timing the menus takes care:

- D-pad presses want about 200 ms. Longer ones auto-repeat once the menus run at
  full speed.
- `a` wants about 400 ms, or 1.5 s while a load is in progress.
- The tutorial video before a mode's first event can't be skipped. It plays
  every launch until profile saving works.

## Environment variables

Defaults that `main.c` sets, which you can override:

| Variable | Default | Why |
|---|---|---|
| `RECOMP_PB_EXEC` | `1` | The software GPU. `0` turns it off: nothing is drawn, but boot runs ~50× faster, which helps when following it by file accesses. |
| `RECOMP_FB_WINDOW` | `1` unless `--headless` | The CPU rasteriser's window. On the fork toolkit (the environment table) only when `RECOMP_PB_BACKEND` is not `d3d11`, which opens its own window; the toolkit ignores the key beside `d3d11` too. |
| `RECOMP_USB` / `RECOMP_USB_PADS` | `1` / `2` | The USB host and pad count (1–4). Split-screen modes are greyed out with one pad. |
| `RECOMP_AC97_READY` | `1` | DirectSound refuses to start until the AC'97 codec reports ready, and RenderWare won't start without audio. |
| `RECOMP_APU_DSP_ACK` | `0x825F8810` | The DSP doorbell DirectSound waits on; [boot.md](boot.md) #5. On the fork toolkit the default is `auto` (`src/env/recomp_env_game.h`). |

On the fork toolkit (`xboxrecomp` with the `recomp_env` table, which
`./burnout3` builds against), the game also reads these toolkit keys, through
the table (every game's packaged launcher sets them; the defaults are
`main.c`'s):

| Variable | Default | Why |
|---|---|---|
| `RECOMP_GAME_FILES` | `Burnout 3 Takedown` | A toolkit key: the unpacked disc; the XBE is `<this>\default.xbe` unless a path is given on the command line. |
| `RECOMP_HDD_DIR` | `saves` | A toolkit key: the emulated hard disk (partition images, the profile). Made with its parents. |
| `RECOMP_STDIO_LOG` | unset | A toolkit key: stdout and stderr go to this file (truncated at start). Proton drops a game's own output, so the bench's `game-stdio.log` and the launchers' `logs\` come from it. |

The packaged launchers set those three below the user-data folder:
`%LOCALAPPDATA%\Burnout3Takedown` on Windows (`BURNOUT3TAKEDOWN_DATA_DIR`
names another, for a test that must not touch the player's saves), and
`~/Games/Burnout3Takedown` on SteamOS (`BURNOUT3_ROOT` at install).

For debugging:

| Variable | Does |
|---|---|
| `RECOMP_FB_DUMP=<prefix>` | Writes the framebuffer to `<prefix>00000.bmp`, ... once per GPU report (`RECOMP_PB_REPORT_MS`, e.g. `5000`). |
| `RECOMP_FB_DUMP_FLIPS=1` | Every flip instead: a consecutive sequence. A file path instead of `1` dumps only while that file exists, so a capture can start mid-game. |
| `RECOMP_PB_REPORT_MS=<ms>` | GPU report interval: fps, flips, draws. |
| `RECOMP_RASTER_THREADS=<n>` | Rasteriser worker threads (default: cores − 4; `1` turns threading off). |
| `RECOMP_WATCHDOG_SECS=<s>` | Hang watchdog. If the title makes no progress for that long, dump the guest call stack (`tools/stackwalk.py` in the toolkit reads it). |
| `RECOMP_USB_STATS=1` | Pad reports and control transfers per pad. |
| `RECOMP_FRAME_TRACE=<file>` | Once `<file>` exists, logs every batch of the next frame (surface, texture, blend, depth, combiners, pixels written) to stderr. Delete and recreate the file to trace another frame. `RECOMP_FRAME_TRACE_METHODS=1` adds every method. |
| `BO3_TRAP_NAN=<file>` | Trap the first x87/SSE NaN once `<file>` exists. That is how the camera NaN (lahf) was found. |

## Options (`config.default.toml`, `config.toml`)

The fork toolkit's enhancements layer is built in (`XBOXRECOMP_ENHANCE`,
on in `CMakeLists.txt`), and every key in it is stock unless something sets it.
The options live in two files. `config.default.toml` sits beside the
executable (in `Contents/Resources/` in the macOS app; the build copies
`packaging/config.default.toml` there) and lists every key at its stock
value. An install or upgrade replaces it, and the game never writes it.
`config/config.toml` holds only the keys the player changed, and nothing
overwrites it. A developer build reads it from the working directory, and
the packaged launchers name the one under the user-data folder through
`RECOMP_CONFIG`. When the file is missing and its folder exists, the game
writes a comment-only header there. An `enhance.toml` left by an older build
is renamed to `config.toml` once.

The lookup order is: the environment, then `config/config.toml` (or the
`RECOMP_CONFIG` file), then `config.default.toml`, then the code default. The
`[ENHANCE]` lines at startup name the files read and the values in force, and
a key the game does not know is reported there too. The toolkit's
`docs/runtime/config.md` has the details. This game has no keys of its own
in the layer.

| Key | Variable | Values | Stock |
|---|---|---|---|
| (the file) | `RECOMP_CONFIG` | a path to the player's `config.toml`, or `none` to read no file at all, not even the defaults file (`RECOMP_ENHANCE_CONFIG` is the old name) | `config/config.toml` |
| `render.scale` | `RECOMP_RENDER_SCALE` | internal resolution factor, 1 to 4 | `1` |
| `display.aspect` | `RECOMP_DISPLAY_ASPECT` | `4:3` (the only one implemented) | `4:3` |
| `present.filter` | `RECOMP_PRESENT_FILTER` | `nearest`, `linear`, `integer` | `nearest` |
| `present.fullscreen` | `RECOMP_PRESENT_FULLSCREEN` | borderless desktop fullscreen at start: `true`/`false` in the file, `1`/`0` in the variable | `false` (`0`) |
| `present.pacing` | `RECOMP_PRESENT_PACING` | `spin` (busy-wait) or `sleep` (the lowered spin waits sleep) | `spin` |
| `textures.pack` | `RECOMP_TEXTURES_PACK` | a texture pack file, or `none` | `none` |
| `textures.font` | `RECOMP_TEXTURES_FONT` | the pack's font-atlas entries, `on`/`off` | `on` |
| `textures.text` | `RECOMP_TEXTURES_TEXT` | the pack's baked-text entries, `on`/`off` | `on` |

The race gate and every bench example here run stock: `RECOMP_CONFIG=none`
and the keys above at their stock values, so a developer's `config.toml`
never changes a measurement. `python3 scripts/race_gate.py --stock-env`
prints those pins as `KEY=VALUE` words for `BENCH_ENV` (`--render-scale N`
for the one A/B), and `--check` fails a run whose `[ENHANCE]` lines show
anything else (check a scale-2 run with `--render-scale 2` too). `-DXBOXRECOMP_ENHANCE=OFF` builds the game
without the layer, for an A/B comparison.

## A scripted session, start to finish

This is how the screenshots and GIFs in the README were made: headless, a live
pad file, and frame dumps that start once the race is running. In Git Bash:

```
mkdir -p runs/mp && touch runs/mp/pad.txt
RECOMP_PAD_LIVE=runs/mp/pad.txt RECOMP_PB_REPORT_MS=5000 \
RECOMP_FB_DUMP=runs/mp/f RECOMP_FB_DUMP_FLIPS=runs/mp/rec.flag \
  ./bin/burnout3.exe --headless > runs/mp/log.txt 2>&1 &

# The log names every file the title opens, which is how to know where it is.
# Wait for ovid\_FEMain.xmv (the title screen), then:
echo start:1200 >> runs/mp/pad.txt
# ...menu presses, one per screen...
# Once the track has streamed in (Tracks\...\streamed.dat in the log):
printf 'rt:600000\np2-rt:600000\n' >> runs/mp/pad.txt
touch runs/mp/rec.flag          # start dumping every flip
```

Then `ffmpeg -framerate 30 -i runs/mp/f%05d.bmp -pix_fmt yuv420p mp.mp4`.

## On a Mac (Metal)

The same `main.c` builds on macOS arm64 with Apple clang, against the fork
toolkit, and plays on the Metal backend:

```
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
./burnout3 build macos          # build/burnout3
./build/burnout3                # a window; --headless for none
```

What differs from Windows:

- `RECOMP_PB_BACKEND` defaults to `metal` (the CPU rasteriser runs a race at
  one or two fps); `RECOMP_PB_BACKEND=cpu` asks for it anyway.
- `RECOMP_TITLE_KEVENTS` defaults to `1`: D3D8 builds the KEVENT its vblank
  DPC sets without `KeInitializeEvent`, and on POSIX the bridge needs a host
  event for it. An explicit value in the environment wins.
- Windowed, the toolkit's SDL host runs the guest on its own thread; the
  window's controllers are the pads (`RECOMP_KEYBOARD=1` adds the keyboard as
  pad 1). Headless (`--headless`, or `RECOMP_HEADLESS=1`) the guest runs on
  the main thread with no window; `RECOMP_PAD_SCRIPT` works as on Windows.
- `--seconds N` ends the run on every host, recording or not.
- A fault prints the same `[FAULT]` report from a SIGBUS/SIGSEGV handler
  (`dladdr` names the generated function; keep the executable unstripped).
- `RECOMP_HANG_WATCHDOG` and `BO3_TRAP_NAN` are Windows-only (they need Win32
  thread contexts); `RECOMP_WATCHDOG_SECS` dumps the guest stack on every
  host. `RECOMP_FB_DUMP` works on Metal too.
- Sound goes through SDL; `SDL_AUDIODRIVER=dummy` runs silent, which is how
  the scripted runs are made. `RECOMP_HDD_DIR` moves the emulated hard disk,
  so a run can start from an empty one:

```
SDL_AUDIODRIVER=dummy RECOMP_HDD_DIR=runs/r1/saves RECOMP_FB_DUMP=runs/r1/f \
RECOMP_FB_DUMP_FLIPS=150 RECOMP_TRACE=flip RECOMP_PAD_SCRIPT=... \
  ./build/burnout3 --headless --seconds 900
```

## The race gate

`scripts/race_gate.py` does that session by itself, from a profile save: it
copies the save's `UDATA` and `TDATA` into the run's own save dir (the save
is only read, and never belongs in the repo), runs the game headless with a
live pad file, and presses through Single Event into a race with the
defaults. Each press waits for its screen: the file the screen opens where
there is one (`Titles30.xmv`, `_FEMain.xmv`, `SaveMeta.xbx`, `Profile 1`,
`CAR1.bgv`, `crash1.rws`, `ident0.awd`), later in the log than the press; a
short settle with the flip count still moving where there is none. A press
counts once the pad reports it (`PAD: live "<press>"`), and a press a
fading screen dropped is repeated, unless a later screen's file has already
opened. RT goes down once the race's sound bank loads and stays down for
60 s (`--mode long`: 600 s).

The save comes from `--fixture` or `$RACE_GATE_FIXTURE`. Runs go to `--out`,
by default the `runs/` folder of a workspace above the checkout; the script
refuses a folder inside the checkout.

```
python3 scripts/race_gate.py --fixture /path/to/workspace/runs/fixtures/b3-save \
    --out /path/to/workspace/runs/gate
python3 scripts/race_gate.py --check /path/to/workspace/runs/gate/<stamp>-race-fast
```

It passes when:

- the race is reached and a frame is dumped after it;
- there is no `[FAULT]` or `[CRASH]` line;
- the flip count never stops for 5 s in the race, and the race runs at
  10 flips/s or more (`--min-race-fps`). A Mac races at about 55 to 75, D3D11
  under Proton at about 13 to 15, and the CPU backend under Proton at about 3
  (use `--min-race-fps 1` there);
- SIGINT stops the game (a hung game needs SIGTERM or SIGKILL);
- the APU's WAV tap grows by at least half the drive's length between race
  start and the end of the drive (about all of it on an idle host, three
  quarters on a loaded one), and that stretch is not silent.

Rates and durations are on the flip trace's clock (`[GPU] flip N T ms`), in
a driven run and a bench run alike. On a Mac the whole fast run takes about
105 s. The route, with the profile loaded:

```
intros -> START on Titles30 -> START ("Press START") -> A (LOAD PROFILE)
-> A (Profile 1) -> A (CONTINUE) -> UP 600 ms, DOWN 15 ms (Single Event)
-> A (RACE) -> A (the options: random region and track, 5 rivals)
-> A (the first car) -> the race; RT held
```

The menu's d-pad repeat counts frames, so a hold that is one step at 30 fps
is four on a fast host: `--nav-ms` sets the DOWN hold (15 ms on the Mac).

### The race gate on the Proton bench

Under Proton it passes on D3D11 (race about 13 flips/s) and, with
`--min-race-fps 1`, on the CPU backend (about 3). Nothing can follow the log
while the bench runs the game, so `--pad-script` prints the same route as a
fixed `RECOMP_PAD_SCRIPT`: START at 100 s (`--start-ms`), a press every 15 s
(`--gap-ms`), DOWN held 50 ms (one step in D3D11's menus, held to 60
flips/s; the CPU backend's run about 15, so give it `--nav-ms 200`), RT 30 s
after the last A and held to the end of the run. It lets the intros play rather than skip them. It prints the
`BENCH_TIMEOUT` the run needs to stderr (505 s for the fast mode).

`RECOMP_SAVE_DIR=@run` starts from an empty save, where this route never
reaches the profile. Keep the save on the bench host outside every synced
tree (a `bench sync` deletes what the tree lacks), copy it to a new folder
for each run (the game writes to it) under `/tmp`, where it is easy to
clear afterwards, and name that folder instead:

```
rsync -a /path/to/workspace/runs/fixtures/b3-save <host>:b3-fixtures/   # once
S=b3-save-$(date +%Y%m%d-%H%M%S)
ssh <host> "mkdir -p /tmp/$S && cp -r ~/b3-fixtures/b3-save/UDATA ~/b3-fixtures/b3-save/TDATA /tmp/$S/"
BENCH_ENV="RECOMP_PAD_SCRIPT=$(python3 scripts/race_gate.py --pad-script) RECOMP_SAVE_DIR=Z:/tmp/$S RECOMP_PB_BACKEND=d3d11 RECOMP_TRACE=flip $(python3 scripts/race_gate.py --stock-env)" \
BENCH_FRAMES=1 BENCH_TIMEOUT=505 ./burnout3 bench run --headless
python3 scripts/race_gate.py --check bench-logs/<stamp>
```

The bench run needs `RECOMP_TRACE=flip`: without flip lines `--check` fails.
`--check` reads the run from its log alone: the race starts at the first
`ident0.awd` after `crash1.rws` and ends at the last flip, and must last the
drive (`--mode`, `--drive-secs`). It reads `exit-code`: 124 is the run
reaching `BENCH_TIMEOUT` and stopping on its SIGINT, which is how this run
ends, and 0 is the game ending by itself; 137 (the game ignored SIGINT and
was killed), any other code, or no file fails. There is no WAV tap there, so the audio is not checked.

## On the Proton bench

`./burnout3 bench` builds the Windows exe on a Linux host and runs it under
Proton there, over ssh (`./burnout3 bench --help`). Copy
`scripts/bench.env.example` to `scripts/bench.env` and set the host. The dump
already on the host (`BENCH_GAME_FILES`) is linked into the host tree as
`Burnout 3 Takedown`; nothing is copied into it.

```
./burnout3 bench setup                 # once per host
./burnout3 bench sync --game-files     # this tree, the toolkit and the CLI; the dump link
./burnout3 bench build                 # build-win/burnout3.exe on the host
```

Each run gets `bench-logs/<stamp>/` on the host, pulled back here:
`game-stdio.log` (the game's output, through `RECOMP_STDIO_LOG`),
`run-info.txt` (the exe's sha256 and the commits it was built from),
`exit-code` and Proton's logs. A `[FAULT]` line fails the run. Give runs a
limit with `BENCH_TIMEOUT` (SIGINT; exit 124 is a run that reached it), not
`--seconds`. `RECOMP_SAVE_DIR=@run` starts each run with an empty hard disk
in its log dir, and `RECOMP_FB_DUMP=@run` dumps the CPU backend's frames to
the run's `frames/` on the host. "Press START" appears about 89 s after
launch (the intros play at their own length now that presents are held to
the 60 Hz vblank), so the scripts press START at 100 s.

```
# boot: the intros, then Press START over the attract movie
BENCH_ENV="$(python3 scripts/race_gate.py --stock-env) RECOMP_FB_DUMP=@run RECOMP_DEBUG=fb_dump_flips=60 RECOMP_SAVE_DIR=@run" \
BENCH_TIMEOUT=180 ./burnout3 bench run --headless

# menu: START, then A every 15 s through the profile screens
BENCH_ENV="$(python3 scripts/race_gate.py --stock-env) RECOMP_PAD_SCRIPT=100000:start:1200,115000:a:400,130000:a:400,145000:a:400,160000:a:400 RECOMP_FB_DUMP=@run RECOMP_DEBUG=fb_dump_flips=60 RECOMP_SAVE_DIR=@run" \
BENCH_TIMEOUT=240 ./burnout3 bench run --headless

# race: the profile flow into a race, then RT held from 285 s
BENCH_ENV="$(python3 scripts/race_gate.py --stock-env) RECOMP_PAD_SCRIPT=100000:start:1200,115000:a:400,130000:a:400,145000:up:300,160000:a:400,175000:a:400,190000:a:400,205000:a:400,220000:a:400,235000:a:400,250000:a:400,265000:a:400,285000:rt:600000 RECOMP_FB_DUMP=@run RECOMP_DEBUG=fb_dump_flips=150 RECOMP_SAVE_DIR=@run" \
BENCH_TIMEOUT=900 ./burnout3 bench run --headless
```

The CPU backend dumps at most 200 frames a run (`fb_dump_flips` spaces
them: about 60 flips a second in the front end). For the D3D11 renderer add
`RECOMP_PB_BACKEND=d3d11 RECOMP_TRACE=flip` to `BENCH_ENV` and set
`BENCH_FRAMES=1` (its frames, every 60th present, go to the run's
`frames/`). That backend opens its own window. `RECOMP_TRACE=flip` gives the
run's `flips:` count and the bench's check that every present shows the
surface the game drew; without it both read "not checked".
