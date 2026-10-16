# Running, recording and driving it headless

## The executable

```
bin\burnout3.exe [--headless] [--record out.mp4] [--seconds N] [path\to\default.xbe]
```

Run it from the repo folder. It reads the disc from `Burnout 3 Takedown\` and
keeps the emulated hard disk (profile, cache partitions) in `saves\`. Delete
`saves\` to go through first boot again.

It is a Windows GUI program: launched from Explorer, a shortcut or
`Play Burnout 3.cmd`, it opens the game's window and nothing else, and
closing that window ends the run. If a startup step fails (the XBE is not
where it looks, the Xbox address range cannot be mapped, an unknown option),
a dialog says which, unless `--headless` or `RECOMP_HEADLESS` is set.
`Play Burnout 3.cmd` starts the exe and returns, so its exit code is not
passed on.

From a terminal the prompt comes back at once and the exe prints nowhere,
so the `[FATAL]` and `[FAULT]` reports and the boot log are lost unless you
keep them. Any of these does:

1. **`RECOMP_STDIO_LOG=<file>`**: stdout and stderr go to `<file>`,
   truncated at start. In PowerShell:
   `$env:RECOMP_STDIO_LOG = "bo3.log"; .\bin\burnout3.exe --headless`, then
   `Get-Content bo3.log -Wait`.
2. **A redirect from cmd**: `bin\burnout3.exe > bo3.log 2>&1`.
3. **A console build**, which prints to the terminal it starts from and
   waits for the game to end, as builds before this option did:
   `cmake -S . -B build-xr -DBO3_CONSOLE=ON`, then
   `cmake --build build-xr --config Release`.

Ctrl+C in a terminal does not reach the GUI exe, so a `--record` run ends
(and encodes) after `--seconds N` or when its window is closed. A
`--headless --record` run has no window to close, so give it `--seconds`,
or use the console build and stop it with Ctrl+C.

The options:

- **Windowed** by default. The title bar reads
  `Burnout 3: Takedown | FPS: 14 | draws: 212`. The title comes from the XBE
  certificate; draws are the draw calls in the last frame.
- **`--headless`** opens no window. Use it for scripts, CI, and over RDP, where a
  window would land on whatever screen the remote session is showing.
- **`--record out.mp4`** writes every presented frame as a BMP into
  `%TEMP%\bo3rec_<pid>\`. When the run ends, after `--seconds N`, when the
  window is closed or (console build) on Ctrl+C, it encodes them with `ffmpeg`
  at 30 fps. If ffmpeg is missing or fails, the frames stay where they are and
  the path is printed. The frames are buffered on disk rather than piped,
  because at a few fps there is nothing to gain from streaming, and an encoder
  that fails can't lose the run.

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
| `RECOMP_FB_WINDOW` | `1` unless `--headless` | The window. |
| `RECOMP_USB` / `RECOMP_USB_PADS` | `1` / `2` | The USB host and pad count (1–4). Split-screen modes are greyed out with one pad. |
| `RECOMP_AC97_READY` | `1` | DirectSound refuses to start until the AC'97 codec reports ready, and RenderWare won't start without audio. |
| `RECOMP_APU_DSP_ACK` | `0x825F8810` | The DSP doorbell DirectSound waits on; [boot.md](boot.md) #5. |

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
