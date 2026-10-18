#!/usr/bin/env python3
"""The game's command: finds uv and the xboxrecomp-cli release (tag) or
commit game.toml pins, then runs it. A copy of xboxrecomp-cli's
wrapper/game.py, named after the game (`<this> wrapper --check` compares
the two); standard library only, Python 3.9 or newer, so a host with only
the system Python still gets the help and the uv hint. Docs:
docs/packaging.md."""

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SLUG = os.path.splitext(os.path.basename(__file__))[0]
UV_MIN = (0, 5, 31)  # xboxrecomp-cli's env.UV_MIN
UV_HINT = {
    "darwin": "brew install uv",
    "linux": "curl -LsSf https://astral.sh/uv/install.sh | sh   (or your distribution's uv package)",
    "win32": "winget install --id=astral-sh.uv -e",
}
UV_DOCS = "https://docs.astral.sh/uv/getting-started/installation/"
MARK = ".xbr-pin"
# xboxrecomp-cli's gitpin.TAG_RE and tag_ok(): nothing git would read as an
# option or a revision expression (v1^ would run v1's parent).
TAG_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")


def tag_ok(tag):
    return (
        bool(TAG_RE.match(tag))
        and ".." not in tag
        and "//" not in tag
        and "/." not in tag
        and not tag.endswith(("/", ".", ".lock"))
    )


def check_pin(cli):
    """The checks the CLI's manifest makes on [cli], before git sees any of
    it: a plain tag name, and a url git cannot read as an option."""
    if cli.get("tag") and not tag_ok(cli["tag"]):
        raise NoCli("game.toml [cli] tag %r is not a plain tag name" % cli["tag"])
    if cli.get("url", "").startswith("-"):
        raise NoCli("game.toml [cli] url %r starts with '-'" % cli["url"])


def cli_table():
    """[cli] tag, commit and url; game.toml keeps them plain one-line strings."""
    vals, cur = {}, None
    with open(os.path.join(ROOT, "game.toml"), encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("["):
                cur = line.strip("[]").strip()
            elif cur == "cli":
                m = re.match(r'^(tag|commit|url)\s*=\s*"([^"\\]*)"\s*(#.*)?$', line)
                if m:
                    vals[m.group(1)] = m.group(2)
    return vals


def find_uv():
    uv = shutil.which("uv")
    if not uv:
        return None
    out = subprocess.run([uv, "--version"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    m = re.match(r"uv (\d+)\.(\d+)\.(\d+)", out.stdout.decode().strip())
    return uv if m and tuple(int(x) for x in m.groups()) >= UV_MIN else None


GET_CLI = "clone it beside this checkout, or set XBOXRECOMP_CLI_DIR"


class NoCli(Exception):
    """Why no CLI can run; main() prints it with the way to get one."""


def git(*args):
    """git's stdout, or NoCli with its message (no network, no such
    commit on the remote, no git)."""
    try:
        r = subprocess.run(["git"] + list(args), capture_output=True, check=True)
    except OSError as e:
        raise NoCli("git: %s" % e) from None
    except subprocess.CalledProcessError as e:
        why = (e.stderr or b"").decode(errors="replace").strip().splitlines()
        raise NoCli(
            "git %s failed%s"
            % (args[0] if args[0] != "-C" else args[2], ": " + why[-1] if why else "")
        ) from None
    return r.stdout.decode(errors="replace").strip()


def head(d):
    try:
        return git("-C", d, "rev-parse", "HEAD")
    except NoCli:
        return ""


def describe(cli):
    """'v0.2.0 (abcdef012345)', the tag alone, or the short commit."""
    tag, commit = cli.get("tag", ""), cli.get("commit", "")
    return "%s (%s)" % (tag, commit[:12]) if tag and commit else tag or commit[:12]


def local_tag(d, tag):
    """The commit tag names in d (^{commit} peels an annotated tag), or ''."""
    try:
        return git("-C", d, "rev-parse", "--verify", "--quiet", "refs/tags/%s^{commit}" % tag)
    except NoCli:
        return ""


def resolve(d, cli, fresh=False):
    """The commit the pin names, for clone d: the commit alone, else the
    tag from d's tags, else that one tag fetched from origin (never forced:
    a tag moved on the remote does not replace the local one). With both,
    the tag must be the commit: a moved tag is refused, never run. fresh:
    d was just cloned, so its tags are the remote's."""
    tag, lock = cli.get("tag", ""), cli.get("commit", "")
    if not tag:
        return lock
    remote = "on " + (cli.get("url") or "origin")
    c, where = local_tag(d, tag), remote if fresh else "in " + d
    if not c:
        ref = "refs/tags/" + tag
        git("-C", d, "fetch", "--quiet", "--no-tags", "origin", ref + ":" + ref)
        c, where = local_tag(d, tag), remote
    if not c:
        raise NoCli("game.toml [cli] tag %s is not %s" % (tag, where))
    if lock and c != lock:
        hint = ""
        if where != remote:
            hint = "; if the remote's tag is right, run 'git -C %s tag -d %s' and rerun" % (d, tag)
        raise NoCli(
            "game.toml [cli] tag %s is %s %s, not the pinned commit %s: the tag moved, or "
            "game.toml names the wrong pair%s" % (tag, c[:12], where, lock[:12], hint)
        )
    return c


def at_pin(d, cli):
    """external/xboxrecomp-cli is this script's: it runs only at the pin. A
    clone it made (MARK) elsewhere is moved to the pin (the game moved it);
    any other checkout there is refused, never run unpinned. At the lock,
    or at a tag-only pin's local tag, nothing needs the network."""
    h = head(d)
    want = cli.get("commit") or (local_tag(d, cli["tag"]) if cli.get("tag") else "")
    if h and h == want:
        return d
    if not os.path.isfile(os.path.join(d, MARK)):
        raise NoCli(
            "external/xboxrecomp-cli is at %s, not the pin %s, and this script did not "
            "clone it: delete it, or set XBOXRECOMP_CLI_DIR to use it as it is"
            % (h[:12] or "no commit", describe(cli))
        )
    pin = resolve(d, cli)
    if h != pin:
        print(
            "%s: xboxrecomp-cli: the pin moved; checking out %s"
            % (SLUG, describe({"tag": cli.get("tag", ""), "commit": pin})),
            file=sys.stderr,
        )
        try:
            git("-C", d, "checkout", "--quiet", pin)
        except NoCli:
            git("-C", d, "fetch", "--quiet", "origin")
            git("-C", d, "checkout", "--quiet", pin)
        if head(d) != pin:
            raise NoCli("external/xboxrecomp-cli would not check out the pin %s" % pin[:12])
    with open(os.path.join(d, MARK), "w") as f:
        f.write(pin + "\n")
    return d


def clone(cli, dest):
    """A clone at the pin, made aside and moved into place only once HEAD is
    the pin: a failed clone or checkout leaves nothing behind."""
    if not cli.get("url") or not (cli.get("commit") or cli.get("tag")):
        raise NoCli("game.toml [cli] names no url to clone it from")
    check_pin(cli)
    if not shutil.which("git"):
        raise NoCli("no git on PATH to clone it with")
    tmp = dest + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    print(
        "%s: cloning %s %s into external/xboxrecomp-cli" % (SLUG, cli["url"], describe(cli)),
        file=sys.stderr,
    )
    try:
        git("clone", "--quiet", "--no-checkout", "--", cli["url"], tmp)
        pin = resolve(tmp, cli, fresh=True)
        git("-C", tmp, "checkout", "--quiet", pin)
        if head(tmp) != pin:
            raise NoCli("the clone is not at the pin %s" % pin[:12])
        excl = git("-C", tmp, "rev-parse", "--git-path", "info/exclude")
        with open(os.path.join(tmp, excl), "a") as f:
            f.write("\n/%s\n" % MARK)  # out of `git status`: no -dirty versions
        with open(os.path.join(tmp, MARK), "w") as f:
            f.write(pin + "\n")
        os.rename(tmp, dest)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    return dest


def find_cli(cli, may_clone=True):
    """$XBOXRECOMP_CLI_DIR, else external/xboxrecomp-cli (at the pin), else
    ../xboxrecomp-cli (the toolkit's order), else a clone at the pin. A
    developer's checkout (the variable, or beside this one) runs as it is."""
    env = os.environ.get("XBOXRECOMP_CLI_DIR")
    if env:
        d = os.path.abspath(env)
        if not os.path.isfile(os.path.join(d, "pyproject.toml")):
            raise NoCli("XBOXRECOMP_CLI_DIR=%s is not an xboxrecomp-cli checkout" % env)
        return d
    check_pin(cli)
    dest = os.path.join(ROOT, "external", "xboxrecomp-cli")
    if os.path.isfile(os.path.join(dest, "pyproject.toml")):
        return at_pin(dest, cli) if may_clone else dest
    beside = os.path.join(ROOT, "..", "xboxrecomp-cli")
    if os.path.isfile(os.path.join(beside, "pyproject.toml")):
        return os.path.abspath(beside)
    if not may_clone:
        raise NoCli("no xboxrecomp-cli found")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    return clone(cli, dest)


def no_cli(why):
    print("%s: %s" % (SLUG, why), file=sys.stderr)
    print("%s: no xboxrecomp-cli: %s" % (SLUG, GET_CLI), file=sys.stderr)


def uv_hint():
    hint = UV_HINT.get(sys.platform, UV_HINT["linux"])
    print(
        "%s: no uv %s on PATH; install it with: %s   (other ways: %s)"
        % (SLUG, ".".join(map(str, UV_MIN)) + "+", hint, UV_DOCS),
        file=sys.stderr,
    )


def main(argv):
    cli = cli_table()
    uv = find_uv()
    if not uv:
        # No clone without uv: nothing could run it yet.
        try:
            d = find_cli(cli, may_clone=False)
        except NoCli as e:
            d = None
            if argv in (["-h"], ["--help"]):
                no_cli(e)
        if argv in (["-h"], ["--help"]) and d:
            helptext = os.path.join(d, "src", "xboxrecomp_cli", "helptext.py")
            return subprocess.run([sys.executable, helptext, ROOT, SLUG]).returncode
        uv_hint()
        return 1
    try:
        d = find_cli(cli)
    except NoCli as e:
        no_cli(e)
        return 1
    env = dict(os.environ)
    for k in ("VIRTUAL_ENV", "UV_PROJECT_ENVIRONMENT"):  # the CLI's own .venv, not the shell's
        env.pop(k, None)
    cmd = [
        uv,
        "run",
        "--quiet",
        "--project",
        d,
        "--locked",
        "--no-dev",
        "xbr",
        "--game",
        ROOT,
        "--prog",
        SLUG,
    ] + argv
    if os.name == "nt":
        return subprocess.run(cmd, env=env).returncode
    os.execve(uv, cmd, env)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
