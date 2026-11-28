#!/usr/bin/env python3
"""Tests for .gitignore: the dump, generated code, the bundles and the
game-data extensions stay out of git, and the manifest and these tests stay
in. Each sample is one baseline line of xboxrecomp-cli's docs/layout.md
("The .gitignore baseline"), filled with this game's own folders from
game.toml, so a missing line would put game data one `git add` away.
Written by `xbr new`; the CLI's own tests keep SAMPLES equal to the
baseline.

  python3 scripts/test_gitignore.py
  python3 -m pytest scripts/test_gitignore.py

Exit 0 when every test passes. Standard library only (Python 3.9: no
tomllib), so it runs before any setup.
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DATA_EXT = (
    "xbe iso xiso bmp png wav sav pack xpr xwb adx sfd ipk xsv xmv rws awd ppm raw "
    "dmg exe pdb nsi tar ico icns xtd"
).split()

# The game.toml values the samples are built from, with the CLI's defaults.
KEYS = {
    ("data", "game_files"): "game_files",
    ("pipeline", "gen"): "src/recomp/gen",
    ("pipeline", "out"): "analysis",
    ("build", "windows_dir"): "build-win",
    ("build", "macos_dir"): "build",
    ("build", "linux_dir"): "build-linux",
}

SAMPLES = (
    "@game_files@/default.xbe",
    # Bare: in a worktree the dump, the clones and the venv are symlinks,
    # and a `dir/` line matches a directory only.
    "@game_files@",
    "@game_files@.partial/x",
    "@game_files@.old/x",
    "@gen@/recomp_0000.c",
    "@gen_parent@/gen.key.json",
    "@out@/x",
    "@windows_dir@/x",
    "@macos_dir@/x",
    "@linux_dir@/x",
    "build-x/x",
    "build-logs/x",
    ".venv/x",
    ".venv",
    ".venv-ghidra",
    "third_party/x",
    "third_party",
    "external/x",
    "external",
    "dist/x",
    "bench-logs/x",
    "bench-logs",
    "wt/x/game.toml",
    "_local/x",
    "runs/x",
    "saves/x",
    "game.toml.bak",
    "game.toml.bak-20261016",
    "a/hud-pack.toml",
    "a/textures-pack.toml",
    "config/enhance-textures.local.toml",
    "a/CACHE/x",
    ".claude/settings.local.json",
    "CLAUDE.local.md",
) + tuple("a/x." + e for e in DATA_EXT)

# Never ignored: the manifest, these tests, the game's tools, the README
# screenshots, and two paths an unanchored top-level line would catch.
TRACKED = (
    "game.toml",
    "scripts/test_gitignore.py",
    "scripts/x.py",
    "docs/images/x.png",
    "openspec/changes/build-x/x",
    "src/external/x",
)


def manifest_values(text):
    """KEYS from game.toml's plain `key = "value"` lines."""
    out, table = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            table = line.strip("[]").strip()
            continue
        m = re.match(r'^([A-Za-z_]+)\s*=\s*"([^"\\]*)"', line)
        if m and (table, m.group(1)) in KEYS:
            out[(table, m.group(1))] = m.group(2)
    return {k[1]: out.get(k, v) for k, v in KEYS.items()}


def samples(values):
    gen = values["gen"]
    v = dict(values, gen_parent=gen.rsplit("/", 1)[0] if "/" in gen else ".")
    out = []
    for s in SAMPLES:
        for k, x in v.items():
            s = s.replace("@%s@" % k, x)
        out.append(s[2:] if s.startswith("./") else s)
    return out


def ignored(paths):
    """The paths .gitignore ignores, judged in an empty repo: the answer
    comes from the rules alone (a tracked, absent or symlinked path is
    judged the same) and no global excludes file counts."""
    with open(os.path.join(ROOT, ".gitignore"), encoding="utf-8") as f:
        text = f.read()
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "init", "-q", d], check=True, capture_output=True)
        with open(os.path.join(d, ".gitignore"), "w", encoding="utf-8") as f:
            f.write(text)
        r = subprocess.run(
            [
                "git",
                "-C",
                d,
                "-c",
                "core.excludesFile=",
                "check-ignore",
                "--no-index",
                "-z",
                "--stdin",
            ],
            input="\0".join(paths).encode() + b"\0",
            capture_output=True,
        )
        assert r.returncode in (0, 1), r.stderr
        return {p for p in r.stdout.decode("utf-8").split("\0") if p}


def game_samples():
    with open(os.path.join(ROOT, "game.toml"), encoding="utf-8") as f:
        return samples(manifest_values(f.read()))


def test_the_baseline_is_ignored():
    want = game_samples()
    missing = [p for p in want if p not in ignored(want)]
    assert not missing, "not ignored: %s" % ", ".join(missing)


def test_the_tracked_files_are_not():
    hit = sorted(ignored(list(TRACKED)))
    assert not hit, "ignored: %s" % ", ".join(hit)


# --- this game ---
# Burnout 3's own lines, below the baseline's marker in .gitignore.


def test_upstreams_screenshots_are_tracked():
    hit = sorted(ignored(["docs/screenshots/x.png"]))
    assert not hit, "ignored: %s" % ", ".join(hit)


def test_upstreams_build_outputs_and_the_own_config_are_ignored():
    want = [
        "build-xr/x",
        "out/x",
        "bin/x",
        "a/x.obj",
        "a/x.dll",
        "config/config.toml",
        "src/game/recomp/gen.1/x",
        "CLAUDE.md",
        ".claude/x",
    ]
    missing = [p for p in want if p not in ignored(want)]
    assert not missing, "not ignored: %s" % ", ".join(missing)


if __name__ == "__main__":
    fails = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("ok  ", name)
            except AssertionError as e:
                fails += 1
                print("FAIL", name, e)
    sys.exit(1 if fails else 0)
