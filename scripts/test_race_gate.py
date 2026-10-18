"""Tests for race_gate.py: log following, save seeding, the route runner
against a fake game, stopping the game, the run checks, and the
fixed-timeline fallback. No game run."""

import array
import json
import os
import subprocess
import sys
import wave

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import race_gate  # noqa: E402


def write_wav(path, parts, rate=4000):
    """A stereo 16-bit WAV of (seconds, amplitude) parts."""
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        for seconds, amp in parts:
            n = int(seconds * rate)
            s = array.array("h", [amp if i % 2 else -amp for i in range(n * 2)])
            if sys.byteorder == "big":
                s.byteswap()
            w.writeframes(s.tobytes())


def test_log_follows_flips_opens_pad_and_crash(tmp_path):
    p = tmp_path / "log"
    p.write_text("[GPU] flip 1 52 ms fence 0x7 batches 0\n")
    log = race_gate.Log(str(p))
    log.poll()
    assert log.flips == 1
    with open(p, "a") as f:
        f.write("  [FILE] D:\\tracks\\_FEMain.xmv (disp 1 opts 0x48 from 0x1) -> 0x0\n")
        f.write("  [FILE] D:\\x.bin (disp 1 opts 0x6C from 0x1) -> FAILED 0xC000003A\n")
        f.write('  PAD: live "a:300" at t=6053 ms\n')
        f.write("[GPU] flip 60 900 ms fence")  # no newline yet: not parsed
    log.poll()
    assert log.flips == 1 and log.pad_live == ["a:300"]
    assert log.opened_since("_femain.XMV", 0) == 0
    assert log.opened_since("_femain.XMV", 1) is None  # before the mark
    assert log.opened_since("x.bin", 0) is None
    with open(p, "a") as f:
        f.write(" 0x9\n[FAULT] access violation\n")
    log.poll()
    assert log.flips == 60 and log.flip_ms == 900
    assert log.crash == "[FAULT] access violation"
    assert log.mark() == 1


def test_seed_save_copies_and_leaves_fixture(tmp_path):
    fx = tmp_path / "fx"
    (fx / "UDATA" / "4541005b").mkdir(parents=True)
    (fx / "TDATA" / "4541005b").mkdir(parents=True)
    (fx / "UDATA" / "4541005b" / "SaveMeta.xbx").write_bytes(b"meta")
    (fx / "README").write_text("private")
    before = sorted(str(p) for p in fx.rglob("*"))
    dest = tmp_path / "run" / "save"
    race_gate.seed_save(str(fx), str(dest))
    assert (dest / "UDATA" / "4541005b" / "SaveMeta.xbx").read_bytes() == b"meta"
    assert (dest / "TDATA" / "4541005b").is_dir()
    assert not (dest / "README").exists()
    (dest / "UDATA" / "4541005b" / "SaveMeta.xbx").write_bytes(b"changed")
    assert (fx / "UDATA" / "4541005b" / "SaveMeta.xbx").read_bytes() == b"meta"
    assert sorted(str(p) for p in fx.rglob("*")) == before


def test_runs_go_to_the_workspace_never_the_checkout(tmp_path):
    ws = tmp_path / "ws"
    repo = ws / "wt" / "x" / "b3"
    (repo / "runs").mkdir(parents=True)  # the checkout's own: never used
    assert race_gate.find_runs_dir(str(repo)) is None
    (ws / "runs").mkdir()
    assert race_gate.find_runs_dir(str(repo)) == str(ws / "runs")
    assert race_gate.inside(str(repo / "runs" / "g"), str(repo))
    assert not race_gate.inside(str(ws / "runs"), str(repo))


def test_main_needs_a_fixture(tmp_path, monkeypatch):
    monkeypatch.delenv(race_gate.FIXTURE_ENV, raising=False)
    with pytest.raises(SystemExit) as e:
        race_gate.main(["--out", str(tmp_path)])
    assert "fixture" in str(e.value)
    with pytest.raises(SystemExit) as e:
        race_gate.main(["--fixture", str(tmp_path), "--out", race_gate.ROOT])
    assert "inside the checkout" in str(e.value)


def test_wav_rms(tmp_path):
    loud = str(tmp_path / "loud.wav")
    write_wav(loud, [(2, 3277)])  # 0.1 of full scale: -20 dBFS
    db, length = race_gate.wav_rms(loud)
    assert abs(length - 2) < 1e-6 and abs(db + 20) < 0.1
    quiet = str(tmp_path / "quiet.wav")
    write_wav(quiet, [(1, 0)])
    assert race_gate.wav_rms(quiet)[0] == -200.0
    assert race_gate.wav_rms(str(tmp_path / "none.wav")) == (None, 0.0)


# The route against a fake game: a clock the runner's sleeps move, flips at
# a steady rate (or not, inside `stalls`), and files the presses open.

# The files a good game opens on each press of the route, by press number.
ANSWERS = {
    1: ["_FEMain.xmv"],
    2: ["SaveMeta.xbx"],
    3: ["Profile 1"],
    10: ["CAR1.bgv"],
    11: ["crash1.rws", "ident0.awd"],
}


class FakeGame:
    def __init__(self, answers=None, rate=100, stalls=(), lines=None, ack=True):
        self.t = 0.0
        self.rate = rate
        self.stalls = stalls  # (from s, to s): no flips
        self.flip = 0
        self.ack = ack
        self.answers = ANSWERS if answers is None else answers
        self.presses = []
        # (time, line) not yet in the log; the intros open Titles30 at 2 s.
        self.pending = [(2.0, self.open_line("Titles30.xmv"))] + list(lines or [])
        self.exit = None
        self.log = race_gate.Log(os.devnull + "-none", clock=lambda: self.t)

    @staticmethod
    def open_line(name):
        return "  [FILE] D:\\%s (disp 1 opts 0x48 from 0x1) -> 0x0" % name

    def sleep(self, s):
        t1 = self.t + s
        if not any(a <= self.t < b for a, b in self.stalls):
            self.flip += max(1, int(s * self.rate))
        out = ["[GPU] flip %d %d ms fence" % (self.flip, int(t1 * 1000))]
        due = sorted(p for p in self.pending if p[0] <= t1)
        self.pending = [p for p in self.pending if p[0] > t1]
        out += [line for _, line in due]
        self.t = t1
        self.log.feed("\n".join(out) + "\n")

    def press(self, what):
        self.presses.append(what)
        if self.ack:
            self.pending.append((self.t + 0.02, '  PAD: live "%s" at t=0 ms' % what))
        for k, name in enumerate(self.answers.get(len(self.presses), [])):
            self.pending.append((self.t + 0.1 + k, self.open_line(name)))

    def runner(self, **kw):
        return race_gate.Runner(
            self.log,
            self.press,
            clock=lambda: self.t,
            sleep=self.sleep,
            exited=lambda: self.exit,
            quiet=True,
            **kw,
        )


ROUTE_PRESSES = [
    "start:300",
    "start:300",
    "a:300",
    "a:300",
    "a:300",
    "up:600",
    "down:15",
    "a:300",
    "a:300",
    "a:300",
    "a:300",
]


def test_route_walks_to_the_race():
    g = FakeGame()
    r = g.runner()
    r.route(race_gate.ROUTE)
    assert g.presses == ROUTE_PRESSES
    seen = [e["event"] for e in r.events if e["event"].startswith("seen")]
    assert seen[0] == "seen Titles30.xmv" and seen[-1] == "seen ident0.awd"


def test_route_waits_for_the_file_and_times_out():
    g = FakeGame()
    g.pending = []  # Titles30 never opens
    with pytest.raises(race_gate.Timeout, match="intros"):
        g.runner().route(race_gate.ROUTE)
    assert g.t >= 120 and not g.presses


def test_secs_wait_counts_from_the_last_press():
    route = [
        {"name": "one", "wait": "secs:0", "press": "a:300"},
        {"name": "two", "wait": "secs:3", "press": "b:300"},
    ]
    g = FakeGame(answers={})
    r = g.runner()
    r.route(route)
    presses = [e for e in r.events if e["event"].startswith("press")]
    assert presses[1]["t"] - presses[0]["t"] >= 3


def test_settle_fails_on_stalled_flips():
    g = FakeGame(stalls=[(2.0, 1000)])
    with pytest.raises(race_gate.Failure, match="settl|stopped"):
        g.runner().route(race_gate.ROUTE)
    assert not g.presses


def test_until_retries_a_dropped_press():
    # The first START lands while Titles30 fades in: nothing opens.
    answers = dict(ANSWERS)
    answers[1], answers[2], answers[3] = [], ["_FEMain.xmv"], ["SaveMeta.xbx"]
    answers[4], answers[11], answers[12] = ["Profile 1"], ["CAR1.bgv"], ANSWERS[11]
    g = FakeGame(answers=answers)
    r = g.runner()
    r.route(race_gate.ROUTE)
    assert g.presses == ["start:300"] + ROUTE_PRESSES
    intro = [e for e in r.events if e["event"] == "seen _FEMain.xmv"]
    assert intro[0]["tries"] == 2


def test_until_gives_up_after_its_tries():
    answers = dict(ANSWERS)
    answers[1] = []
    for k in range(2, 6):
        answers[k] = []
    g = FakeGame(answers=answers)
    with pytest.raises(race_gate.Timeout, match="intros"):
        g.runner().route(race_gate.ROUTE)
    assert g.presses == ["start:300"] * 5


def test_an_open_before_the_press_does_not_answer_it():
    # _FEMain.xmv opened once before the START (in log order): only an open
    # after the press counts, so the step retries until one comes.
    answers = dict(ANSWERS)
    answers[1] = []
    answers[2] = ["_FEMain.xmv"]
    g = FakeGame(answers=answers, lines=[(1.0, FakeGame.open_line("_FEMain.xmv"))])
    r = g.runner()
    route = race_gate.ROUTE[:1]
    r.route(route)
    assert g.presses == ["start:300", "start:300"]


def test_a_later_screen_stops_the_retries():
    # The START went two screens on: a later step's file opened, not this
    # step's. Pressing again would act on the wrong screen.
    answers = dict(ANSWERS)
    answers[1] = ["Profile 1"]
    g = FakeGame(answers=answers)
    with pytest.raises(race_gate.Failure, match="Profile 1 opened before _FEMain.xmv"):
        g.runner().route(race_gate.ROUTE)
    assert g.presses == ["start:300"]


def test_route_stops_on_a_crash_line():
    g = FakeGame(lines=[(3.0, "[FAULT] access violation at 0x1234")])
    with pytest.raises(race_gate.Failure, match="crash during .*FAULT"):
        g.runner().route(race_gate.ROUTE)


def test_route_stops_when_the_game_exits():
    g = FakeGame()
    g.exit = 3
    with pytest.raises(race_gate.Failure, match="exited \\(3\\)"):
        g.runner().route(race_gate.ROUTE)


def test_a_press_the_pad_does_not_report_fails():
    g = FakeGame(ack=False)
    with pytest.raises(race_gate.Failure, match="did not report start:300"):
        g.runner().route(race_gate.ROUTE)


def test_race_drives_and_measures():
    g = FakeGame()
    sizes = iter([1000, 5000])
    r = g.runner(wav_size=lambda: next(sizes))
    r.route(race_gate.ROUTE)
    out = r.race(20)
    assert g.presses[-1] == "rt:25000"
    assert out["wav_bytes"] == [1000, 5000]
    assert out["race_end"]["t"] - out["race_start"]["t"] >= 20
    assert out["race_end"]["flip"] > out["race_start"]["flip"]
    assert out["race_end"]["ms"] > out["race_start"]["ms"]


def test_race_fails_when_the_flips_stop():
    g = FakeGame()
    r = g.runner(stall_secs=5)
    r.route(race_gate.ROUTE)
    g.stalls = [(g.t + 8, 10000)]
    with pytest.raises(race_gate.Failure, match="flips stopped .* during the drive"):
        r.race(60)
    assert g.t < g.stalls[0][0] + 6


# stop(): the signal that ended the game.


def child(ignore):
    code = (
        "import signal, sys, time\n"
        "for s in %r: signal.signal(getattr(signal, s), signal.SIG_IGN)\n"
        "print('ready', flush=True)\n"
        "time.sleep(60)\n" % (ignore,)
    )
    p = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE)
    assert p.stdout.readline().strip() == b"ready"
    return p


@pytest.mark.parametrize(
    "ignore,want",
    [((), "INT"), (("SIGINT",), "TERM"), (("SIGINT", "SIGTERM"), "KILL")],
)
def test_stop_escalates(ignore, want):
    p = child(ignore)
    try:
        assert race_gate.stop(p, grace=0.5) == want
        assert p.poll() is not None
    finally:
        if p.poll() is None:
            p.kill()
        p.stdout.close()


def test_stop_on_a_game_already_gone():
    p = subprocess.Popen([sys.executable, "-c", "pass"])
    p.wait()
    assert race_gate.stop(p) == "exited"


# check(): a driven run (gate.json) and a bench run (log and exit-code).

RATE = 4000  # the test WAVs' sample rate; 4 bytes a frame

# The layer's startup lines of a stock run (as the toolkit prints them).
STOCK_ENHANCE = [
    "[ENHANCE] config: none (RECOMP_CONFIG)",
    "[ENHANCE] render.scale=1 present.filter=nearest present.fullscreen=0"
    " present.pacing=spin (display.aspect=4:3)",
    "[ENHANCE] textures.pack=none",
]


def make_run(
    tmp_path,
    race_amp=3277,
    menu_amp=3277,
    crash=False,
    race_flip=1000,
    frames=(600, 1200),
    stop="INT",
    gap=None,
    wav_grew=20,
    enhance=STOCK_ENHANCE,
):
    """A driven run: flips 1..3000 at 10 ms each (100 flips/s), the race
    from flip 1000 (10 s) to 3000 (30 s), a 20 s drive. The WAV has 10 s of
    menu audio and then the drive's."""
    run = tmp_path / "run"
    (run / "frames").mkdir(parents=True)
    lines, ms = list(enhance), 0
    for n in range(1, 3001):
        ms += 10
        if gap and n == gap[0]:
            ms += gap[1]
        lines.append("[GPU] flip %d %d ms fence" % (n, ms))
        if n in frames:
            # The dump files are counted, not named by flip: the log says which.
            k = frames.index(n)
            (run / "frames" / ("f%03d.bmp" % k)).write_bytes(b"")
            lines.append("  [GPU] flip %d: frame dump" % n)
            lines.append("  [GPU] framebuffer dump: %s/frames/f%03d.bmp (640x464)" % (run, k))
    if crash:
        lines.append("[FAULT] boom")
    (run / "game-stdio.log").write_text("\n".join(lines) + "\n")
    write_wav(str(run / "audio.wav"), [(10, menu_amp), (20, race_amp)], RATE)
    start = race_gate.WAV_HEADER + 10 * RATE * 4
    result = {
        "wall": 35.0,
        "drive_secs": 20,
        "stop": stop,
        "race_start": {"t": 10.0, "flip": race_flip, "ms": race_flip * 10},
        "race_end": {"t": 30.0, "flip": 3000, "ms": 30000},
        "wav_bytes": [start, start + int(wav_grew * RATE * 4)],
        "crash": "[FAULT] boom" if crash else None,
    }
    return str(run), result


def test_check_passes_a_good_run(tmp_path):
    run, result = make_run(tmp_path)
    ok, rep = race_gate.check(run, result)
    assert ok, rep["failures"]
    assert rep["race_frames"] == 1 and rep["stop"] == "INT"
    assert rep["flips"] == 3000 and rep["race_flips"] == 2000
    assert rep["race_flips_per_s"] == 100.0 and rep["race_secs"] == 20.0
    assert rep["audio_drive_s"] == 20.0 and abs(rep["audio_dbfs"] + 20) < 0.1


def test_check_fails_silence_crash_and_no_race_frame(tmp_path):
    run, result = make_run(tmp_path, race_amp=0, crash=True, frames=(600,))
    ok, rep = race_gate.check(run, result)
    assert not ok
    text = " ".join(rep["failures"])
    assert "silent" in text and "crash" in text and "no frame dumped" in text


def test_check_measures_the_drive_not_the_menu(tmp_path):
    # Loud menu, silent race: the tail-of-file measure passed this.
    run, result = make_run(tmp_path, race_amp=0, menu_amp=8000)
    ok, rep = race_gate.check(run, result)
    assert not ok and "audio silent in the race" in " ".join(rep["failures"])


def test_check_fails_a_wav_that_did_not_grow(tmp_path):
    run, result = make_run(tmp_path, wav_grew=5)
    ok, rep = race_gate.check(run, result)
    assert not ok
    assert "the WAV grew 5.0 s in the 20 s drive" in rep["failures"]


def test_check_needs_the_wav_sizes(tmp_path):
    run, result = make_run(tmp_path)
    del result["wav_bytes"]
    ok, rep = race_gate.check(run, result)
    assert not ok and "no WAV sizes for the drive" in rep["failures"]


@pytest.mark.parametrize("sig", ["TERM", "KILL"])
def test_check_fails_a_game_that_ignored_sigint(tmp_path, sig):
    run, result = make_run(tmp_path, stop=sig)
    ok, rep = race_gate.check(run, result)
    assert not ok and "needed SIG%s" % sig in " ".join(rep["failures"])


def test_check_fails_a_slow_race(tmp_path):
    run, result = make_run(tmp_path)
    ok, rep = race_gate.check(run, result, min_race_fps=150)
    assert not ok and "100.0 flips/s (floor 150)" in " ".join(rep["failures"])


def test_check_fails_a_freeze_in_the_race(tmp_path):
    run, result = make_run(tmp_path, gap=(2000, 6000))
    result["race_end"]["ms"] += 6000
    ok, rep = race_gate.check(run, result)
    assert not ok and "flips stopped at 1999 for 6.0 s" in " ".join(rep["failures"])


def test_check_fails_a_short_race(tmp_path):
    run, result = make_run(tmp_path)
    result["drive_secs"] = 60
    ok, rep = race_gate.check(run, result)
    assert not ok and "ran 20 s of the 60 s drive" in " ".join(rep["failures"])


def test_check_reads_gate_json(tmp_path):
    run, result = make_run(tmp_path)
    with open(os.path.join(run, "gate.json"), "w") as f:
        json.dump(result, f)
    assert race_gate.check(run)[0]


def test_route_is_well_formed():
    keys = {"name", "wait", "settle", "press", "until", "retry", "tries", "timeout"}
    assert race_gate.ROUTE[-1]["name"] == "race start"
    for st in race_gate.ROUTE:
        assert set(st) <= keys, st
        kind = st["wait"].split(":", 1)[0]
        assert kind in ("file", "secs"), st
        assert st.get("timeout", 60) > 0
        if "until" in st:
            assert st.get("press") and st.get("tries", 1) >= 1, st


def test_pad_script_timeline():
    text, limit = race_gate.pad_script(60)
    steps = [e.split(":") for e in text.split(",")]
    times = [int(e[0]) for e in steps]
    assert times == sorted(times) and times[0] == 100000
    assert ["175000", "down", "50"] in steps
    assert steps[-1][:2] == ["265000", "rt"]
    # RT is held past the limit the run needs: the whole drive after the
    # load's slack.
    assert limit == 265 + race_gate.PAD_LOAD_SLACK_S + 60
    assert 265000 + int(steps[-1][2]) > limit * 1000
    assert text.count(":a:400") == 7 and text.count(":start:1200") == 1


def make_bench_run(tmp_path, rc="124", frames=(600, 2400), d3d11=False):
    run, _ = make_run(tmp_path, frames=() if d3d11 else frames)
    os.remove(os.path.join(run, "audio.wav"))
    log = os.path.join(run, "game-stdio.log")
    lines = open(log).read().splitlines()
    if d3d11:
        for n in frames:
            i = lines.index("[GPU] flip %d %d ms fence" % (n, n * 10))
            lines.insert(i + 1, "[d3d11] dumped Z:/x/frames/frame_%04d.bmp" % n)
    i = lines.index("[GPU] flip 2000 20000 ms fence")
    lines[i:i] = [
        "  [FILE] D:\\sound\\ident0.awd (disp 1 opts 0x6C from 0x1) -> 0x0",  # boot: not yet
        "  [FILE] D:\\tracks\\crash1.rws (disp 1 opts 0x6C from 0x1) -> 0x0",
        "  [FILE] D:\\sound\\ident0.awd (disp 1 opts 0x6C from 0x1) -> 0x0",
    ]
    open(log, "w").write("\n".join(lines) + "\n")
    if rc is not None:
        open(os.path.join(run, "exit-code"), "w").write(rc + "\n")
    return run


@pytest.mark.parametrize("d3d11", [False, True])
def test_check_reads_a_bench_run_from_its_log(tmp_path, d3d11):
    run = make_bench_run(tmp_path, d3d11=d3d11)
    ok, rep = race_gate.check(run, drive_secs=10)
    assert ok, rep["failures"]
    assert rep["race_frames"] == 1 and "audio_dbfs" not in rep
    assert rep["exit_code"] == "124"
    assert rep["race_flips_per_s"] == 100.0  # flips 1999..3000 over 10 game s


def test_check_bench_run_must_cover_the_drive(tmp_path):
    run = make_bench_run(tmp_path)
    ok, rep = race_gate.check(run, drive_secs=60)
    assert not ok and "ran 10 s of the 60 s drive" in " ".join(rep["failures"])


@pytest.mark.parametrize(
    "rc,want",
    [(None, "no exit-code"), ("137", "killed"), ("3", "exit 3"), ("0", None)],
)
def test_check_bench_exit_code(tmp_path, rc, want):
    run = make_bench_run(tmp_path, rc=rc)
    ok, rep = race_gate.check(run)
    if want is None:
        assert ok, rep["failures"]
    else:
        assert not ok and want in " ".join(rep["failures"])


def test_check_bench_run_without_flip_lines(tmp_path):
    run = make_bench_run(tmp_path)
    log = os.path.join(run, "game-stdio.log")
    lines = [x for x in open(log).read().splitlines() if not x.startswith("[GPU] flip")]
    open(log, "w").write("\n".join(lines) + "\n")
    ok, rep = race_gate.check(run)
    assert not ok and "RECOMP_TRACE=flip" in " ".join(rep["failures"])


def test_stock_env_reads_no_config_and_varies_only_the_scale():
    env = race_gate.stock_env()
    assert env["RECOMP_CONFIG"] == "none"
    assert env["RECOMP_RENDER_SCALE"] == "1"
    assert env["RECOMP_PRESENT_PACING"] == "spin"
    two = race_gate.stock_env(2)
    assert two["RECOMP_RENDER_SCALE"] == "2"
    assert {k: v for k, v in two.items() if k != "RECOMP_RENDER_SCALE"} == {
        k: v for k, v in env.items() if k != "RECOMP_RENDER_SCALE"
    }


def test_check_reports_a_stock_run(tmp_path):
    run, result = make_run(tmp_path)
    ok, rep = race_gate.check(run, result)
    assert ok, rep["failures"]
    assert rep["enhance_config"] == "none (RECOMP_CONFIG)"
    assert rep["enhance"] == {
        "render.scale": 1,
        "render.aniso": 0,
        "present.filter": "nearest",
        "present.fullscreen": 0,
        "present.pacing": "spin",
    }


def test_check_a_scale_2_run_passes_only_at_scale_2(tmp_path):
    lines = [x.replace("render.scale=1", "render.scale=2") for x in STOCK_ENHANCE]
    run, result = make_run(tmp_path, enhance=lines)
    ok, rep = race_gate.check(run, result, render_scale=2)
    assert ok, rep["failures"]
    assert rep["enhance"]["render.scale"] == 2
    ok, rep = race_gate.check(run, result)
    assert not ok and "render.scale=2, want 1" in rep["failures"]


def test_check_reads_render_aniso(tmp_path):
    # The toolkit that adds render.aniso prints it after the scale, folded
    # (1 is off and logs 0): 0 passes, anything else is not stock.
    for value, ok_want in (("0", True), ("16", False)):
        lines = [
            x.replace("render.scale=1 ", "render.scale=1 render.aniso=%s " % value)
            for x in STOCK_ENHANCE
        ]
        run, result = make_run(tmp_path / value, enhance=lines)
        ok, rep = race_gate.check(run, result)
        assert ok == ok_want, rep["failures"]
        assert rep["enhance"]["render.aniso"] == int(value)
        if not ok:
            assert "render.aniso=16, want 0" in rep["failures"]


def test_check_fails_a_run_that_read_a_config_file(tmp_path):
    lines = [
        "[ENHANCE] config: renamed ./config/enhance.toml to config.toml (one-time migration)",
        "[ENHANCE] config: /x/config.default.toml (5 keys), ./config/config.toml (1 keys)",
    ] + STOCK_ENHANCE[1:]
    run, result = make_run(tmp_path, enhance=lines)
    ok, rep = race_gate.check(run, result)
    assert not ok
    assert rep["enhance_config"].startswith("/x/config.default.toml")
    assert "the run read settings files" in " ".join(rep["failures"])


def test_check_fails_non_stock_pacing_and_a_build_without_the_layer(tmp_path):
    lines = [x.replace("pacing=spin", "pacing=sleep") for x in STOCK_ENHANCE]
    run, result = make_run(tmp_path, enhance=lines)
    ok, rep = race_gate.check(run, result)
    assert not ok and "present.pacing=sleep, want spin" in rep["failures"]
    run, result = make_run(tmp_path / "bare", enhance=[])
    ok, rep = race_gate.check(run, result)
    assert not ok and "enhancements layer" in " ".join(rep["failures"])


def test_stock_env_pins_all_ten_toolkit_keys():
    assert set(race_gate.STOCK_ENV) == {
        "RECOMP_CONFIG",
        "RECOMP_RENDER_SCALE",
        "RECOMP_RENDER_ANISO",
        "RECOMP_DISPLAY_ASPECT",
        "RECOMP_PRESENT_FILTER",
        "RECOMP_PRESENT_FULLSCREEN",
        "RECOMP_PRESENT_PACING",
        "RECOMP_TEXTURES_PACK",
        "RECOMP_TEXTURES_FONT",
        "RECOMP_TEXTURES_TEXT",
    }


def test_stock_env_words(capsys):
    assert race_gate.main(["--stock-env", "--render-scale", "2"]) == 0
    words = capsys.readouterr().out.split()
    assert dict(w.split("=", 1) for w in words) == race_gate.stock_env(2)


class FakeProc:
    """A game that has already exited: the route stops at once."""

    returncode = 3

    def poll(self):
        return 3


@pytest.mark.parametrize("argv,scale", [([], "1"), (["--render-scale", "2"], "2")])
def test_drive_pins_stock_over_the_shell(tmp_path, monkeypatch, argv, scale):
    monkeypatch.setenv("RECOMP_CONFIG", "/x/config.toml")
    monkeypatch.setenv("RECOMP_RENDER_SCALE", "3")
    seen = {}

    def popen(cmd, env=None, **kw):
        seen.update(env)
        return FakeProc()

    monkeypatch.setattr(race_gate.subprocess, "Popen", popen)
    fx = tmp_path / "fx"
    (fx / "UDATA").mkdir(parents=True)
    (fx / "TDATA").mkdir()
    out = tmp_path / "out"
    out.mkdir()
    args = ["--fixture", str(fx), "--out", str(out), "--exe", "/nonexistent/burnout3"]
    assert race_gate.main(args + argv) == 1
    (run,) = out.iterdir()
    words = dict(w.split("=", 1) for w in (run / "env.txt").read_text().split("\n")[0].split())
    assert words["RECOMP_CONFIG"] == seen["RECOMP_CONFIG"] == "none"
    assert words["RECOMP_RENDER_SCALE"] == seen["RECOMP_RENDER_SCALE"] == scale
    assert words["RECOMP_PRESENT_PACING"] == "spin"
