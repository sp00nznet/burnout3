"""scripts/audit-public.sh on throwaway repositories shaped like the fork:
upstream's history (origin/main, by sp00nz), the fork's published main
(public/main) and our commits on top. A clean range passes; one planted
hit per rule fails it. The planted strings are built from pieces so this
file does not trip the audit of this repository.

  uv run pytest scripts/test_audit_public.py
"""

import os
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "audit-public.sh")
NOREPLY = "4037632+apexJCL@users.noreply.github.com"
UPSTREAM = "sp00nz@example.invalid"
# Stand-ins for the private patterns (a host name, a personal address).
HOSTNAME = "bench" + "box-7"
EMAIL = "some" + "one.private@"


def png(*extra):
    """A 1x1 PNG, with EXTRA (type, data) chunks before its IDAT."""

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    body = b"".join(chunk(t, d) for t, d in extra)
    idat = chunk(b"IDAT", zlib.compress(b"\0\0"))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + body + idat + chunk(b"IEND", b"")


pytestmark = pytest.mark.skipif(
    sys.platform == "win32" or not shutil.which("bash") or not shutil.which("git"),
    reason="needs bash and git",
)


def git(repo, *args, email=NOREPLY, name="carlos"):
    return subprocess.run(
        ["git", "-c", "user.name=" + name, "-c", "user.email=" + email, *args],
        cwd=repo,
        capture_output=True,
        text=True,
        check=True,
    ).stdout


def commit(r, path, data, msg="change", email=NOREPLY, name="carlos", force=False):
    p = os.path.join(r, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())
    git(r, "add", *(["-f"] if force else []), "--", path)
    git(r, "commit", "-qm", msg, email=email, name=name)


def repo(tmp_path):
    """Upstream's commit (origin/main, also the fork's public/main) with a
    screenshot, the private patterns file, and nothing of ours yet."""
    r = str(tmp_path / "r")
    os.makedirs(r)
    git(r, "init", "-q", "-b", "main")
    commit(r, "README.md", "# burnout3\n", "upstream", UPSTREAM, "sp00nz")
    commit(r, "docs/screenshots/race.png", b"\x89PNG\0\1", "shots", UPSTREAM, "sp00nz")
    commit(r, "docs/screenshots/hero.gif", b"GIF89a\0\1", "hero", UPSTREAM, "sp00nz")
    git(r, "update-ref", "refs/remotes/origin/main", "HEAD")
    git(r, "update-ref", "refs/remotes/public/main", "HEAD")
    with open(os.path.join(r, ".git", "info", "audit-private"), "w") as f:
        f.write("# private\n%s\n%s\n" % (HOSTNAME, EMAIL.replace(".", "\\.")))
    return r


def audit(r, *args, env=None):
    e = dict(os.environ)
    e.pop("XBR_AUDIT_PRIVATE", None)
    e.update(env or {})
    return subprocess.run(["bash", SCRIPT, *args], cwd=r, capture_output=True, text=True, env=e)


def test_clean(tmp_path):
    r = repo(tmp_path)
    commit(r, "src/a.c", "int x;\n")
    commit(r, "docs/images/title-1x.png", png(), "readme images")
    p = audit(r)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "clean" in p.stdout
    # --all reads upstream's tree and messages too, but identities only of
    # origin/main..main.
    p = audit(r, "--all")
    assert p.returncode == 0, p.stdout + p.stderr


def test_upstream_commit_by_sp00nz_accepted(tmp_path):
    """Upstream moved and was merged into main: its commits are new to the
    fork (public/main..main) but below origin/main, and keep sp00nz's
    identity."""
    r = repo(tmp_path)
    commit(r, "src/a.c", "int x;\n")
    git(r, "checkout", "-q", "-b", "up", "origin/main")
    commit(r, "src/up.c", "int up;\n", "upstream fix", UPSTREAM, "sp00nz")
    git(r, "update-ref", "refs/remotes/origin/main", "HEAD")
    git(r, "checkout", "-q", "main")
    git(r, "merge", "-q", "--no-ff", "-m", "merge upstream", "up")
    log = git(r, "log", "--format=%ae", "public/main..main")
    assert UPSTREAM in log
    p = audit(r)
    assert p.returncode == 0, p.stdout + p.stderr


@pytest.mark.parametrize(
    "path, data, msg, email, force, rule",
    [
        ("a.py", "p = '/Us" + "ers/someone/x'\n", "c", NOREPLY, False, "text"),
        ("docs/FRAME.BMP", "BM", "c", NOREPLY, False, "game data"),
        ("data/intro.xmv", "x", "c", NOREPLY, False, "game data"),
        ("data/track.Rws", "x", "c", NOREPLY, False, "game data"),
        ("docs/fr\u00e1me.bmp", "BM", "c", NOREPLY, False, "game data"),
        ("docs/screenshots/new.png", b"\x89PNG\0\3", "c", NOREPLY, False, "game data"),
        ("docs/screenshots/race.png", b"\x89PNG\0\4", "c", NOREPLY, False, "game data"),
        ("docs/images/a.png", png((b"tEXt", b"Comment\0x")), "c", NOREPLY, False, "image chunks"),
        ("docs/images/b.png", png((b"eXIf", b"MM\0*")), "c", NOREPLY, False, "image chunks"),
        ("docs/images/c.png", b"not a png", "c", NOREPLY, False, "image chunks"),
        ("a.py", "p = '/ho" + "me/someone/x'\n", "c", NOREPLY, False, "text"),
        ("a.py", "p = '/var/ho" + "me/someone'\n", "c", NOREPLY, False, "text"),
        ("a.md", "see C:\\Us" + "ers\\someone\\x\n", "c", NOREPLY, False, "text"),
        ("a.md", "see https://clau" + "de.ai/chat/x\n", "c", NOREPLY, False, "text"),
        ("a.md", "the %s host\n" % HOSTNAME.upper(), "c", NOREPLY, False, "text"),
        ("a.md", "mail %sexample.invalid\n" % EMAIL, "c", NOREPLY, False, "text"),
        ("a.txt", "ok\n", "from /Us" + "ers/me", NOREPLY, False, "message"),
        ("docs/frame.bmp", "BM", "c", NOREPLY, False, "game data"),
        ("docs/shots/x.png", "png", "c", NOREPLY, False, "game data"),
        ("docs/images/clip.gif", b"GIF89a\0\1", "c", NOREPLY, False, "binary"),
        ("blob.bin", b"\0\1\2\0" * 64, "c", NOREPLY, False, "binary"),
        ("a.txt", "ok\n", "c", "someone@example.invalid", False, "identity"),
        ("TESTING.md", "local\n", "c", NOREPLY, True, "never tracked"),
        ("runs/x/run.txt", "x\n", "c", NOREPLY, True, "never tracked"),
        ("Burnout 3 Takedown/Data/x.txt", "x\n", "c", NOREPLY, True, "never tracked"),
        ("src/game/recomp/gen/recomp_0000.c", "x\n", "c", NOREPLY, True, "never tracked"),
        ("build-win/x.txt", "x\n", "c", NOREPLY, True, "never tracked"),
    ],
)
def test_each_rule_hits(tmp_path, path, data, msg, email, force, rule):
    r = repo(tmp_path)
    commit(r, path, data, msg, email, force=force)
    p = audit(r)
    assert p.returncode == 1, (rule, p.stdout, p.stderr)
    assert "HIT  " + rule in p.stdout, (rule, p.stdout)
    assert audit(r, "--all").returncode == 1, rule


def test_wrong_name_hits(tmp_path):
    r = repo(tmp_path)
    commit(r, "a.txt", "ok\n", "c", NOREPLY, name="someone")
    p = audit(r)
    assert p.returncode == 1 and "HIT  identity" in p.stdout, p.stdout


def test_text_hit_names_file_and_line(tmp_path):
    r = repo(tmp_path)
    commit(r, "src/a.py", "x = 1\ny = 2\np = '/Us" + "ers/someone/x'\n")
    p = audit(r)
    assert p.returncode == 1 and "src/a.py:3: p =" in p.stdout, p.stdout


@pytest.mark.parametrize(
    "path, data, force, rule",
    [
        ("a.py", "p = '/Us" + "ers/someone/x'\n", False, "text"),
        ("blob.bin", b"\0\1\2\0" * 64, False, "binary"),
        ("docs/frame.bmp", "BM", False, "game data"),
        ("runs/x/run.txt", "x\n", True, "never tracked"),
    ],
)
def test_added_then_removed_in_range_hits(tmp_path, path, data, force, rule):
    """A push publishes every commit of the range, so a file or a line that
    a later commit of the same range removes is still out."""
    r = repo(tmp_path)
    commit(r, path, data, force=force)
    git(r, "rm", "-q", "--", path)
    git(r, "commit", "-qm", "remove it")
    commit(r, "src/a.c", "int x;\n")
    p = audit(r)
    assert p.returncode == 1, (rule, p.stdout, p.stderr)
    assert "HIT  " + rule in p.stdout, (rule, p.stdout)


def test_ident_range_follows_the_range(tmp_path):
    """A range whose tip is not main: its identities are checked, not
    main's."""
    r = repo(tmp_path)
    commit(r, "src/a.c", "int x;\n")
    git(r, "checkout", "-q", "-b", "side")
    commit(r, "src/b.c", "int y;\n", "c", "someone@example.invalid")
    git(r, "checkout", "-q", "main")
    assert audit(r).returncode == 0
    p = audit(r, "public/main..side")
    assert p.returncode == 1 and "HIT  identity" in p.stdout, p.stdout


def test_upstream_screenshot_changed_and_restored_hits(tmp_path):
    """Our version of an upstream screenshot, put back before the tip, was
    still published by the range."""
    r = repo(tmp_path)
    commit(r, "docs/screenshots/race.png", b"\x89PNG\0\x09", "ours")
    git(r, "checkout", "-q", "origin/main", "--", "docs/screenshots/race.png")
    git(r, "commit", "-qm", "upstream's again")
    p = audit(r)
    assert p.returncode == 1 and "docs/screenshots/race.png" in p.stdout, p.stdout


def test_wrong_identity_hits_with_all_too(tmp_path):
    """--all widens the text checks, never the identity range."""
    r = repo(tmp_path)
    commit(r, "a.txt", "ok\n", "c", "someone@example.invalid")
    p = audit(r, "--all")
    assert p.returncode == 1 and "HIT  identity" in p.stdout, p.stdout
    assert UPSTREAM not in p.stdout


def test_refuses_without_private_patterns(tmp_path):
    r = repo(tmp_path)
    os.remove(os.path.join(r, ".git", "info", "audit-private"))
    p = audit(r)
    assert p.returncode == 2 and "no private patterns" in p.stderr, p.stderr
    other = str(tmp_path / "patterns")
    with open(other, "w") as f:
        f.write(HOSTNAME + "\n")
    assert audit(r, env={"XBR_AUDIT_PRIVATE": other}).returncode == 0


def test_needs_public_and_origin(tmp_path):
    r = repo(tmp_path)
    git(r, "update-ref", "-d", "refs/remotes/public/main")
    p = audit(r)
    assert p.returncode == 2 and "use --all" in p.stderr, p.stderr
    assert audit(r, "--all").returncode == 0
    git(r, "update-ref", "-d", "refs/remotes/origin/main")
    p = audit(r, "--all")
    assert p.returncode == 2 and "origin/main" in p.stderr, p.stderr
