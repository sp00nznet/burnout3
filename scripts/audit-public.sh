#!/usr/bin/env bash
# Before a push to the public fork (apexJCL/burnout3-recomp): what the push would
# publish, checked for what must never leave the maintainer's machines.
# xboxrecomp-cli's scripts/audit-public.sh, with this fork's ranges, the
# upstream screenshots and the README images allowed, and a refusal list.
#
#   scripts/audit-public.sh            the range public/main..main
#   scripts/audit-public.sh RANGE      another range (A..B)
#   scripts/audit-public.sh --all      every tracked file at HEAD, and the
#                                      whole history's messages
#
# Checked, over every commit of the range (a push publishes the history,
# not only the tip: a line or a file added and removed again inside the
# range is still published):
# - added lines and commit messages for private paths (home directories on
#   macOS, Linux and Windows, an immutable host's /var/home), links into
#   Claude's web app and the private patterns below;
# - the files the range adds or changes for binaries and game data (*.xbe
#   *.iso *.xiso *.bmp *.png *.wav *.sav, the dump's own formats as
#   .gitignore lists them, and *.pack, a texture pack made from game data;
#   any case), except two slots: upstream's docs/screenshots/*.{png,gif},
#   only while each is upstream's file unchanged, and the README's
#   docs/images/*.png, which must carry no text or Exif chunks;
# - those files and the tree at the range's tip (HEAD with --all) for the
#   refusal list: the dump, saves, runs, bench logs, bundles, build trees,
#   generated code and the local-only notes, which .gitignore keeps out
#   today and a `git add -f` or a changed .gitignore could slip in;
# - every author and committer of origin/main..<the range's tip> (HEAD with
#   --all), which must be carlos with the noreply address. Only that range:
#   the commits below origin/main are upstream's (sp00nz's) and keep their
#   identity.
# Exit 1 on any hit, 2 when it cannot check.
#
# Private patterns (the bench host's name, the personal email, anything
# else that must not appear) are not in this repository, since naming them
# here would publish them: one extended regex per line, matched without
# case, in the file $XBR_AUDIT_PRIVATE or else <git dir>/info/audit-private.
# Without one the audit refuses to pass.
#
# The regexes below bracket a letter (/[U]sers/) so this file does not
# match itself.
set -euo pipefail

NAME="carlos"
NOREPLY="4037632+apexJCL@users.noreply.github.com"
PUBLIC_PATTERNS=(
    '/[U]sers/'
    '/[h]ome/'
    '/var/[h]ome/'
    '[A-Za-z]:(\\\\|\\|/)[U]sers(\\\\|\\|/)'
    '[c]laude\.ai'
)
# Matched without case: a dump copied from a FATX or Windows disk keeps
# upper-case names.
DATA_EXT='\.(xbe|iso|xiso|bmp|png|wav|sav|pack|xpr|xwb|adx|sfd|ipk|xsv|xmv|rws|awd|ppm|raw)$'
UPSTREAM_SHOTS='^docs/screenshots/[^/]+\.(png|gif)$'
README_IMAGES='^docs/images/[^/]+\.png$'
# Never tracked (extended regexes on the path).
REFUSE=(
    '^_local/'
    '^Burnout 3 Takedown(/|$)'
    '^saves/'
    '^runs/'
    '^bench-logs(/|$)'
    '^dist/'
    '^build[^/]*/'
    '^bin/'
    '^src/game/recomp/gen(\.[^/]*)?/'
    '^TESTING\.md$'
    '^CLAUDE\.md$'
    '^\.claude/'
)

# Paths as they are (no octal quoting), so a non-ASCII name still ends in
# its extension and still matches the rules above.
g() { git -c core.quotePath=false "$@"; }

cd "$(git rev-parse --show-toplevel)"
all=0
range="public/main..main"
case "${1:-}" in
    --all) all=1 ;;
    -h | --help)
        sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
        exit 0
        ;;
    "") ;;
    *) range=$1 ;;
esac
if [ $all = 0 ] && ! git rev-parse -q --verify "${range%%..*}^{commit}" >/dev/null; then
    echo "audit-public: no ${range%%..*} (no public remote, or nothing fetched?): use --all" >&2
    exit 2
fi
if ! git rev-parse -q --verify "origin/main^{commit}" >/dev/null; then
    echo "audit-public: no origin/main: fetch upstream (origin) first" >&2
    exit 2
fi
if [ $all = 1 ]; then
    tip=HEAD
    hist=HEAD
else
    tip=${range##*..}
    tip=${tip:-HEAD}
    hist=$range
fi
ident_range="origin/main..$tip"

private=${XBR_AUDIT_PRIVATE:-$(git rev-parse --git-path info/audit-private)}
if [ ! -s "$private" ]; then
    echo "audit-public: no private patterns in $private, so it cannot check for them." >&2
    echo "Write one extended regex per line (the bench host's name, the personal email)," >&2
    echo "once per clone:" >&2
    echo "    printf '%s\n' 'hostname' 'name@example[.]com' > \"\$(git rev-parse --git-path info/audit-private)\"" >&2
    echo "or set XBR_AUDIT_PRIVATE to a file that has them." >&2
    exit 2
fi
patterns=("${PUBLIC_PATTERNS[@]}")
while IFS= read -r p || [ -n "$p" ]; do
    [ -n "$p" ] && [ "${p:0:1}" != "#" ] && patterns+=("$p")
done <"$private"

hits=0
hit() {
    echo "HIT  $*"
    hits=$((hits + 1))
}
matches() {
    local a f=$1
    shift
    for a in "$@"; do
        [[ $f =~ $a ]] && return 0
    done
    return 1
}
data_ext() {
    local r=1
    shopt -s nocasematch
    [[ $1 =~ $DATA_EXT ]] && r=0
    shopt -u nocasematch
    return $r
}
# Every blob of path $2 that the commits of $1 wrote.
blobs() { g log --raw --no-abbrev --no-renames --format= "$1" -- "$2" | awk '$4 !~ /^0+$/ {print $4}' | sort -u; }
# Upstream's screenshot slot: each version the range wrote is one upstream
# has had (a merge of upstream brings its new ones), and the tip's is
# origin/main's, so nothing of ours goes out under its name.
upstream_file() {
    local f=$1 b up
    up=$(blobs origin/main "$f")
    [ -n "$up" ] || return 1
    for b in $(blobs "$hist" "$f"); do
        grep -qx "$b" <<<"$up" || return 1
    done
    b=$(git rev-parse -q --verify "$tip:$f" 2>/dev/null) || return 0
    [ "$b" = "$(git rev-parse -q --verify "origin/main:$f" 2>/dev/null)" ]
}
allowed() {
    [[ $1 =~ $README_IMAGES ]] && return 0
    [[ $1 =~ $UPSTREAM_SHOTS ]] && upstream_file "$1"
}
# The chunk types of a PNG on stdin, or "not-a-png".
png_chunks() {
    python3 -c '
import struct, sys
d = sys.stdin.buffer.read()
if d[:8] != b"\x89PNG\r\n\x1a\n":
    print("not-a-png")
    sys.exit()
i = 8
while i + 8 <= len(d):
    n, t = struct.unpack(">I4s", d[i : i + 8])
    print(t.decode("latin-1"))
    i += 12 + n
'
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# What is checked: the text (each commit's added lines as "sha file:line:
# text", or the whole tree as "file:line:text"), the messages,
# the files written, the tree at the tip, the identities.
if [ $all = 1 ]; then
    text() { g grep -I -n -E -i -e "$1" HEAD -- . | sed 's/^HEAD://' || true; }
    messages=$(git log --format='%H %B' HEAD)
    files=$(g ls-files)
    empty=$(git hash-object -t tree /dev/null)
    binaries=$(g diff --numstat "$empty" HEAD | awk -F'\t' '$1 == "-" {print $3}')
else
    # Every added line of every commit, as "sha file:line: text".
    g log -p --unified=0 --no-renames --no-color --format='commit %h' "$range" | awk '
        /^commit [0-9a-f]+$/ { c = $2; hdr = 0; next }
        /^diff --git / { hdr = 1; next }
        hdr && /^\+\+\+ / { f = substr($0, 5); sub(/^b\//, "", f); next }
        /^@@ / { match($0, /\+[0-9]+/); n = substr($0, RSTART + 1, RLENGTH - 1) + 0; hdr = 0; next }
        !hdr && /^\+/ { print c " " f ":" n ": " substr($0, 2); n++ }
    ' >"$tmp/text"
    text() { grep -E -i -e "$1" "$tmp/text" || true; }
    messages=$(git log --format='%H %B' "$range")
    files=$(g log --name-only --no-renames --diff-filter=AMR --format= "$range" | sed '/^$/d' | sort -u)
    binaries=$(g log --numstat --no-renames --diff-filter=AMR --format= "$range" | awk -F'\t' '$1 == "-" {print $3}' | sort -u)
fi
tree=$(g ls-tree -r --name-only "$tip")
idents=$(git log --format='%h%x09%an%x09%ae%x09%cn%x09%ce' "$ident_range")

for p in "${patterns[@]}"; do
    while IFS= read -r line; do
        [ -n "$line" ] && hit "text /$p/: $line"
    done < <(text "$p")
    while IFS= read -r line; do
        [ -n "$line" ] && hit "message /$p/: $line"
    done < <(grep -E -i -e "$p" <<<"$messages" || true)
done
while IFS= read -r f; do
    [ -z "$f" ] && continue
    allowed "$f" && continue
    data_ext "$f" && hit "game data or capture: $f"
done <<<"$files"
while IFS= read -r f; do
    [ -z "$f" ] && continue
    allowed "$f" && continue
    hit "binary: $f"
done <<<"$binaries"
# The README's images: every version published, with no text or Exif
# chunk (a path, a host or a date can ride in one).
while IFS= read -r f; do
    [[ -n $f && $f =~ $README_IMAGES ]] || continue
    for b in $(blobs "$hist" "$f"); do
        bad=$(git cat-file blob "$b" | png_chunks | grep -E -x 'not-a-png|tEXt|iTXt|zTXt|eXIf' | sort -u | tr '\n' ' ') || true
        [ -n "$bad" ] && hit "image chunks: $f (${b:0:12}): $bad"
    done
done <<<"$files"
while IFS= read -r f; do
    [ -z "$f" ] && continue
    matches "$f" "${REFUSE[@]}" && hit "never tracked: $f"
done < <(printf '%s\n%s\n' "$files" "$tree" | sort -u)
while IFS=$'\t' read -r sha an ae cn ce; do
    [ -z "${sha:-}" ] && continue
    [ "$an" = "$NAME" ] && [ "$cn" = "$NAME" ] && [ "$ae" = "$NOREPLY" ] && [ "$ce" = "$NOREPLY" ] && continue
    hit "identity: $sha author $an <$ae> committer $cn <$ce> (want $NAME <$NOREPLY>)"
done <<<"$idents"

if [ $hits -gt 0 ]; then
    echo "audit-public: $hits hits" >&2
    exit 1
fi
echo "audit-public: clean ($([ $all = 1 ] && echo "HEAD, whole history" || echo "$range"); identities $ident_range)"
