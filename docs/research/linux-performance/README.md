# Burnout 3 Linux performance investigation — 2026-10-09

Contributed by **djultra64**. This is evidence from an experimental Linux
recompilation that uses the original lifted game logic with an in-process
xemu NV2A/Vulkan device backend. It is **not a benchmark of this repository's
Windows/software renderer**, and does not add a supported Linux port here.

The useful results are reduced IRQ latency, avoiding unchanged uploads and
cubemap readbacks, several unsuccessful optimization attempts, and measurements
that distinguish real work from apparent idle time. A short training overlay
also removes approximately two minutes of repeated navigation without changing
the original extracted files.

[Aggregate measurements](bounded-measurements.json) retain run IDs, timestamps,
binary/artifact hashes, scope durations and limitations. The raw traces and
executables are not distributed. The independent offline
[analysis tools](../../../tools/research/linux-live/README.md) include synthetic
fixtures so interval accounting and rejection of malformed traces are testable
without game assets. Reproducing the game measurements still requires the
experimental producer; these tools do not instrument the upstream runtime.

## Environment and provenance

| Component | Audited revision / version |
|---|---|
| Burnout 3 title glue | [`79b578dcbb03868ef0a20ceceeb7aabd5f23a015`](https://github.com/sp00nznet/burnout3/tree/79b578dcbb03868ef0a20ceceeb7aabd5f23a015) |
| xboxrecomp base, with local modifications | [`b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4`](https://github.com/sp00nznet/xboxrecomp/tree/b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4) |
| xemu base, with local host/device modifications | [`478b4f496102379c7eaa7f3ec10e714a703c4300`](https://github.com/xemu-project/xemu/tree/478b4f496102379c7eaa7f3ec10e714a703c4300) |
| Hardware | AMD Ryzen Z1 Extreme, RADV PHOENIX |
| Graphics | Mesa 26.2.2, internal 1920×1440 (scale 3), display 1920×1080 at 60 Hz, FIFO presentation |
| Analysis / probe build | Python 3.14.3, GCC 15.2.0, ffprobe 7.1.3 |

Base revisions alone do not reconstruct the modified runtime. Binary hashes
identify the actual executions; the aggregate JSON is not a complete build
recipe. Linux integration also includes third-party code with licensing
constraints. No xemu/Cxbx-derived implementation is proposed for this MIT repo.
The Python analysis and overlay tools were developed for this investigation;
format references are cited, without importing codec or emulator code.

Runs were manually navigated and sometimes manually driven, with different
collision/Impact Time histories. Their rates are descriptive observations,
not controlled before/after estimates. Presents, guest swaps, vblank events,
accepted WSI requests and unique original game images are different quantities.

## What the attempts actually established

The numbered runs below are local experiment IDs, not upstream releases.
Earlier garage observations and later race observations must not be compared
as if they were the same scene.

| Attempts | Observation | Transferable lesson |
|---|---|---|
| Initial hybrid renderer | Garage around 1.45 presentations/s; Vulkan clears alone left geometry work on CPU. Black frames and unsupported methods occurred during integration. | Using Vulkan for part of a path does not establish an efficient or correct renderer. |
| RUN001–016 | Memory, MMIO, IRQ, video, profile and retarget work brought original menus/garage up; RUN016 garage 3.807/s. | Functional coverage first; movie/menu rates do not benchmark a race. |
| RUN017–018 | Avoided unchanged vertex uploads and adjusted SWATH flush behavior; garage 4.126→8.177/s. | A specific redundant operation can be a productive target. |
| RUN019–027 | HAL/selected CPU/XMV optimization left garage around 8/s. | Faster decoding and fewer diagnostic captures do not imply faster 3D. |
| RUN028–032 | FIFO budget and dispatcher wake changes reduced NOP→ISR latency from 6.613 to 0.0578 ms. Garage approximately 11→21/s; RUN032 race 12.965/s. | Measure the actual interrupt handoff, including delayed retries. |
| RUN033–038 | Validation overhead and texture-version churn were found; RUN036 regressed to 11.196/s, then RUN037/038 observed 33.357/34.067/s under differing conditions. | Cache/version work can introduce its own cost; measure recovery separately from a new optimization. |
| RUN039–044 | Scale, bus and audio changes overlapped. RUN042/044 observed 24.463/23.459/s at 1920×1440; audio pitch corrected. | Confounded runs cannot isolate a resolution effect. Audible fidelity is a separate result. |
| RUN045–046 | Kept six cubemap faces resident, eliminating six coherence readbacks per presentation. Race observations 23.459→25.412/s. | Preserve GPU-to-GPU dependencies rather than round-tripping through guest RAM when no CPU read requires it. Global rate comparison remains descriptive. |
| RUN047–049 | Fence retirement, uniform reuse and four-way memoization; RUN047/048 25.681/26.167/s. RUN049 lacked accelerator input. | Fewer submissions or comparisons are not automatically greater comparable driving throughput. |
| RUN050–054 | IRQ rejection→retry median 10.07→0.306 ms; RUN052 33.493/s. Later runs added instrumentation. | Correct a demonstrated latency mechanism; do not count each diagnostic run as another gain. |
| RUN055–061 | Two banks, deferred RELEASE, guards and WAIT required protection/coherence repairs before restoring racing. RUN061 29.623/s. | Overlapping batch openings do not establish sustained overlap or speedup. |
| RUN062 | Grouped RELEASE: 30.07691/s, about 11.97 MAIN batches/present. | No meaningful throughput improvement demonstrated. |
| RUN063 | Bulk index/constant consumption was exercised: 30.07469/s. Pushbuffer producer/PFIFO remained. | A negative result for this partial optimization neither validates nor disproves a full frontend replacement. |
| RUN064 | Conservative CPU WAIT: 28.09872/s, about 20.02 MAIN batches/present. | Observed regression; the stricter dependency policy was not itself proof of a hardware defect. |
| RUN065 | Ordered GPU WAIT: 30.09493/s, about 12.22 MAIN batches/present. | Recovered the regression without a meaningful net gain over RUN063. |
| RUN066 | Same RUN065 executable, no HUD/WAV diagnostics; 27.70575/s in a selected pre-crash interval. | Different manual/collision history prevents attributing the rate to removing diagnostics. |
| RUN067–068 | Passive cadence and XDK scopes; short training clip verified in the Linux runtime. | Explain pacing and nested waits before proposing to delete them. These runs are not an optimization A/B. |

RUN065's selected interval had 797 presents in 26.482861896 s
(30.09493472/s), including Impact Time. It does not prove steady normal racing
at 30 FPS. RUN066–068 retained scale 3; no new scale-1/480p comparison was run.
An earlier user observation of similar behavior at 480p is not a controlled
resolution experiment either.

## Queueing, batch closures and coherence

In a bounded six-second RUN065 trace:

- Device-worker service union: **4.593112173 s**.
- Barrier union within service: **1.662929177 s**; other service:
  **2.907191163 s**. Gate waiting: **22.992294 ms**.
- 2,202 MAIN closures: 1,248 NOP, 671 standalone AUX, 102 DISPLAY,
  83 vertex-bank, 11 cache-slot, one MMIO-consumer, and 86 outside the
  attributed barriers. **Zero closures were proven safely removable.**
- Logic queue time: **2.539259281 s**, of which **2.415365560 s** overlapped
  worker service for other requests. Roughly 95% of queue time was not a free,
  idle worker.
- 16,337 memory-resolve requests: **203.962 ms** own service,
  **2,187.958 ms** queueing, **142.866 ms** return latency. Of that queueing,
  **2,078.245 ms** overlapped worker service.

Do not sum barrier time, service time, queue time and GPU timestamps into a
frame budget: many intervals overlap or nest. A closure cause names the
trigger, not a proof that its dependencies can be omitted. The unclassified
closures are not automatically removable RELEASE quanta.

Sampled RUN065 CPU cycles attributed `b3_gpu_memory_arm` 8.785% of the worker
and 3.858% of the process. The path already avoids protection syscalls when
armed; it still scans pages/aliases. A range/intent/epoch memo would require
invalidation on permissions, aliases, backing and fault repair. It cannot
simply eliminate ownership faults. Sample weights are not exclusive frame time.

Guest O0 chunks accounted for 13.265% of the logic thread but only 2.026% of
the sampled process in RUN065. The main observed guest hotspot was already O2;
only seven reviewed chunks were allowed O2 by that generator. PGO/LTO were not
measured. Applying optimization indiscriminately is not an established safe gain.
The audio worker also had substantial samples, but its criticality to graphics
was not established. Linear resampling is a fidelity change, not a free speedup.

## Cadence and the XDK before the flip handler

RUN067's six-second trace had 362,561 records, with no reported dropped,
invalid or failed-I/O events:

- 143 complete post-handler `waiting_for_flip` intervals occupied a union of
  **120,968 ns (0.120968 ms)**; median 641 ns, maximum 10,540 ns.
- 144 presentation requests were accepted, zero drops; attempt elapsed time
  summed to **16.021440 ms**. Distinct source-image/draw tuples did not prove
  unique original images or completed physical scanout.
- Median observed WRITE interval: **40.0004245 ms**.

That only measured the post-handler state. RUN068 additionally timed guest
functions at VAs `0x00351090` (Swap), `0x00351770` (waiter) and `0x003518E0`
(reserve), without changing original guest register/memory effects:

| Scope | Complete calls | Inclusive wall ms | Exclusive wall ms |
|---|---:|---:|---:|
| Swap | 182 | 694.394868 | 322.319752 |
| waiter | 182 | 372.075116 | 372.075116 |
| reserve | 121 | 78.751579 | 78.751579 |

The 485 complete scopes formed a **773.146447 ms union in six seconds**
(12.885774%). The waiter nested inside Swap; adding both inclusive sums would
inflate exposure. Same-caller overlap with queue, own service and wake/return
was 353.293704, 11.362881 and 17.323071 ms respectively. The remainder is not
proven CPU busy time, pure vsync time or safely removable work.

Waiter median: 1.884 µs; maximum: 45.141388 ms. There were 42 calls at least
1 ms, nine at least 10 ms. Nesting/stack shape was consistent with the prior
buffer-token path, but arguments and return addresses were not captured; exact
callsite attribution remains an inference. The trace had 395,072 records and
no reported loss. Observed WRITE median was 31.992223 ms; 183 WSI requests,
zero drops. Manual race captures showed ordinary driving, but do not establish
a deterministic comparison to RUN065/067.

## What Mercenaries teaches, and what it does not

The audited [Mercenaries-Recompiled snapshot](https://github.com/KraftMacAndChee/Mercenaries-Recompiled/tree/f513ae4e041401b7b032410016940971488d523b)
was `f513ae4e041401b7b032410016940971488d523b`; 1,114 text-file hashes were
recorded locally. It was reviewed as source, not built or benchmarked locally.
Its [release notes](https://github.com/KraftMacAndChee/Mercenaries-Recompiled/releases)
include selectable 30/60/90/120/unlimited caps. A selectable cap is not a
reproducible throughput measurement. The
[public readme](https://github.com/KraftMacAndChee/Mercenaries-Recompiled/blob/f513ae4e041401b7b032410016940971488d523b/ports/mercenaries/resources/public-readme.txt)
does not guarantee high-rate performance, and
[compatibility notes](https://github.com/KraftMacAndChee/Mercenaries-Recompiled/blob/f513ae4e041401b7b032410016940971488d523b/COMPATIBILITY.md)
limit how much one machine/path establishes about timing independence.

| Source finding | Consequence for Burnout experiments |
|---|---|
| Shader bytecode cache and warmup (`src/d3d/d3d8_shader_cache.c`, title `shader_warmup.c`) | Can reduce cold stutter. First establish compilation/creation continuing in the measured warm race; DXBC cannot be reused as a Vulkan pipeline cache. |
| Byte equality and dirty constants/programs (`d3d8_vsh.c`, `d3d8_combiners.c`, `nv2a_pgraph_d3d11.c`) | Reuse immutable versions in flight, audit missing cases. Preserve exact bits, padding, NaNs, signed zero and lifetime. |
| Bulk non-incrementing command runs (`src/nv2a/nv2a_core.c`) | Preserve PUT/GET, DMA, budget and class. Our partial bulk consumer already had a negative rate result; this does not measure a full replacement. |
| Dynamic buffers/resident surfaces | D3D11 renaming has different synchronization contracts from Vulkan fences/ownership. |
| Texture same-binding shortcut | The reviewed condition can skip validation despite a changed generation. Same binding does not prove immutable content; do not weaken coherence to reproduce a speed claim. |
| Linear resampling/optional DSP | Changes filtering/fidelity; no demonstrated graphics benefit here. |
| XMV/REP MOVS helpers | Compare complete guest effects and overlap/direction semantics. Title addresses/layouts are not universal XDK contracts. Faster video is not necessarily faster racing. |
| High-FPS timing patches | Unlocking presents also requires validating title-specific logic. Mercenaries timer addresses are not Burnout patches. |

Reading the source provides ideas; no Mercenaries implementation is copied in
this contribution. Audit individual changes instead of replacing the translator
or runtime wholesale and losing more general local semantic corrections.

## Other references and the scope of the conclusion

[JSRF](https://github.com/andeecollard/jsrf-recomp#the-direct3d-lift) reports a
35–37→12.7 ms result on Apple Silicon. Its
[G73–G76 report](https://github.com/andeecollard/jsrf-recomp/blob/main/docs/jsrf/goals/JSRF_GOALS_2026-09-25_EVENING_FINISH_THE_LIFT.md)
attributes successive gains to measured waits, diagnostic hashing, preparation
and acknowledgements. This is a precedent for a large gain after identifying
specific costs, not a transferable multiplier. Remaining D3D bookkeeping was
small enough that further replacement was not automatically useful there.

[Halo's published optimization history](https://github.com/thelinkin3000/halo-ce-universal-bnunu#optimisation)
reports 103→303 FPS cumulatively on another machine/scene, including interpolation
with 30 Hz simulation. It is not a Burnout throughput estimate.
[reB3](https://github.com/lolz0r/reb3/tree/ce57602cc9fb59c54487fe4241daf8391dfd0b0b)
showed approximately 59.627 swaps/s in a local short 720p reconstruction harness,
with different game coverage and dummy audio. Its residency and GPU postprocess
principles are useful; that measurement is not proof of retail game logic at 60.

From RUN065's nominal 33.228 ms/present, 60/90/120 would require approximately
49.84%/66.56%/74.92% shorter intervals. The captured MAIN GPU timestamp around
7.35 ms/present excludes other critical work; it is not proof of a 120 FPS path.
The measured data did not justify promising those targets within a personal
one-to-two-day work budget. **This is not proof that 60 FPS is impossible**, nor
a decision about upstream's roadmap or another backend. A full frontend remains
unmeasured; a short prototype does not establish equivalent coverage or total
savings. A useful next experiment must identify a correct removable dependency,
measure its critical-path effect, and compare matched scenes.

## Additional title and lifetime findings

These are source-audit findings from the local modified runtime, not fixes
validated in upstream Windows:

- The original XDK producer can publish more work before an older release
  completes, provided pushbuffer/resource reuse is safe. GET advancement,
  publication, a fence token and GPU completion are distinct witnesses.
  Swap's alternating previous-buffer token is a real dependency. Never copy
  the write counter into the read counter to manufacture completion.
- Supporting two batches requires resources with per-slot lifetimes, retained
  release destinations and ordered stores after actual GPU completion.
  Descriptors, uniforms, vertices, indexes, query slots and surface references
  must survive until every dependent submission retires. A mapping lease is
  not an allocation-generation pin; allocator coalescing/reuse can invalidate
  a seemingly stable pointer or table index. CPU coherence and physical free
  also require different operations. Existing allocator ownership should remain
  authoritative rather than adding a parallel renderer allocation ledger.
- The original IRQ bridge needs the actual import slots/ordinals, not four
  convenient consecutive thunk entries. The audited slots 76/93/94/98 resolve
  ordinals 107/98/109/119 (DPC init, interrupt connect/init, DPC enqueue).
  A component fixture reaching one RDI operation did not establish completion
  of an ISR, NOP or frame; unknown targets remained explicit failures.
- Static widescreen inspection found the original XAPI getter shifts the
  `XC_VIDEO` DWORD right by 16 and masks it. A local HLE value of `0x03`
  therefore delivered zero video flags despite a comment claiming widescreen.
  The title stores the widescreen boolean at `0x004D617D`, retains 640×480,
  and changes an initialization field from `0x100` to `0x110`.
  Encoder-option mode filtering also tests `0x00010000`. These observations
  support investigating the original anamorphic route. They do not validate
  every camera/HUD/movie/split-screen path, and stretching a 4:3 WSI rectangle
  alone is not an equivalent widescreen fix.

No generated function bodies, disassembly listings, captured pushbuffers or
third-party device implementation are included with these descriptions.

## Repeated-training shortcut

The [overlay utility](../../../tools/research/linux-live/README.md#short-training-overlay)
retains the original first complete XMV packet: six frames/200 ms, rather than
synthesizing a replacement codec stream. It sets EOF lookahead fields and duration,
leaving all retained payload bytes unchanged. The original clip is 123,563 ms;
the derived clip is 200 ms, reducing clip duration by 123.363 seconds. This is not
an exact boot-time saving, because menu inputs and loading are separate.

ffprobe 7.1.3 decoded all six frames without errors. The Linux guest reached a
controllable race; the human tester confirmed narration stopped on transition.
The narration is separate from the clip (the original XMV has zero audio tracks).
Upstream Windows playback has not been tested. The original ISO and extracted
clip remained unchanged. Treat the overlay as an optional research fixture,
never a normal-gameplay default or a distributed asset.
