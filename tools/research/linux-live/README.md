# Offline Linux LIVE research tools

These MIT Python tools analyze the experimental trace format discussed in
[the performance investigation](../../../docs/research/linux-performance/README.md).
They do not start a game, collect traces, add a Linux backend or improve the
Windows runtime's performance. Python 3.10+ is required. No third-party Python
packages are needed.

Run the independent synthetic fixtures from the repository root:

```sh
python tools/research/linux-live/scripts/analyze-xemu-live-trace.py --self-test
python -m unittest discover -s tools/research/linux-live/tests/scripts -v
```

The asset-dependent training tests skip unless `B3_TRAINING_ASSET` points to
your own original `ovid/TrD_ENG.xmv`; those tests also require `ffprobe` on PATH.
No fixture contains extracted game bytes. They use disposable directories under
this packet's ignored `out/` directory.

## Trace analyzers

```sh
python tools/research/linux-live/scripts/analyze-xemu-live-trace.py /path/to/causal.bin --game-log /path/to/game.log
python tools/research/linux-live/scripts/analyze-xemu-live-critical-path.py /path/to/causal.bin --game-log /path/to/game.log --output /path/to/critical.json
python tools/research/linux-live/scripts/analyze-xemu-live-cadence.py /path/to/cadence.bin --game-log /path/to/game.log
python tools/research/linux-live/scripts/analyze-xemu-live-xdk-scopes.py /path/to/scopes.bin --game-log /path/to/game.log
```

Use `--help` for output options. The critical-path analyzer handles the older
service/barrier events; use the cadence/scope analyzers for their corresponding
extended captures. Unknown events fail closed rather than silently disappearing.

The v1 binary header consists of eight little-endian u64 fields: magic
`0x4233545241434531`, version 1, record bytes 56, endian marker
`0x0102030405060708`, window begin/end monotonic ns, capacity, clock ID (1).
Each record is seven little-endian u64 fields: reservation sequence, monotonic
ns, TID, event, correlation ID, a, b. Event names/layout decoding live in the
parser. This is a custom experimental format, not an upstream logging format.

The parsers check headers, sequence continuity, worker ordering, correlation
IDs, nesting, censored edges and producer statistics. Missing producer loss
statistics remain unknown, rather than a claim of zero loss. Same-worker
ordering constraints do not invent globally sorted producer timestamps.
Wall intervals are not CPU residency. Nested intervals use unions/exclusive
partitions; accepted presents are not physical scanout or unique original images.
Synthetic tests verify interpretation, not hardware behavior or replay coverage.

## Short training overlay

Only the audited English training clip SHA256
`1cd0e83f03c859dbb1f0fb59bad551293af7b1056e5522b3966ad8b305e3b662`
is supported. Other regions/revisions fail without creating an overlay.

```sh
python tools/research/linux-live/scripts/prepare-short-training.py \
  --source '/path/to/your/extracted/disc' \
  --output tools/research/linux-live/out/short-training
```

The output must be a new directory under `tools/research/linux-live/out/`.
Existing outputs are never overwritten. The script reads the original XMV,
validates its hash/header/frame boundaries, retains one complete packet and
changes only u32 offsets 0, 28 and 36 (EOF lookahead, duration, EOF lookahead).
It creates symlinks to all other extracted assets and writes an audit manifest.
The extracted source and ISO are never written. Remove only the disposable
overlay to clean up; keep its source directory available while using it.

Symlinks require platform support and permissions (Windows commonly needs
Developer Mode or administrator rights). The tool reports failure and cleans
its exclusively created output directory if linking fails. No Windows test was
performed. Offline decode and the Linux guest's race transition/narration stop
were verified, as described in the report.

**This does not automatically configure the upstream executable.** At the
reviewed revision, `src/game/main.c` uses `BO3_GAME_DIR` for asset mounting;
passing a different XBE path alone does not redirect all game assets. Use the
overlay only with a research runtime that explicitly supports selecting the
game directory, or deliberately configure a separate research build. No runtime
change or replacement of your normal extracted directory is part of this PR.

Format references used for investigation (no code imported):
[FFmpeg XMV demuxer](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/xmv.c),
[xboxrecomp XMV parser at the audited revision](https://github.com/sp00nznet/xboxrecomp/blob/b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4/tools/xmv/xmv_demux.py).
The shortened video remains derived game data and must not be committed or shared.
