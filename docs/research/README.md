# Research notes

Notes on Burnout 3's formats and executable from the early reverse-engineering,
kept because they are still true of the game. They are not about this
project's code. For that, see [../architecture.md](../architecture.md) and
[../boot.md](../boot.md).

| File | About |
|---|---|
| [xbe-analysis.md](xbe-analysis.md) | The XBE: sections, libraries, entry point, kernel thunks |
| [renderware.md](renderware.md) | Criterion's RenderWare fork as linked into the game |
| [asset-formats.md](asset-formats.md) | The disc's file types, overview |
| [formats/](formats/) | One file per format: `.bgv` vehicles, global TXD, `prgdata`, static and streamed `.dat` |

## Experimental performance evidence

[Linux performance investigation (2026-10-09)](linux-performance/README.md)
records successful and unsuccessful optimization attempts, bounded queue/flip/XDK
measurements, and lessons from Mercenaries and other recompilations. Its Linux
xemu/Vulkan observations are separate from upstream Windows performance. The
linked tools run offline and include an optional short-training asset overlay;
no game files, generated code or emulator implementation are included.
