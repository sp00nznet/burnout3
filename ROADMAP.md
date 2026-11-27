# Roadmap

## Next

- **Profile saving.** The profile is created every launch, so the unskippable
  tutorial video plays before the first event of every mode. With the save
  written to `saves\` and read back, it plays once per mode, as on hardware.
- **Speed.** Races run at 2–4 fps on the software NV2A. Profile the rasteriser
  and the vertex-program interpreter first; a GPU (D3D11) backend for the
  executor comes after.
- **Menu backdrop streaks.** Fixed in the toolkit: xboxrecomp PR #355 clamps
  interpolated depth, so movie quads at the far plane pass LEQUAL. Drop this
  item once the toolkit pin includes it.
- **Sky dome seams.** Visible texture seams between the sky's segments.
- **Bump and dot-product texture stages.** They read as zero today, which dulls
  the car paint and road reflections.

## Later

- Check HL2 and the other xboxrecomp titles against the toolkit changes
  Burnout 3 needed (xboxrecomp #138-#156, merged). They were verified by the
  toolkit's unit and conformance suites and by Burnout 3, but no other title
  has been rebuilt on them yet.
- Four-pad support end to end. The runtime has four ports; Burnout 3's modes are
  two-player split-screen and pass-the-pad Party Crash, so two are enabled.
- Linux, once the xboxrecomp POSIX runtime covers the NV2A executor and USB.

## Out of scope

- Xbox Live and System Link play.
- Redistributing anything from the game: generated source, assets, movies.
- Hand-written replacements for game code. If the title does something wrong
  here, the fix is in the recompiler or the runtime.
