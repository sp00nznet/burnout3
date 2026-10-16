# Booting on the xboxrecomp runtime

How Burnout 3 gets from its XBE entry point to the "Press START" title screen
on the shared [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) runtime,
with no hand-written game code. `src/game/main.c` brings up the hardware the
XBE expects and jumps to 0x001D2807. Every screen below is drawn by the title's
own recompiled D3D8LTCG through the runtime's NV2A pushbuffer executor.

The build follows `../hl2`. `regen.sh` drives the toolkit's pipeline, and
`config/seed_functions.json` holds the function entries that only a run can
find. The old in-tree runtime fork under `src/{kernel,d3d,audio,input,apu,
nv2a,platform}` is no longer built.

![EA intro](screenshots/boot_ea_intro.png)
![Criterion intro](screenshots/boot_criterion_intro.png)
![Title screen](screenshots/boot_title_press_start.png)

## Running it

```
./regen.sh --disasm                        # disasm + func_id + abi + recomp
cmake -S . -B build-xr -DBO3_COMPILE_JOBS=3
cmake --build build-xr --config Release --target burnout3
RECOMP_PB_REPORT_MS=5000 RECOMP_FB_DUMP=frame ./bin/burnout3.exe --headless   # frame00000.bmp ...
```

[running.md](running.md) covers the flags and variables. `RECOMP_PB_EXEC=0`
turns the software GPU off: nothing is drawn, but the title runs about 50x
faster, which is the quicker way to follow the boot by its file accesses.
`saves/` holds the emulated HDD partition images; delete it for a first-boot
run.

## What stood between the entry point and the title screen

In order. Each fix is either a runtime or toolkit change, which goes upstream,
or per-title data in `main.c`.

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Fault in `xbox_MemoryLayoutInit` reading guest 0x80001000; "contiguous window failed (error 487)" | Windows cannot release part of a reservation. The RAM-mirror span leaked 1.8 GB at host 0x80000000. HL2 never saw it because its 768 MB map puts the span above 4 GB. | toolkit `fix/span-reservation-leak` |
| 2 | XAPI quick-reboots (`HalReturnToFirmware(2)`) right after start | `saves\` did not exist, so `Partition1.img` could not be created | `main.c` creates `saves/` |
| 3 | Spin in `sub_00351770` | D3D8LTCG waits on the GPU fence: device pointer at 0x0035FB48, `+0x2C` submitted, `*(+0x30)` consumed | `xbox_Nv2aMirrorFence(0x0035FB48, 0x2C, 0x30)` |
| 4 | Fault in `sub_001FD770`, reading guest 0xFFFFFFEC | RenderWare's audio device is created during engine start. `sub_0020A540` calls `DirectSoundCreate` (0x002F7432), which failed, and the NULL device went on to the pipeline builder. | `RECOMP_AC97_READY=1` plus APU MMIO routing in the fault handler |
| 5 | Spin at 0x002F8029 | DirectSound writes 3 to a DSP command word and waits for the DSP to clear it. There is no DSP56300 here. | `RECOMP_APU_DSP_ACK=0x825F8810`, observed as `ebx+0x810` in the spin |
| 6 | Unrecoverable fault in the host `memcpy` reading 0xFE830B78 | The whole APU aperture was trapped, but GP/EP DSP memory is plain RAM on hardware, and the MMIO decoder cannot emulate a lowered `rep movsd` | toolkit `fix/apu-dsp-memory-untrapped` |
| 7 | XAPI takes the dashboard-update path (`C:\xboxdash.xbe`, then launches `dashupdate.xbe`) | Two memcpy jump-table arms had been seeded as functions. That split `memcpy`, an indirect call during XAPI init went unresolved, and heap creation was skipped. | dropped the bad seeds; toolkit `fix/switch-table-offset-index` |
| 8 | Stall in `sub_002F805E` with the executor on | The doorbell ack only ran inside `se_frame`, which is gated off while the APU front end is trapped | toolkit `fix/apu-doorbell-ungated` |
| 9 | Intro movie holds on its first frame | The XMV player paces on `[device+0x1DE8]`, which only D3D's vblank handler (`sub_00350EC0`) increments | `xbox_Nv2aFrameCounter(0x0035FB48, 0x1DE8)` |
| 10 | Black screen at the language movie (`ovid\englis30.xmv`) on xboxrecomp v0.14.0, with `[ICALL] Failed to resolve 0x00014C80` repeating | 0x00014C80 is called through a pointer and packed straight after a switch table, which v0.14.0's disassembler does not walk past. v0.14.0 also leaves 0x000229E0, 0x00022B30, 0x00022D30 and 0x00067D10 unresolved. | seeded in `config/seed_functions.json`; sp00nznet/xboxrecomp#186 finds all five statically |
| 11 | Main menu stops advancing once the profile has loaded (UDATA) on xboxrecomp v0.14.0, with `[ICALL] Failed to resolve 0x000782C0` on every call | 0x000782C0 is an indirect-call target that v0.14.0's disassembler misses | seeded in `config/seed_functions.json`; sp00nznet/xboxrecomp#186 finds it statically |

Numbers 4 and 9 are what the HL2 loader needed, and 5 is the Wreckless
doorbell, which also sits at offset 0x...F8810. What changes from title to
title is the address, not the mechanism.

The address in row 5 also changes with the toolkit's allocator: the
contiguous page allocator of sp00nznet/xboxrecomp#189 moved it. Since
sp00nznet/xboxrecomp#193 (v0.14.0) the toolkit derives the doorbell itself
(`RECOMP_APU_DSP_ACK` unset means `auto`), so `main.c` no longer sets it. An
older toolkit has no `auto` and hangs at DirectSound start.

### Seeds are measurements, and some of them are wrong

`tools.seed_from_log` feeds indirect-call targets that a run reached back into
the disassembler. It accepted 0x00244404 and 0x00244544 as "aliases" inside
`sub_002442A0`. That function is the CRT `memcpy`, and both addresses are arms
of its internal jump tables, reached through `jmp [reg*4+table]` dispatches
that the lifter had emitted as indirect tail jumps. Seeding the first one ended
`memcpy` at 0x00244403, so its main table's arms fell outside the function and
another dispatch went unresolved. The fix belongs in the lifter, which now
lifts those tables as local switches (#7). A seed the tool marks as an alias
should be checked against the function it lands in before it is kept.

## The boot sequence

All of it comes from the title's own code:

```
ovid\englis30.xmv    EA intro
ovid\cri_rw30.xmv    Criterion intro
ovid\Titles30.xmv    title sequence
tracks\_femain.rws + tracks\_FEMain.xmv   "Press START to continue", attract video behind it
```

## Past the title screen: the pad, the menus, a race

With a controller the title walks its own frontend: the profile prompt, the
name keyboard, "save your profile?", the main menu, Single Event, race
options and vehicle select, then the takedown tutorial and the race load.

![Profile prompt](screenshots/boot_profile_prompt.png)
![Main menu](screenshots/menu_main.png)
![Single Event](screenshots/menu_single_event.png)
![Vehicle select](screenshots/menu_vehicle_select.png)

### Driving it with no one at the desk

```
RECOMP_PAD_LIVE=pad.txt RECOMP_PB_EXEC=1 RECOMP_FB_DUMP=f RECOMP_PB_REPORT_MS=2000 ./bin/burnout3.exe
echo start:1200 >> pad.txt       # pressed as soon as it is read
echo down:300  >> pad.txt        # short: a held d-pad auto-repeats
```

`RECOMP_PAD_LIVE` presses each line appended to the file; lines read together
fire one after another. `RECOMP_PAD_SCRIPT` takes a fixed timeline instead,
but how long a screen takes to appear under the software rasteriser varies
too much for timelines to hit menus reliably. Hold presses for about a second,
because the title polls its pad only a few times a second while rasterising.
A d-pad press held that long auto-repeats, so keep those short. `RECOMP_USB` is
on by default in `main.c`.

### What stood between "Press START" and the race

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 10 | Logo and header art are white boxes | P8 (palettised) textures were unsupported; the palette is `SET_TEXTURE_PALETTE`, 0x1B20 | toolkit `feat/pb-exec-p8` |
| 11 | With USB on, the boot parks at `LocalCache00.bin`; one port interrupt, no enumeration | The OHCI model read the driver's descriptor addresses (physical) as low RAM rather than the contiguous window | toolkit `fix/ohci-physical-descriptors` |
| 12 | Enumeration only worked when logging slowed the title | XPP owns physical page 0 (a private arena ending at 0x80001000); the contiguous allocator gave that page to XAPI's launch data | toolkit `fix/contig-skip-page-zero` |
| 13 | Enumerated but never attached | The XID class driver's AddDevice (0x00364C40) and three siblings are only reachable through the class-driver table at 0x00362AF4 | seeds in `config/seed_functions.json` |
| 14 | Frozen after the profile screens | Waiting for a sound to finish (`IDirectSoundBuffer::GetStatus`); the APU model never raised its interrupt and read voice memory as low RAM | toolkit `fix/apu-bus-and-irq` |
| 15 | Input dies in every sub-menu | A rumble `SET_REPORT` on the control pipe overwrote the input report's completion in the done queue | toolkit, one done queue per pass |
| 16 | Pad polled twice a second, then a freeze at vehicle select | Host-run ISRs and DPCs did not restore IRQL; `KPCR.Irql` (fs:[0x24]) was never written, so DirectSound's DPC took a critical section; the APU frame thread starved the title of its lock and could spin on a voice producing no samples | toolkit: IRQL dispatcher epilogue, KPCR publish, APU lock yield, underrun guard |

### What stood between the race load and 3D

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 17 | Race frozen at "Fight Through Traffic", GPU idle | x87 precision control: the Xbox rounds results to float (PC=24). RW's draw-list insert compared a double-precision value against its own float copy and walked back and forth forever | toolkit `fix/x87-precision-control` |
| 18 | Every 3D batch collapsed to one point | Vertex-program interpreter sent a paired ILU result to the temp field, not R1; and `SET_VIEWPORT_SCALE`/`_OFFSET` (c[58]/c[59]) were dropped | toolkit, vertex-program series |
| 19 | Camera matrices NaN in the race; stunt readout "-2147483648ft" | `lahf` lifted to a comment, so MSVC's `ucomiss; lahf; test ah,0x44` zero test answered at random and a guarded 0/0 ran (`sub_0016FB50`). Found with `BO3_TRAP_NAN` | toolkit `fix/lahf-unordered-compares` |
| 20 | Full-screen passes painted over the scene | Only one blend pair was implemented | toolkit, pb_exec |
| 21 | Race main view: sky only, other cars as dots; cube-map passes fine | `cmpltss` was unimplemented. RW's frustum build masks each side plane's reciprocal length with it, so all four side planes were (0,0,0,0) and every track section failed `sub_001B23F0`. Traced from the empty per-group visibility flags (`sub_001AD6B0`) back to the RwCamera planes | toolkit `fix/sse-compares` (all SSE compares, 4-lane rsqrt/rcp/sqrt, single cmps/scas) |
| 22 | Race unlit, black sky, composite passes wrong | No register combiners and only one texture stage: every pixel was "stage 0 texel x diffuse" | toolkit: `nv2a_combiner.c`, four stages, texture shader modes, fog unit, alpha test |
| 23 | Final frame had the HUD over the speed-blur but no scene | The scene is copied into the framebuffer by a quad sent in immediate mode with two texture coordinate sets; immediate mode kept texcoord 0 only and dropped it | toolkit: all 16 immediate attributes |


With those in, the Crash Nav globe and map, the garage (3D car in the parking
structure) and the race's cube-map reflection passes all render through the
title's own vertex programs.

Debug knobs used: `BO3_TRAP_NAN=<flag file>` (main.c) unmasks the SSE
invalid exception once the file exists and names the op that first makes a
NaN; `RECOMP_FRAME_TRACE=<flag file>` logs one whole frame of draws;
`RECOMP_VSH_TRACE=<n>` samples vertex-program batches.

## Where it stands

Every game mode renders and plays, driven headlessly with scripted pads:

| Mode | Players | Capture |
|---|---|---|
| Race | 1 | `race_single_event.png` |
| Time Attack | 1 | `time_attack.png` |
| Road Rage | 1 | `road_rage.png` |
| Crash | 1 | `crash_drive_sequence.png`, `crash_pileup.png`, `crash_result.png` |
| Race | 2 split-screen | `splitscreen_p1_drives.png`, `splitscreen_p2_drives.png` |
| Road Rage | 2 split-screen | `splitscreen_road_rage.png` |
| Double Impact | 2 split-screen | `double_impact.png` |
| Team Crush | 2 split-screen | `team_crush.png` |
| Party Crash | pass-the-pad | `party_crash.png` |

Two controllers are connected by default (`RECOMP_USB_PADS=2`, set in
main.c); pad 2 is host XInput pad 2 or `p2-` steps in a pad script.

Speed under the software GPU: menus ~14 fps, races ~2-4 fps on 12 cores.

Known gaps: a vertical-streak pattern over the menu backdrop movie; the
sky dome's texture seams; bump/dot-product texture modes read zero; the
profile is not saved, so every launch replays the tutorial videos.
