#!/usr/bin/env python3
"""race_gate.py: the Burnout 3 race gate, driven screen by screen.

  python3 scripts/race_gate.py --fixture DIR [--out DIR] [--mode fast|long]
                               [--exe build/burnout3] [--name NAME]
                               [--drive-secs S] [--timeout S]
                               [--render-scale N]
  python3 scripts/race_gate.py --pad-script [--mode fast|long] [--start-ms MS]
                               [--gap-ms MS] [--nav-ms MS] [--race-ms MS]
  python3 scripts/race_gate.py --check RUNDIR [--mode fast|long]
                               [--min-race-fps N] [--render-scale N]
  python3 scripts/race_gate.py --stock-env [--render-scale N]

Runs the game headless from a copy of a profile save (the fixture: UDATA and
TDATA from a real install, never modified, never committed; --fixture or
$RACE_GATE_FIXTURE), and presses through the front end into a Single Event
race with the defaults:

  intros (START skips each) -> "Press START" -> load the profile ->
  main menu, 2nd entry "Single Event" -> the defaults -> the race

Each press waits for the screen it answers instead of a fixed time: the log
names every file the title opens ([FILE] lines), and the flip trace
(RECOMP_TRACE=flip) says the screen is drawing. A press goes to the game
through RECOMP_PAD_LIVE, a file the toolkit's pad reads while it runs, and
counts once the pad reports it (PAD: live "<press>"). Once the race has
streamed in, RT is held for --drive-secs (fast: 60, long: 600, the static
hunts' length) and the run ends. Runs go to --out, by default the runs/
folder of a workspace above the checkout; never inside the checkout.

Checks, written to RUNDIR/gate.json and printed: the race was reached and a
frame was dumped after it; no [FAULT] (or [CRASH]) line; the flips kept
moving through the drive, at no less than --min-race-fps; SIGINT stopped the
game; the audio tap (RECOMP_DEBUG=audio_wav) grew by the drive's length
while the race played, and is not silent there. Exit 0 when every check
passes. The run pins the enhancements layer at stock (RECOMP_CONFIG=none and
the stock keys); --render-scale is the only one it varies, and the check
fails a log whose [ENHANCE] lines show anything else. --stock-env prints the
same pins for a bench run's BENCH_ENV.

--pad-script prints the same route as a fixed RECOMP_PAD_SCRIPT timeline, for
a runner that cannot feed RECOMP_PAD_LIVE (./burnout3 bench run under
Proton): the waits become fixed gaps, so it is slower and less robust than
the driven run, and the intros play out.

Standard library only; Python 3.9.
"""

import argparse
import array
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FIXTURE_ENV = "RACE_GATE_FIXTURE"

# The enhancements layer at stock, whatever the shell or a config.toml says:
# no settings file at all (the defaults file beside the exe included), and
# every one of the toolkit's keys at its stock value (the code defaults in
# its enhance.c), so a gate run measures the game as it ships.
# --render-scale is the one A/B it takes; check() reads the [ENHANCE] lines
# back to prove the run was this.
STOCK_ENV = {
    "RECOMP_CONFIG": "none",
    "RECOMP_RENDER_SCALE": "1",
    "RECOMP_RENDER_ANISO": "0",
    "RECOMP_DISPLAY_ASPECT": "4:3",
    "RECOMP_PRESENT_FILTER": "nearest",
    "RECOMP_PRESENT_FULLSCREEN": "0",
    "RECOMP_PRESENT_PACING": "spin",
    "RECOMP_TEXTURES_PACK": "none",
    "RECOMP_TEXTURES_FONT": "on",
    "RECOMP_TEXTURES_TEXT": "on",
}

MODES = {"fast": 60, "long": 600}

# Flips that stop for this long mean a hung or frozen game. The slowest
# flip seen anywhere in a race run is under half a second (the CPU backend
# under Proton, about 3 flips/s); a menu or track load stays under that too.
STALL_SECS = 5.0

# The race's flips per second (flip count over the flip trace's own clock),
# below which the run fails. Mac Metal raced at 52 and 74 flips/s (the
# first two passing gate runs; held to the vblank, about 56); D3D11 under
# Proton at about 13 to 15. Ten is a
# fifth of the slowest Mac run and below the Proton one, so it trips on a
# game crawling at a few frames a second, not on a slow host. The CPU
# backend under Proton races at about 3: pass --min-race-fps 1 there.
MIN_RACE_FPS = 10.0

# The WAV tap writes what the device is given, so a race playing at speed
# grows it by about the drive's length: 97% of wall time on an idle Mac, 76%
# with a parallel build loading it (load average 70). Half catches an APU
# that stopped or stutters, not a busy host.
AUDIO_MIN_FRACTION = 0.5
# The toolkit's tap (apu_wav.c) writes a plain 44-byte header.
WAV_HEADER = 44

FLIP = re.compile(r"^\[GPU\] flip (\d+) (\d+) ms")
FILE_OPEN = re.compile(r"\[FILE\] (\S.*?) \(disp ")
DUMP = re.compile(r"\[GPU\] flip (\d+): frame dump")
D3D11_DUMP = "[d3d11] dumped "
PAD_LIVE = re.compile(r'PAD: live "(.*)" at t=')
CRASH = ("[FAULT]", "[CRASH]")
# The layer's startup lines: the settings files read ("none (RECOMP_CONFIG)"
# when there are none; "renamed ..."/"cannot ..." lines are about the user
# file, not the files read), and the values in force. render.aniso sits
# after the scale from the toolkit that adds it on; older toolkits omit it.
ENHANCE_CONFIG = re.compile(r"^\[ENHANCE\] config: (?!renamed |cannot )(.*)$")
ENHANCE_VALUES = re.compile(
    r"^\[ENHANCE\] render\.scale=(\d+) (?:render\.aniso=(\d+) )?present\.filter=(\S+) "
    r"present\.fullscreen=(\d+) present\.pacing=(\S+)"
)

# The route, explored on the Mac with a loaded profile.
# Each step waits for its event, settles, presses, and may verify:
#   wait    "file:<substring>": the title opens a file whose path contains it
#           (case-insensitive), later in the log than the previous step's
#           answer (or its press, when it has no "until");
#           "secs:<s>": s seconds after the previous step's press
#   settle  seconds after the event before the press, with the screen still
#           flipping (a file opens before its screen takes input, and a menu
#           fades in); a stalled flip count fails the step
#   press   one pad line, or a list sent one at a time, 1 s apart
#   until   a file the press makes the title open, later in the log than the
#           press; until it does the press is repeated every "retry" seconds
#           (a screen drops input while it fades in), at most "tries" times,
#           unless a file a later step waits for has opened: then the route
#           is off and the step fails instead of pressing blind
#   timeout seconds to wait for the event, or for "until"
# Only some screens open a file. The profile list, "Load successful", the
# main menu, SELECT MODE and the race options open none, so those steps go
# by time. Every A from "Load successful" to SELECT CAR moves one screen on,
# which is why the last of them can repeat until the car loads.
#
# The d-pad: the Mac front end flips at several hundred per second, so the
# menu's auto-repeat fires within a 60 ms hold (four entries); 15 ms moves
# one. UP held 600 ms parks on the first entry (World Tour); the menu does
# not wrap there.
ROUTE = [
    # The pad enumerates with the first intro, about 1 s before the third
    # (Titles30). START there skips to "Press START", which plays _FEMain.xmv.
    {
        "name": "intros",
        "wait": "file:Titles30.xmv",
        "timeout": 120,
        "settle": 0.5,
        "press": "start:300",
        "until": "_FEMain.xmv",
        "retry": 3,
        "tries": 5,
    },
    # START: the profile prompt reads the save's metadata.
    {
        "name": "press start",
        "wait": "secs:0",
        "settle": 2,
        "press": "start:300",
        "until": "SaveMeta.xbx",
        "retry": 3,
        "tries": 5,
    },
    # LOAD PROFILE (highlighted): the list of saved profiles.
    {
        "name": "load profile",
        "wait": "secs:0",
        "settle": 2,
        "press": "a:300",
        "until": "Profile 1",
        "retry": 3,
        "tries": 4,
    },
    # The first profile: "Load successful", CONTINUE.
    {"name": "profile 1", "wait": "secs:0", "settle": 2.5, "press": "a:300"},
    # CONTINUE: the main menu.
    {"name": "continue", "wait": "secs:0", "settle": 2.5, "press": "a:300"},
    # Main menu, 2nd entry: Single Event (SELECT MODE, RACE highlighted).
    {
        "name": "single event",
        "wait": "secs:0",
        "settle": 2.5,
        "press": ["up:600", "down:NAV", "a:300"],
    },
    # RACE: the race options (Region and Track Random, Rivals 5).
    {"name": "race", "wait": "secs:0", "settle": 2.5, "press": "a:300"},
    # The options as they are: SELECT CAR loads the first car.
    {
        "name": "options",
        "wait": "secs:0",
        "settle": 2.5,
        "press": "a:300",
        "until": "CAR1.bgv",
        "retry": 3,
        "tries": 3,
    },
    # The car as it is: the race loads; crash1.rws comes last of the track.
    {
        "name": "select car",
        "wait": "secs:0",
        "settle": 2.5,
        "press": "a:300",
        "until": "crash1.rws",
        "retry": 4,
        "tries": 3,
        "timeout": 60,
    },
    # The race's music and announcer bank: the flyover is over and the
    # countdown runs. RT goes down here and stays down.
    {"name": "race start", "wait": "file:ident0.awd", "timeout": 60},
]


def find_runs_dir(start=ROOT):
    """The workspace's runs/ above the checkout (or a worktree of it), never
    the checkout's own: run output does not belong in a repo."""
    d = os.path.dirname(start)
    for _ in range(4):
        cand = os.path.join(d, "runs")
        if os.path.isdir(cand):
            return cand
        d = os.path.dirname(d)
    return None


def inside(path, root):
    path, root = os.path.realpath(path), os.path.realpath(root)
    return path == root or path.startswith(root + os.sep)


def seed_save(fixture, dest):
    """Copy UDATA and TDATA from the fixture into the run's own save root.
    The fixture is only read."""
    os.makedirs(dest, exist_ok=True)
    for sub in ("UDATA", "TDATA"):
        src = os.path.join(fixture, sub)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(dest, sub))
    if not os.path.isdir(os.path.join(dest, "UDATA")):
        sys.exit("race_gate: %s has no UDATA" % fixture)


class Log:
    """Follows the game's log as it is written: flips, file opens, the pad's
    live presses, crashes. Order in the log is what counts: opens is a list,
    and a step asks for a file at or after an index into it."""

    def __init__(self, path, clock=time.monotonic):
        self.path = path
        self.clock = clock
        self.f = None
        self.buf = ""
        self.flips = 0
        self.flip_ms = 0
        self.opens = []  # (clock s, flip, path)
        self.pad_live = []  # the presses the pad reported, in order
        self.crash = None

    def poll(self):
        if self.f is None:
            if not os.path.exists(self.path):
                return
            self.f = open(self.path, encoding="utf-8", errors="replace")
        chunk = self.f.read()
        if not chunk:
            return
        self.feed(chunk)

    def feed(self, chunk):
        self.buf += chunk
        lines = self.buf.split("\n")
        self.buf = lines.pop()
        now = self.clock()
        for line in lines:
            m = FLIP.match(line)
            if m:
                self.flips = int(m.group(1))
                self.flip_ms = int(m.group(2))
                continue
            if "[FILE]" in line:
                m = FILE_OPEN.search(line)
                if m and "FAILED" not in line:
                    self.opens.append((now, self.flips, m.group(1)))
                continue
            m = PAD_LIVE.search(line)
            if m:
                self.pad_live.append(m.group(1))
            elif self.crash is None and any(c in line for c in CRASH):
                self.crash = line.strip()

    def mark(self):
        """The index the next file open will get, with the log read up to
        now: a file at or after it opened after this call."""
        self.poll()
        return len(self.opens)

    def opened_since(self, sub, since):
        """The index of the first open at or after `since` whose path holds
        `sub` (case-insensitive), or None."""
        sub = sub.lower()
        for i in range(since, len(self.opens)):
            if sub in self.opens[i][2].lower():
                return i
        return None


class Failure(Exception):
    pass


class Timeout(Failure):
    pass


def later_files(route, i):
    """The files the steps after route[i] wait for."""
    out = []
    for st in route[i + 1 :]:
        kind, _, arg = st["wait"].partition(":")
        if kind == "file":
            out.append(arg)
        if st.get("until"):
            out.append(st["until"])
    return out


class Runner:
    """Walks the route against a followed log. The log, the pad, the clock
    and the process are passed in, so the tests drive it with a fake game."""

    def __init__(
        self,
        log,
        press,
        clock=time.monotonic,
        sleep=time.sleep,
        exited=lambda: None,
        wav_size=lambda: None,
        nav_ms=15,
        stall_secs=STALL_SECS,
        pad_ack_secs=3.0,
        poll_secs=0.05,
        quiet=False,
    ):
        self.log = log
        self.press_fn = press
        self.clock = clock
        self.sleep = sleep
        self.exited = exited
        self.wav_size = wav_size
        self.nav_ms = nav_ms
        self.stall_secs = stall_secs
        self.pad_ack_secs = pad_ack_secs
        self.poll_secs = poll_secs
        self.quiet = quiet
        self.t0 = clock()
        self.last_press = 0.0
        self.events = []

    def now(self):
        return self.clock() - self.t0

    def ev(self, name, **kw):
        e = {"t": round(self.now(), 2), "flip": self.log.flips, "event": name}
        e.update(kw)
        self.events.append(e)
        if not self.quiet:
            print("  [%7.1fs flip %6d] %s %s" % (e["t"], e["flip"], name, kw or ""), flush=True)

    def wait(self, pred, timeout, step, stall=None):
        """Poll until pred() is true. Fails on a crash line, on the game
        exiting, after `timeout` s, and, with `stall`, when the flip count
        has not moved for `stall` s."""
        end = self.clock() + timeout
        moved, last = self.clock(), self.log.flips
        while True:
            self.log.poll()
            if self.log.crash:
                raise Failure("crash during %s: %s" % (step, self.log.crash))
            rc = self.exited()
            if rc is not None:
                raise Failure("the game exited (%s) during %s" % (rc, step))
            r = pred()
            if r is not None and r is not False:
                return r
            now = self.clock()
            if self.log.flips != last:
                moved, last = now, self.log.flips
            elif stall is not None and now - moved >= stall:
                raise Failure(
                    "the flips stopped at %d for %.0f s during %s" % (last, now - moved, step)
                )
            if now >= end:
                raise Timeout("timed out after %d s waiting for %s" % (timeout, step))
            self.sleep(self.poll_secs)

    def sleep_for(self, secs, step, stall=None):
        end = self.clock() + secs
        self.wait(lambda: self.clock() >= end, secs + 5, step, stall)

    def press(self, what, step):
        """Write one pad line and wait for the pad to report it."""
        what = what.replace("NAV", str(self.nav_ms))
        n = len(self.log.pad_live)
        self.press_fn(what)
        self.last_press = self.now()
        self.ev("press " + what, step=step)
        try:
            self.wait(lambda: len(self.log.pad_live) > n, self.pad_ack_secs, step)
        except Timeout:
            raise Failure(
                "the pad did not report %s within %.0f s (%s)" % (what, self.pad_ack_secs, step)
            ) from None
        if self.log.pad_live[n] != what:
            raise Failure("the pad reported %r for %r (%s)" % (self.log.pad_live[n], what, step))

    def route(self, route):
        """Walk the steps; returns with the last step's event seen."""
        mark = 0
        for i, st in enumerate(route):
            kind, _, arg = st["wait"].partition(":")
            name = st["name"]
            timeout = st.get("timeout", 60)
            if kind == "file":
                idx = self.wait(lambda a=arg, m=mark: self.log.opened_since(a, m), timeout, name)
                mark = idx + 1
                self.ev("seen " + arg, step=name)
            else:
                left = self.last_press + float(arg) - self.now()
                if left > 0:
                    self.sleep_for(left, name)
            settle = st.get("settle", 0)
            if settle:
                base = self.log.flips
                self.sleep_for(settle, name + " settle", stall=settle)
                if self.log.flips <= base:
                    raise Failure("no flips while settling for %s (flip %d)" % (name, base))
            presses = st.get("press") or []
            if isinstance(presses, str):
                presses = [presses]
            if not presses:
                continue
            until = st.get("until")
            tries = st.get("tries", 1)
            later = later_files(route, i)
            sent = self.log.mark()  # a late answer to an earlier try counts
            for attempt in range(tries):
                if attempt:
                    self.log.poll()
                    for f in later:
                        if self.log.opened_since(f, sent) is not None:
                            raise Failure(
                                "%s opened before %s (%s): the route is off" % (f, until, name)
                            )
                for j, what in enumerate(presses):
                    if j:
                        self.sleep_for(1.0, name)
                    self.press(what, name)
                if not until:
                    break
                retry = st.get("retry", 3) if attempt + 1 < tries else timeout
                try:
                    idx = self.wait(
                        lambda u=until, s=sent: self.log.opened_since(u, s), retry, name
                    )
                except Timeout:
                    if attempt + 1 == tries:
                        raise
                    continue
                self.ev("seen " + until, step=name, tries=attempt + 1)
                break
            mark = idx + 1 if until else sent

    def race(self, drive_secs):
        """Hold RT for the drive with the flips watched; returns the race's
        start and end (runner clock s, flip, the flip trace's ms) and the
        WAV tap's size at each."""
        self.log.poll()
        out = {
            "race_start": {
                "t": round(self.now(), 2),
                "flip": self.log.flips,
                "ms": self.log.flip_ms,
            },
            "wav_bytes": [self.wav_size()],
        }
        self.ev("race start")
        self.press("rt:%d" % ((drive_secs + 5) * 1000), "race start")
        self.ev("drive", secs=drive_secs)
        self.sleep_for(drive_secs, "the drive", stall=self.stall_secs)
        self.log.poll()
        out["wav_bytes"].append(self.wav_size())
        out["race_end"] = {
            "t": round(self.now(), 2),
            "flip": self.log.flips,
            "ms": self.log.flip_ms,
        }
        self.ev("drive end")
        return out


def file_size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return None


def stock_env(render_scale=1):
    """The stock pins for a gate run, with the render scale under test."""
    env = dict(STOCK_ENV)
    env["RECOMP_RENDER_SCALE"] = str(render_scale)
    return env


def stock_env_words(render_scale=1):
    """stock_env() as KEY=VALUE words, the form BENCH_ENV takes."""
    return " ".join("%s=%s" % kv for kv in stock_env(render_scale).items())


def drive(args, run):
    """Launch the game, walk the route, drive. Returns the run's result."""
    exe = os.path.abspath(args.exe)
    log_path = os.path.join(run, "game-stdio.log")
    pad = os.path.join(run, "pad.txt")
    wav = os.path.join(run, "audio.wav")
    open(pad, "w").close()
    env = dict(os.environ)
    env.update(
        {
            "SDL_AUDIODRIVER": env.get("SDL_AUDIODRIVER", "dummy"),
            "RECOMP_SAVE_DIR": os.path.join(run, "save"),
            "RECOMP_HDD_DIR": os.path.join(run, "hdd"),
            "RECOMP_PAD_LIVE": pad,
            "RECOMP_TRACE": "flip",
            "RECOMP_DEBUG": "fb_dump=%s,fb_dump_flips=%d,audio_wav=%s"
            % (os.path.join(run, "frames", "f"), args.dump_every, wav),
        }
    )
    if os.environ.get("RECOMP_DEBUG"):
        env["RECOMP_DEBUG"] += "," + os.environ["RECOMP_DEBUG"]
    if os.environ.get("RECOMP_TRACE"):
        env["RECOMP_TRACE"] += "," + os.environ["RECOMP_TRACE"]
    env.update(stock_env(args.render_scale))
    # The DSOUND doorbell ack: without it the boot stops after flip 2, right
    # after B3XboxFx.bin. A game default on main, a toolkit default only from
    # f465cd3 on, and the macOS host branch dropped its own: say it here so
    # the gate boots on either toolkit.
    env.setdefault("RECOMP_APU_DSP_ACK", "auto")
    os.makedirs(os.path.join(run, "frames"), exist_ok=True)
    cmd = [exe, "--headless", "--seconds", str(int(args.timeout))]
    with open(os.path.join(run, "env.txt"), "w") as f:
        keys = sorted(k for k in env if k.startswith(("RECOMP_", "SDL_")))
        f.write(" ".join("%s=%s" % (k, env[k]) for k in keys) + "\n")
        f.write(" ".join(cmd) + "\n")

    def append(what):
        with open(pad, "a") as f:
            f.write(what + "\n")

    out = open(log_path, "w")
    proc = subprocess.Popen(cmd, cwd=args.cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    log = Log(log_path)
    runner = Runner(
        log, append, exited=proc.poll, wav_size=lambda: file_size(wav), nav_ms=args.nav_ms
    )
    result = {"events": runner.events}
    try:
        runner.route(ROUTE)
        result.update(runner.race(args.drive_secs))
    except Failure as e:
        result["failure"] = str(e)
        print("race_gate: " + str(e), flush=True)
    finally:
        result["stop"] = stop(proc)
        out.close()
        log.poll()
        result["exit"] = proc.returncode
        result["wall"] = round(runner.now(), 2)
        result["crash"] = log.crash
    return result


def stop(proc, grace=10.0):
    """SIGINT (the game ends as at --seconds, flushing the WAV), then SIGTERM,
    then kill: a title stuck in a wait ignores SIGINT. Returns what ended it:
    "exited" (it was already gone), "INT", "TERM" or "KILL"."""
    if proc.poll() is not None:
        return "exited"
    for sig, name in ((signal.SIGINT, "INT"), (signal.SIGTERM, "TERM")):
        proc.send_signal(sig)
        try:
            proc.wait(grace)
            return name
        except subprocess.TimeoutExpired:
            pass
    proc.kill()
    proc.wait()
    return "KILL"


def wav_rms(path, start_s=0.0, end_s=None):
    """RMS (dBFS) of a 16-bit WAV between two times, and its length in s."""
    try:
        w = wave.open(path, "rb")
    except (OSError, EOFError, wave.Error):
        return None, 0.0
    with w:
        rate, width = w.getframerate(), w.getsampwidth()
        n = w.getnframes()
        length = n / float(rate) if rate else 0.0
        if width != 2 or not rate:
            return None, length
        a = int(start_s * rate)
        b = n if end_s is None else min(n, int(end_s * rate))
        if b <= a:
            return None, length
        w.setpos(a)
        acc, count = 0.0, 0
        left = b - a
        while left > 0:
            k = min(left, rate)
            s = array.array("h", w.readframes(k))
            if sys.byteorder == "big":
                s.byteswap()
            acc += sum(x * x for x in s)
            count += len(s)
            left -= k
    if not count or acc == 0:
        return -200.0, length
    return 20 * math.log10(math.sqrt(acc / count) / 32768.0), length


def wav_format(path):
    """(rate, bytes per sample frame) of a WAV, or None."""
    try:
        with wave.open(path, "rb") as w:
            return w.getframerate(), w.getnchannels() * w.getsampwidth()
    except (OSError, EOFError, wave.Error):
        return None


def result_from_log(run):
    """The timeline of a run nobody drove from here (a --pad-script run on the
    bench): the race starts at the first ident0.awd after crash1.rws, as in
    the route, and ends at the last flip."""
    flip, ms, track, race, crash = 0, 0, False, None, None
    with open(os.path.join(run, "game-stdio.log"), errors="replace") as f:
        for line in f:
            m = FLIP.match(line)
            if m:
                flip, ms = int(m.group(1)), int(m.group(2))
            elif "[FILE]" in line and "FAILED" not in line:
                low = line.lower()
                track = track or "crash1.rws" in low
                if track and race is None and "ident0.awd" in low:
                    race = {"flip": flip, "ms": ms}
            elif crash is None and any(c in line for c in CRASH):
                crash = line.strip()
    result = {"crash": crash, "race_end": {"flip": flip, "ms": ms}}
    if race:
        result["race_start"] = race
    return result


def bench_exit(run, fails, rep):
    """The bench's exit-code: 124 is a run that reached BENCH_TIMEOUT and
    stopped on its SIGINT, which is how a --pad-script run ends (RT is held
    to the limit); 0 is the game ending by itself. 137 is a game that
    ignored SIGINT and was killed (hung); anything else ended it early."""
    try:
        with open(os.path.join(run, "exit-code")) as f:
            rc = f.read().strip()
    except FileNotFoundError:
        rc = None
    rep["exit_code"] = rc
    if not rc:
        fails.append("no exit-code (not a bench run, or the host script died)")
    elif rc == "137":
        fails.append("exit 137: the game ignored SIGINT at the limit and was killed")
    elif rc not in ("0", "124"):
        fails.append("exit %s: the game ended before the limit" % rc)


def check(
    run,
    result=None,
    silence_db=-50.0,
    min_race_fps=MIN_RACE_FPS,
    stall_secs=STALL_SECS,
    drive_secs=None,
    render_scale=1,
):
    """The gate's checks on a run directory; returns (ok, report dict). A run
    without gate.json is a bench run, read from its log and exit-code; it
    has no audio tap. Every rate and duration here is on the flip trace's
    clock (the "[GPU] flip N T ms" lines), so a driven run and a bench run
    compare. The [ENHANCE] lines must show a stock run at render_scale."""
    live = True
    if result is None:
        try:
            with open(os.path.join(run, "gate.json")) as f:
                result = json.load(f)
        except FileNotFoundError:
            result, live = result_from_log(run), False
    drive_secs = result.get("drive_secs") or drive_secs
    rep = {"run": run, "wall_s": result.get("wall")}
    fails = []
    if result.get("failure"):
        fails.append(result["failure"])
    if result.get("crash"):
        fails.append("crash line: " + result["crash"])
    if live:
        rep["stop"] = result.get("stop")
        if result.get("stop") in ("TERM", "KILL"):
            fails.append("the game ignored SIGINT and needed SIG%s to stop" % result["stop"])
    else:
        bench_exit(run, fails, rep)
    race = result.get("race_start")
    end = result.get("race_end")
    rep["race_reached"] = bool(race)
    if not race:
        fails.append("the race was not reached")
    flips, dumps = [], []
    config = values = None
    with open(os.path.join(run, "game-stdio.log"), errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            m = ENHANCE_CONFIG.match(line)
            if m:
                config = m.group(1)
                continue
            m = ENHANCE_VALUES.match(line)
            if m:
                values = m.groups()
                continue
            m = FLIP.match(line)
            if m:
                flips.append((int(m.group(1)), int(m.group(2))))
                continue
            m = DUMP.search(line)
            if m:
                dumps.append(int(m.group(1)))
            elif D3D11_DUMP in line:
                dumps.append(flips[-1][0] if flips else 0)
    # A frame dumped after the race started. The dump files are numbered in
    # order; the log says which flip each one is ("flip N: frame dump"), or
    # for D3D11 (BENCH_FRAMES=1) the flip line before it.
    enhance_checks(config, values, render_scale, rep, fails)
    rep["frames"] = len(dumps)
    if race:
        after = [n for n in dumps if n > race["flip"]]
        rep["race_frames"] = len(after)
        if not after:
            fails.append("no frame dumped after the race started")
    if not flips:
        fails.append("no flip lines in the log (run with RECOMP_TRACE=flip)")
    else:
        rep["flips"] = flips[-1][0]
        rep["flips_per_s"] = round(flips[-1][0] / max(flips[-1][1] / 1000.0, 1e-3), 1)
    in_race = []
    if race and end and flips:
        in_race = [x for x in flips if race["flip"] <= x[0] <= end["flip"]]
    if len(in_race) >= 2:
        (f0, ms0), (f1, ms1) = in_race[0], in_race[-1]
        secs = (ms1 - ms0) / 1000.0
        rep["race_flips"] = f1 - f0
        rep["race_secs"] = round(secs, 1)
        rep["race_flips_per_s"] = round((f1 - f0) / max(secs, 1e-3), 1)
        if rep["race_flips_per_s"] < min_race_fps:
            fails.append(
                "the race ran at %.1f flips/s (floor %.0f)"
                % (rep["race_flips_per_s"], min_race_fps)
            )
        gap, at = max((b[1] - a[1], a[0]) for a, b in zip(in_race, in_race[1:]))
        rep["race_max_flip_gap_ms"] = gap
        if gap >= stall_secs * 1000:
            fails.append("the flips stopped at %d for %.1f s in the race" % (at, gap / 1000.0))
        if drive_secs and secs < 0.9 * drive_secs:
            fails.append("the race ran %.0f s of the %d s drive" % (secs, drive_secs))
    elif race and end and flips:
        fails.append("no flips in the race")
    if live:
        audio(run, result, rep, fails, silence_db)
    rep["failures"] = fails
    rep["ok"] = not fails
    return not fails, rep


def enhance_checks(config, values, render_scale, rep, fails):
    """The run was stock but for the scale under test: no settings file read
    and the layer's values those of STOCK_ENV. A build without the layer
    prints no [ENHANCE] lines, and its run proves nothing about them."""
    rep["enhance_config"] = config
    if values is None:
        fails.append("no [ENHANCE] values line (a build without the enhancements layer?)")
        return
    scale, aniso, filt, fullscreen, pacing = values
    rep["enhance"] = {
        "render.scale": int(scale),
        "render.aniso": int(aniso or 0),
        "present.filter": filt,
        "present.fullscreen": int(fullscreen),
        "present.pacing": pacing,
    }
    if config is None:
        fails.append("no [ENHANCE] config line")
    elif not config.startswith("none"):
        fails.append("the run read settings files: %s (want RECOMP_CONFIG=none)" % config)
    want = {
        "render.scale": render_scale,
        "render.aniso": int(STOCK_ENV["RECOMP_RENDER_ANISO"]),
        "present.filter": STOCK_ENV["RECOMP_PRESENT_FILTER"],
        "present.fullscreen": int(STOCK_ENV["RECOMP_PRESENT_FULLSCREEN"]),
        "present.pacing": STOCK_ENV["RECOMP_PRESENT_PACING"],
    }
    for key, value in want.items():
        if rep["enhance"][key] != value:
            fails.append("%s=%s, want %s" % (key, rep["enhance"][key], value))


def audio(run, result, rep, fails, silence_db):
    """The WAV tap over the drive: the bytes written between race start and
    drive end (its size at each), which must cover most of the drive and
    not be silent. Measuring that range, not the file's tail, keeps menu
    audio out of it."""
    wav = os.path.join(run, "audio.wav")
    fmt = wav_format(wav)
    _, length = wav_rms(wav)
    rep["audio_s"] = round(length, 1)
    sizes = result.get("wav_bytes") or []
    race, end = result.get("race_start"), result.get("race_end")
    if fmt is None:
        fails.append("no audio tap (audio.wav)")
        return
    if len(sizes) != 2 or None in sizes or not race or not end:
        if race and end:
            fails.append("no WAV sizes for the drive")
        return
    rate, frame = fmt
    a = max(0, sizes[0] - WAV_HEADER) // frame
    b = max(0, sizes[1] - WAV_HEADER) // frame
    grew = (b - a) / float(rate)
    drive = end["t"] - race["t"]
    rep["audio_drive_s"] = round(grew, 1)
    if grew < AUDIO_MIN_FRACTION * drive:
        fails.append("the WAV grew %.1f s in the %.0f s drive" % (grew, drive))
    db, _ = wav_rms(wav, a / float(rate), b / float(rate))
    rep["audio_dbfs"] = None if db is None else round(db, 1)
    if db is None:
        fails.append("no audio in the drive")
    elif db < silence_db:
        fails.append("audio silent in the race (%.1f dBFS < %.0f)" % (db, silence_db))


# The fixed timeline: the same screens, one press each, at fixed times from
# the pad's first report. It cannot see the screens, so it waits for the
# slowest one: the intros play out (START during them could land on "Press
# START" early and shift every later press by a screen), and the gaps are a
# whole menu's fade on the slow host. Offsets are in gaps after "Press START".
PAD_STEPS = [
    (0, "start:1200"),  # "Press START" -> LOAD PROFILE / NEW PROFILE
    (1, "a:400"),  # LOAD PROFILE -> the profile list
    (2, "a:400"),  # Profile 1 -> "Load successful"
    (3, "a:400"),  # CONTINUE -> main menu
    (4, "up:600"),  # park on World Tour (the top does not wrap)
    (5, "down:NAV"),  # -> Single Event
    (6, "a:400"),  # -> SELECT MODE (RACE)
    (7, "a:400"),  # -> race options
    (8, "a:400"),  # -> SELECT CAR
    (9, "a:400"),  # -> the race loads
]

# The Proton bench's timings (docs/running.md): "Press START" about 89 s
# after launch now that presents are held to the 60 Hz vblank (the intros
# play at their own length; it was 76 s when D3D11 flipped them faster), so
# START at 100 s, then a screen every 15 s. The menu's d-pad repeat counts
# frames: unpaced D3D11 flipped about 130 a second there, where a 300 ms hold
# moved three entries (run 20261010-100737); at 60 a second 50 ms (3 frames)
# moves one (run 20261010-113724). The CPU backend's menus flip about 15 a
# second: --nav-ms 200 there.
PAD_START_MS, PAD_GAP_MS, PAD_NAV_MS, PAD_RACE_MS = 100000, 15000, 50, 30000
# From the last A to the race's first flip under Proton: about 40 s on
# D3D11, up to 150 s on the CPU backend. RT is held through it and the limit
# leaves the whole drive after it.
PAD_LOAD_SLACK_S = 180


def pad_script(
    drive_secs,
    start_ms=PAD_START_MS,
    gap_ms=PAD_GAP_MS,
    nav_ms=PAD_NAV_MS,
    race_ms=PAD_RACE_MS,
):
    """The route as a fixed RECOMP_PAD_SCRIPT, for runners without PAD_LIVE:
    START at start_ms, a press every gap_ms, RT race_ms after the last, held
    to the end of the run. Returns (script, the BENCH_TIMEOUT it needs)."""
    out = []
    for k, what in PAD_STEPS:
        out.append("%d:%s" % (start_ms + k * gap_ms, what.replace("NAV", str(nav_ms))))
    rt = start_ms + PAD_STEPS[-1][0] * gap_ms + race_ms
    limit = rt // 1000 + PAD_LOAD_SLACK_S + drive_secs
    out.append("%d:rt:%d" % (rt, (limit + 30) * 1000 - rt))
    return ",".join(out), limit


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mode", choices=sorted(MODES), default="fast")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "burnout3"))
    ap.add_argument("--cwd", default=ROOT, help="where the game finds Burnout 3 Takedown")
    ap.add_argument(
        "--fixture",
        default=os.environ.get(FIXTURE_ENV),
        help="the save to seed from (UDATA, TDATA); default $%s" % FIXTURE_ENV,
    )
    ap.add_argument("--out", default=None, help="default: the workspace's runs/ above the checkout")
    ap.add_argument("--name", default=None)
    ap.add_argument("--drive-secs", type=int, default=None)
    ap.add_argument("--timeout", type=float, default=None, help="hard limit for the whole run")
    # The Mac front end flips at several hundred a second and the toolkit
    # stops at 200 dumps, so a sparse interval keeps some for the race.
    ap.add_argument("--dump-every", type=int, default=1000, help="flips between frame dumps")
    ap.add_argument(
        "--render-scale",
        type=int,
        default=1,
        help="RECOMP_RENDER_SCALE for an A/B run (every other enhancement key stays stock);"
        " --check and --stock-env take it too",
    )
    ap.add_argument(
        "--stock-env",
        action="store_true",
        help="print the stock pins as KEY=VALUE words, for BENCH_ENV",
    )
    ap.add_argument("--check", metavar="RUNDIR")
    ap.add_argument("--min-race-fps", type=float, default=MIN_RACE_FPS)
    ap.add_argument("--pad-script", action="store_true")
    ap.add_argument(
        "--nav-ms",
        type=int,
        default=None,
        help="d-pad hold for one menu step (driven: 15, --pad-script: %d)" % PAD_NAV_MS,
    )
    ap.add_argument("--start-ms", type=int, default=PAD_START_MS, help="--pad-script: START")
    ap.add_argument("--gap-ms", type=int, default=PAD_GAP_MS, help="--pad-script: between screens")
    ap.add_argument("--race-ms", type=int, default=PAD_RACE_MS, help="--pad-script: load to RT")
    args = ap.parse_args(argv)
    if args.drive_secs is None:
        args.drive_secs = MODES[args.mode]

    if args.check:
        ok, rep = check(
            args.check,
            min_race_fps=args.min_race_fps,
            drive_secs=args.drive_secs,
            render_scale=args.render_scale,
        )
        print(json.dumps(rep, indent=2))
        return 0 if ok else 1
    if args.stock_env:
        print(stock_env_words(args.render_scale))
        return 0
    if args.pad_script:
        nav = PAD_NAV_MS if args.nav_ms is None else args.nav_ms
        script, limit = pad_script(args.drive_secs, args.start_ms, args.gap_ms, nav, args.race_ms)
        print(script)
        print("race_gate: BENCH_TIMEOUT=%d" % limit, file=sys.stderr)
        return 0
    if args.nav_ms is None:
        args.nav_ms = 15
    if args.timeout is None:
        args.timeout = 240 + args.drive_secs
    fixture = args.fixture
    if not fixture or not os.path.isdir(fixture):
        sys.exit("race_gate: no save fixture (--fixture DIR or $%s)" % FIXTURE_ENV)
    out = args.out or find_runs_dir()
    if not out:
        sys.exit("race_gate: no runs/ above %s; give --out" % ROOT)
    if inside(out, ROOT):
        sys.exit("race_gate: --out %s is inside the checkout %s" % (out, ROOT))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run = os.path.abspath(os.path.join(out, "%s-%s" % (stamp, args.name or "race-" + args.mode)))
    os.makedirs(run)
    seed_save(os.path.abspath(fixture), os.path.join(run, "save"))
    print(
        "race_gate: %s, %s mode (%d s drive), save from %s"
        % (run, args.mode, args.drive_secs, fixture)
    )
    result = drive(args, run)
    result.update({"mode": args.mode, "drive_secs": args.drive_secs, "fixture": fixture})
    ok, rep = check(run, result, min_race_fps=args.min_race_fps, render_scale=args.render_scale)
    result["report"] = rep
    with open(os.path.join(run, "gate.json"), "w") as f:
        json.dump(result, f, indent=2)
    print(json.dumps(rep, indent=2))
    print("race_gate: %s in %.0f s" % ("PASS" if ok else "FAIL", result["wall"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
