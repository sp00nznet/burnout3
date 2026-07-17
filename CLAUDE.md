# Burnout 3: Takedown - Static Recompilation Project

## Project Overview
Static recompilation of Xbox Burnout 3: Takedown (2004) for Windows 11.
The goal is to translate the original x86 Xbox code into a native Windows executable.

## Key Facts
- **XBE**: `default.xbe` is a retail Xbox executable, XDK 5849, base address 0x00010000
- **Entry Point**: 0x001D2807 (retail, XOR-decoded with 0xA8FC57AB)
- **Engine**: Criterion's custom RenderWare fork (~3.7), statically linked
- **Code Size**: 2.73 MB in .text section (game + CRT + RW engine)
- **Kernel Imports**: 147 Xbox kernel functions to replace with Win32
- **Libraries**: 11 statically linked XDK libs (D3D8LTCG, DSOUND, XMV, XONLINE, etc.)

## Repository Structure
- `docs/` - Detailed analysis and documentation
- `tools/xbe_parser/` - XBE file parser (Python)
- `tools/disasm/` - Disassembly tools (planned)
- `tools/asset_tools/` - Asset conversion (planned)
- `src/kernel/` - Xbox kernel replacement layer (Win32)
- `src/d3d/` - D3D8→D3D11 graphics abstraction
- `src/audio/` - Audio system (DSOUND→XAudio2)
- `src/input/` - Input system (XPP→XInput)
- `src/game/` - Decompiled game code

## Git/GitHub
- Remote: https://github.com/sp00nznet/burnout3.git
- Game assets (`Burnout 3 Takedown/`) are gitignored (large binary files)
- All toolchain code and documentation IS committed

## Conventions
- Python tools use Python 3.10+
- C/C++ code targets MSVC (Visual Studio 2022) or MinGW-w64
- Addresses are always shown as hex with 0x prefix (e.g., 0x001D2807)
- Xbox kernel function names use their original Xbox names with `xbox_` prefix when reimplemented

## Current Work State (Session 43)

### Status: PLAYABLE — driving on real track geometry at 32fps
Game boots to menus, R key launches race, player drives on real Burnout 3 track geometry rendered through recompiled gen code pipeline. Gen code render chain (sub_00351490→sub_00351770_gen→sub_00350C10) active. Gameplay state machine transitions 5→4→5 working. Track geometry drawn via D3D8 DrawPrimitiveUP with pre-transformed vertices. Chase camera follows physics body. Pre-race cinematic plays (flyover camera cuts). 6000 verts/frame, 7 batched draws, 32fps.

### Session 43 Changes (2026-03-20)
- **Gen code render chain un-stubbed**: sub_00351490 (begin camera), sub_00351770_gen (62K scene render), sub_00350C10 (end camera) all running
- **D3D8LTCG device context fully fixed**:
  - GPU read pointer trick: device+0x30 → device+0x2C (eliminates PB spin loops)
  - PB ring management: +0x24/+0x28/+0x2C/+0x30/+0x34/+0x38/+0x44/+0x48 all patched post-snapshot
  - RT surfaces at +0x1974/+0x1978 initialized non-NULL
  - Camera active flag (device+8 bit 14) forced per-frame
- **24 D3D8LTCG stubs fixed**: Proper Xbox stack cleanup calling conventions (esp += 4/8/12)
- **Gameplay state machine**: R key launches race via fe_start_race()
  - State transitions: menu(5) → race init(4) → gameplay(5)
  - Phase state machine (sub_001AA100): phases 1→3→4→5→6→7→9→0x13
  - Phase 9 timeout after 30 tries (render list builder can't complete without RW track loading)
  - g_race_init_done signal prevents state oscillation
- **Track geometry rendering**: Direct D3D8 DrawPrimitiveUP with batched pre-transformed vertices
  - 6000 verts/frame from 49 track chunks, 7 draw calls
  - View-projection transform, distance culling (500 unit radius)
  - Triangle strip → triangle list conversion
- **Chase camera**: Physics body-based fallback with 75° FOV, heading-relative positioning
- **Float ICALL guard**: Rejects IEEE 754 values (exponent 50-200) as vtable entries
- **Input system**: fe_menu_is_racing() fallback for gameplay detection
- **Race launch flow**: R key → fe_start_race → state=4 → phase 1-9 → timeout → state=5 → gameplay
- **g_force_respawn**: Resets physics init flag when launching race (prevents boot-time flag blocking)
- **D3D8LTCG device context reference doc**: `docs/d3d8ltcg_device_context.md` (reusable)
- **Log noise reduced**: Mirror failures (3+50K), SKIP-READ (10+100K)

### Session 42 Changes (2026-03-17)
- **Live render pipeline investigation**: Attempted to enable original sub_001AE6F0 call chain
  - sub_0003FEE0 (RW state setup) crashes on uninitialized D3D device context fields
  - Even with D3D8LTCG mid-entry stubs, gen code walks pointer chains through device+0xCA0+
  - sub_001AD350 (render list dispatch) returns immediately: render entry table res[+24]=NULL
  - Render entry table populated by sub_0003FEE0 chain → circular dependency
- **D3D8LTCG mid-entry stubs added** (recomp_manual.c):
  - sub_0034F5B0 (70K render state flush) — stubbed, returns current PB position
  - sub_003558A0 (45K render state flush) — stubbed, returns current PB position
  - sub_0034D410 (79K render state flush) — stubbed, returns current PB position
  - These are all entry points into the same giant D3D8LTCG function (ends at 0x360A54)
- **D3D device context snapshot captured from xemu** (`src/nv2a/d3d_device_snapshot.h`):
  - Device is STATIC at 0x0035D6A0 (in D3D8LTCG section), NOT heap-allocated
  - 16KB snapshot captured during menu rendering (game state 5, cam=0x4D4008)
  - Loaded at boot via memcpy, with PB/surface/viewport fixups
  - Capture script: `tools/xemu_debug/capture_d3d_device.py`
- **Key findings**:
  - Render list resource at 0x01C7D000: res[+08]=native ptr, res[+24]=NULL (entry table)
  - sub_0003FEE0 STILL crashes even with snapshot — 16KB contains hundreds of xemu
    heap pointers (0x0078xxxx etc.) that can't be trivially fixed up
  - Screen entries exist (1736 at 0x4B01B0) but never processed for rendering
  - sub_0003FEE0 calls: sub_0034F5B0, sub_003518E0, sub_003558A0, sub_0003FE10,
    sub_0034D410, sub_00040CF0 — gen code walks deep pointer chains in device state
- **Manual sub_0003FEE0 override** (recomp_manual.c):
  - Performs safe matrix copies (game_obj+0x500→+0x6E0, device+0xCA0→game_obj+0x540)
  - Calls sub_0034C2E0 (D3D clear) and sub_001AD350 ×3 (render dispatch)
  - Skips D3D8LTCG state flush (sub_0034F5B0/sub_003558A0/sub_0034D410)
  - Skips sub_00040CF0 (walks device pointer chains)
  - **Restored 60fps** (was 32fps with direct D3D clear in sub_001AE6F0)
  - Gen code disabled in recomp_0000.c (#if 0)

### Session 40 Changes (2026-03-15)
- **NV2A PGRAPH → D3D11 translator** (`src/nv2a/nv2a_pgraph_d3d11.c/h`): New reusable module
  - Intercepts NV2A Kelvin method calls from push buffer parser
  - Handles: SET_BEGIN_END, INLINE_ARRAY, CLEAR_SURFACE, blend/depth/cull state, viewport
  - Converts 5-dword inline vertices (X,Y,U,V,Color) to 28-byte XYZRHW+Diffuse+TEX1
  - QUADS (mode 8) → triangle list conversion (6 verts per quad)
  - Routes through D3D8 COM `DrawPrimitiveUP` → D3D11
  - 24 draw calls/frame, 1079 vertices, correct Burnout 3 menu layout
- **Push buffer replay** (`src/nv2a/nv2a_pb_replay.c`): Replays captured xemu data
  - 32KB push buffer captured from xemu during menu rendering
  - 268 NV2A command headers parsed, 6471 methods/frame dispatched
  - Auto-activates after 60 frames for testing
- **xemu debug session**: Comprehensive menu state capture
  - Screen list vtable = 0x003A9FA4 (10 methods, all in gen code, NOT stubbed)
  - 536 screen entries at 0x004B01B0, flag=2 (active)
  - Frontend vtable at 0x003A9E7C confirmed, [3]=0x00014D20
  - Render context at 0x35FB48 → NV2A push buffer DMA state
  - sub_001AE6F0 NOT called during menus (wrong assumption corrected)
  - Real chain: sub_00014D20 → sub_0034C2E0 + sub_0034D530 from RW pipeline
  - Menu rendering uses INLINE_ARRAY with TRIANGLE_STRIP/QUADS, no im2d
- **pgraph_method() updated**: Routes through D3D11 translator before legacy logging
- **Bridge render path**: When replay active, clears to dark blue + draws replay geometry
- **Captured push buffer data**: `src/nv2a/menu_pushbuffer_data.h` (embedded C array)

### Session 40 Key Discovery: Menu Render Pipeline
The original menu rendering does NOT use im2d or sub_001AE6F0. Instead:
1. `sub_000171A0` → vtable[3] → `sub_00014D20` (per frame, calls sub_0034C2E0 for D3D clear)
2. RW display pipeline → `sub_0034D530` (79K D3D8LTCG, writes NV2A push buffer commands)
3. Push buffer contains: BEGIN_END(TRIANGLE_STRIP/QUADS) + INLINE_ARRAY vertex data
4. Vertex format: float X, float Y, float U, float V, D3DCOLOR (20 bytes, 640×480 screen space)
5. ~448 vertices/frame in ~25 draw batches = textured 2D quads for all menu UI elements

### Session 39 Changes (2026-03-13)
- **sub_001AA100 REWRITTEN**: Full phase state machine (phases 1-9 → 0x13) instead of simple 30-frame skip
  - Phase 1: sub_001B5A80 (scene init)
  - Phase 3: sub_0018B250 (resource check)
  - Phase 4: sub_0013EA20 (audio init, timeout after 5 tries)
  - Phase 5: Scene descriptor sanitization (garbage count fix)
  - Phase 6: B790/B794 screen definition check (already populated at 0x0045BAD0)
  - Phase 7: sub_0018E820 + sub_001888F0
  - Phase 9: sub_0019AE10 (render list builder) — successfully creates resource at 0x01C7D000
- **sub_001AE6F0 overridden**: Frontend render dispatch, currently D3D clear + diagnostics
  - Original calls sub_001AD350 (render list dispatch) → sub_0034D530 (79K render) → im2d
  - Both disabled due to native pointer issues
- **Screen list garbage data IDENTIFIED AND FIXED**:
  - Screen list sub-object at 0x004AF590 had 0x3F800000 (1.0f) in count/index fields
  - sub_001AED70 (find-free-slot) was scanning from index 1065353216, writing entries beyond array
  - PATCH: Zero count and index fields before sub_0001F7C0 screen registration
- **Screen entry invalidation DISABLED** (gen patches):
  - sub_001AEE20 (recomp_0004.c): vtable guard changed to keep entries active, not invalidate
  - sub_0001F7C0 (recomp_0000.c): 19 invalidation blocks commented out (all had NULL vtable → eax=0 → decrement)
  - Screen entries now persist with flag=1 (active) instead of being immediately cleared
- **Screen list vtable = NULL**: Root cause is C++ constructor never ran for screen list sub-object
  - Object at ebp+0x83E0 (0x004AF580 with Xbox base 0x004A71A0) initialized with data but no vtable
  - RECOMP_ICALL_SAFE(0) returns eax=0, causing validation failure
  - All 3 vtable methods (offsets +0, +8, +0xC) read MEM32(0+offset) = 0 → lookup fails
- **recomp_0004.c gen patches**: sub_001AE6F0 `#if 0`, sub_001AEE20 vtable guard fix
- **recomp_0000.c gen patches**: Screen list init PATCH, 19× invalidation disable, screen array trace

### Session 38 Changes
- **Im2d rendering pipeline WORKING**: RwIm2DRenderPrimitive (sub_001DE900) overridden
  - Routes pre-transformed 2D vertices through rw_bridge_im2d_render() → D3D8→D3D11
  - Driver entry 0x0F (sub_001DBDE0) also overridden as safety net
  - Auto BeginScene/EndScene management for im2d calls outside 3D render pass
  - Debug HUD overlay: top bar = game state color, bottom bar = pulsing im2d active indicator
- **Im2d callback table populated**: MEM32(0x7592CC) = 0x1E2930 (render triangle), 0x7592D0 = 0x1E2330 (render line)
- **VEH divide-by-zero handler**: Catches EXCEPTION_INT_DIVIDE_BY_ZERO, decodes x86-64 idiv instruction, sets EAX/EDX=0, skips past. Prevents SEH unwinding game loop.
- **Game audio events connected**: AWD sounds trigger on state machine transitions
  - State 7→4: "Zoom" sound, State 4→5: "MenuIn" sound, Boot: "GlobeHigh" chime
  - Audio played through APU software mixer (waveOut 48kHz)
- **recomp_0005.c**: `#if 0` around gen sub_001DBDE0, sub_001DE900

### Session 37 Changes
- **sub_001AA100 overridden** in recomp_manual.c: forces return 1 after 30 frames
  - Enables state 4→5 transition in outer state machine (sub_000165F0)
- **Frontend render path activated**: sub_000171A0 now fires every frame
  - Frontend obj at 0x4D4008, vtable at 0x1C45BA68 (native ptr, not Xbox VA)
- **recomp_0004.c**: `#if 0` around gen sub_001AA100

### Previous Status
Game boots, loads, runs gameplay loop. **Two rendering modes** toggled with V key:
1. **Pseudo-3D** (default): OutRun-style 2.5D rendering (unchanged from Session 21)
2. **True 3D** (V key): Full 3D world-space rendering with chase camera, time-of-day cycling, roadside scenery, tunnels, night stars, rain weather, and 3D vehicle models.

### RenderWare 3D Renderer
- **rw_renderer.c/h**: Full 3D scene renderer using D3D8→D3D11 layer
  - `RW_Camera`, `RW_Mesh`, `RW_Object`, `RW_Scene` data structures
  - Persistent GPU vertex/index buffers (no per-frame upload)
  - Chase camera: follows player car along heading with speed-adaptive distance
  - Procedural road: 80 segments with curve offsets, center dashes, edge lines
  - Ground plane: large grass quad following player position
  - Mountain backdrop: 24-peak ring of triangle silhouettes (time-of-day colored)
  - HUD: speed bar, boost bar, score/multiplier, "3D MODE" indicator
  - **Time-of-day** (NEW Session 24): 4-phase color cycling (dawn→day→sunset→night, 3000-unit period)
    - Sky gradient, ground plane, mountains, road all cycle colors
    - Road darkens at night, building windows light up
  - **Roadside objects** (NEW Session 24): 6 types placed along procedural road
    - Guard posts, trees (regular/tall), buildings (with night windows), road signs, billboards
    - Deterministic hash-based placement, 18-unit spacing, both sides of road
  - **Tunnel sections** (NEW Session 24): every 2000 units, 200 units long
    - Ceiling, walls, orange strip lights, curve-following segments
  - **Night stars** (NEW Session 24): 40 twinkling stars during night phase (cycle 0.55-0.95)
  - **Rain weather** (NEW Session 24): 6000-unit cycle with rain streaks, fog overlay
- **rw_math.h**: Shared math utilities extracted from main.c
  - mat4_identity, mat4_perspective, mat4_lookat, mat4_rotation_y
  - mat4_translation, mat4_scaling, mat4_multiply (new)
- **V key toggle**: switches between pseudo-3D and true 3D at runtime
- Both rendering paths coexist — pseudo-3D code is NOT removed

### Physics Model (sub_000636D0 + sub_000110E0)
- **Heading angle** at fake physics body +0x18 (radians, 0=north, CW positive)
- **Scalar speed** at +0x1C (units/s, max 50 normal / 75 boosting, drag 0.8)
- W/S: forward/reverse acceleration, A/D: steering (speed-dependent)
- Road curves apply centripetal force (0x5FFD10) pushing car outward
- Position integrated: pos += speed * heading_dir * dt

### 3D Vehicle Models (NEW - Session 21)
- **BGV loader** (`bgv_loader.c`): parses Criterion .bgv vehicle geometry files
  - Xbox D3DVSDT_NORMPACKED3 packed normals (11-11-10 bit signed)
  - Triangle strip → triangle list conversion with degenerate restart markers
  - Draw call extraction from sub-entry descriptors (pattern scan)
  - Fake directional lighting baked into vertex colors
- **Vehicle catalog**: 67 models across 7 classes (COMP/CUPE/HEVY/HSPC/MSCL/SPRT/SUPR)
- **3D model viewer** (M key): turntable view with auto-rotation, ground plane, grid
- **Model cycling** (N/P keys): browse all 67 vehicles, window title shows stats
- **Player car**: actual 3D model in gameplay view (off-center projection positioning)
- **Traffic cars**: 6 different 3D models with color tints (red/blue/yellow/green/silver/grey)
- Vehicle textures are in .btv files (paint variants) - format not yet decoded

### D3D8 Rendering (main.c)
- **Pseudo-3D perspective** (OutRun-style): camera behind car, road to horizon
- 50 road segments with accumulated curve AND hill offsets
- Sky gradient with **stars during night phase** (twinkle, color variation)
- Mountain silhouettes with parallax, grass ground plane
- **Time-of-day**: dawn→day→sunset→night color cycling every 3000 world units
- Alternating road stripes, rumble strip edges, yellow center dashes, white lane dividers
- **6 roadside object types**: guard posts, trees (2 sizes), buildings, road signs, billboards
- **Tunnel sections** every 2000 units: ceiling, walls, orange strip lights
- **Rain puddles** on road surface during rain weather
- 12 traffic obstacles (8 same-dir + 4 oncoming) with taillights and headlights
- AI traffic: sine-wave lane drifting + **braking when player approaches**
- Player car with shadow, steering tilt, windshield, taillights, boost exhaust flames
- **Headlight beams** projecting forward during night phase
- **Wall collision sparks**: orange/yellow particle burst
- Speed lines at 25+ speed
- **Rain weather**: diagonal streaks + fog overlay cycling every 6000 units
- Screen shake on crash
- **Rear-view mirror** at top center showing traffic behind player
- HUD: speed bar, boost bar, takedown pips, **score/multiplier bar**, checkpoint banner
- Flash overlay: white=takedown, red=crash, **green=checkpoint**

### Memory Layout
- 0x5FFF00: Fake physics body (+08 accel, +0C turn, +10 px, +14 py, +18 hdg, +1C spd)
- 0x5FFE00: Obstacle array (12 × 16B: pos_x, pos_y, speed, flags)
- 0x5FFD00: Takedown count (uint32)
- 0x5FFD04: Flash timer (float)
- 0x5FFD08: Boost meter 0-100 (float)
- 0x5FFD0C: Boost button state (uint32)
- 0x5FFD10: Road curve at player (float)
- 0x5FFD14: Distance traveled (uint32)
- 0x5FFD18: Screen shake timer (float)
- 0x5FFD1C: Last checkpoint distance (uint32)
- 0x5FFD20: Checkpoint flash timer (float)
- 0x5FFD24: Score (uint32)
- 0x5FFD28: Score multiplier (float, 1.0-8.0)
- 0x5FFD2C: Combo timer (float, resets multiplier decay)
- 0x5FFD30: Spark timer (float)
- 0x5FFD34: Spark side (uint32, 0=left, 1=right)

### Gameplay Features
- Road curves and hills with centripetal physics
- Wall collision: bounce, half speed, sparks, multiplier reset
- TAKEDOWN (same-dir): speed boost + 500pts × multiplier + boost +25
- CRASH (oncoming): 85% speed loss + shake + multiplier reset
- Near-miss: boost fill + 50-100pts × multiplier (continuous)
- Boost: Shift/gamepad drains meter for +50% max speed
- **Score system**: points from near-misses, takedowns, checkpoints
- **Combo multiplier**: 1x-8x, builds with actions, decays when idle, resets on crash/wall
- **Checkpoints** every 500m: green flash + 15 boost + 1000pts × multiplier
- Difficulty ramps with distance (traffic density + speed)
- AI braking when player approaches from behind in same lane

### Session 31: xemu Live Debugging (NEW)
- **xemu GDB stub analysis**: Connected to running game via GDB RSP on port 1234
- **Game state machine discovered**: 0x4D53B8 values: 5=menus AND regular racing, 4=crash mode only, 7=loading
- **Camera pointer as gameplay indicator**: 0x4D5370 == 0x4D45D0 means gameplay (any mode), 0x4D4008 means menus
- **Gameplay detection FIXED**: Changed `B8==4` check to `B8==4 || cam_ptr==0x4D45D0` in both recomp_manual.c and main.c
- **Race state writeback**: Countdown timer (0x411BF8), lap counter (0x411BDC/0x411B30), position (0x550520)
- **Boost meter**: 0x40FD7C is performance accumulator (100→1800+ range), NOT max speed cap
- **Race control block**: 0x411B20 active flag, 0x411BE0 race type, 0x411BE8 total laps
- **Standings array**: Player position at 0x550520, car IDs at 0x550524+ (player = car 8)
- **Career takedowns**: Cumulative gold/silver/bronze at 0x44D14C/0x44D154/0x44D150
- **AI car state**: Base 0x410600, stride 0xF0 (240 bytes/car), contains hit/damage counts
- **Crash physics**: Parts array at 0x5492B0 (80B stride), spreads 3000+ units during explosions
- **Scoring display**: 0x5566E0-0x556708, rewritten per results screen
- Tested across 5 game modes: menus, crash junction, regular race, burning lap, road rage

### Session 30 Progress
- Created **xboxrecomp** toolkit repo (https://github.com/sp00nznet/xboxrecomp)
  - 4 toolchain modules (xbe_parser, disasm, func_id, recomp)
  - 6 pipeline guides, 8 technical deep dives, 3 format references
  - 3 runtime templates (recomp_types.h, xbox_memory.h, kernel_stubs.h)
  - 15,196 lines across 55 files

### Next Steps
1. **Capture push buffers for sub-menus** from xemu (SINGLE EVENT, OPTIONS, etc.) and
   drive PB replay from game's menu selection state for interactive menu navigation
2. **Un-stub sub_0034CBF0** — write manual override to set render state dirty flags
   (surface+0x80000, device+0x1A04/1A08 swap) without D3D pointer chain walks,
   enabling sub_0034D530_gen to flush actual vertex data
3. Decode .btv vehicle texture format and apply to 3D models
4. Long-term: fix the real physics world initialization

### Key Input Addresses
- Accumulators: 0x4D652C (throttle), 0x4D6530 (steering) - written by game_frame_pump()
- Car object: 0x557880 (esi in sub_000636D0), +0x1B4 → velocity ptr (= 0x5FFF00)
- Boost button: 0x5FFD0C (from VK_SHIFT or gamepad A/RB in game_frame_pump)
- Button events: 0x4A1C74-0x4A1C79 (processed by sub_00013F10)

### Controls
- **Keyboard**: WASD = drive, Shift = boost, ESC = quit
- **V key**: toggle true 3D rendering mode (chase camera)
- **T key**: cycle through 37 tracks (loads track geometry in 3D mode)
- **M key**: toggle 3D model viewer (turntable view)
- **N/P keys**: next/previous vehicle model (in model viewer)
- **Gamepad**: Left stick = steer, RT/LT = gas/brake, A or RB = boost

### Regenerating gen/ (2026-07-16: no longer a manual checklist)

The whole pipeline is reproducible now. From the repo root:

```
py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe"        # NOT --text-only
py -3 tools/ghidra_naming/merge_names.py --names-json tools/ghidra_naming/xdk_names.json --apply
py -3 -m tools.func_id "Burnout 3 Takedown/default.xbe"
py -3 -m tools.recomp "Burnout 3 Takedown/default.xbe" --all --split 1000 --exclude-manual
py -3 tools/recomp/gen_dangling_stubs.py
cmake -S . -B build && cmake --build build --config Release
```

`--exclude-manual` reads recomp_manual.c and declares-but-does-not-define every
function it overrides, which is what the old `#if 0` patches did by hand. Do not
hand-patch gen/ — it is gitignored and regenerable, and hand edits are lost on
the next run. If gen needs a change, change the recompiler.

**Never run tools.disasm with --text-only.** It skips the XDK sections (D3D,
DSOUND, XNET, XONLINE, XPP, WMADEC, XGRPH, XMV), and with no function
boundaries there the disassembler glues each function to the next — which is
where the mythical "79KB D3D8LTCG giants" came from. sub_0034D530 was recorded
as 79,140 bytes with zero instructions parsed; it is really 533 bytes, and it
is D3DDevice_SetViewport.

### Gen File Patches (HISTORICAL — superseded by --exclude-manual)

Kept only as a record of what used to be re-applied by hand. Most entries are
`#if 0` around a function recomp_manual.c overrides; --exclude-manual now does
that automatically (59 functions on the last run). The pre-re-lift tree is in
src/game/recomp/gen.pre_relift_backup/ if any of this needs recovering.
1. **recomp_0000.c**: extern g_tick_110e0_count, sub_000165F0 entry/ESP traces, sub_00015570 vtable guard, sub_0003D9E0 #if 0, **sub_000636D0 #if 0**, jump table→C switch (replace_all), state traces, exit path traces, case 3 traces, **screen list init PATCH before sub_0001F7C0** (zero idx/count at MEM32(0x4A1E94)+0x10+0x04/0x08), **19× screen entry invalidation disable** (comment out `MEM8(esi+0x1E)=LO8(ebx)` + `MEM32(edi+8)--`)
2. **recomp_0002.c**: #if 0 around sub_00135040, sub_00135240
3. **recomp_0003.c**: extern g_tick_110e0_count, flag clear, ESP+callee-saved save/restore, game loop traces
4. **recomp_0004.c**: #if 0 around sub_001CFDD0, sub_001BEFF0, sub_001C1670, sub_001C1740, sub_001C66F0, sub_001AA100, **sub_001AE6F0**; vtable guards in sub_001B4170, sub_001B41F0, **sub_001AEE20 vtable guard rewritten** (native ptr conversion, skip-without-invalidate)
5. **recomp_0005.c**: #if 0 around 35 functions + sub_001DBDE0, sub_001DE900
6. **recomp_0006.c**: #if 0 around sub_001FE1E0, sub_00221F20
7. **recomp_0007.c**: #if 0 around sub_00244C51, sub_00249B7C, sub_00249B9C
8. **recomp_0022.c**: #if 0 around sub_00351770, sub_003518E0, sub_0034D530, **sub_0034D410, sub_0034F5B0, sub_003558A0** (Session 42)
12. **recomp_stubs.c**: sub_00355F50 stub removed (now generated as recomp_355f50.c)
13. **recomp_355f50.c**: NEW FILE — generated D3D8LTCG dirty flag processor (8005 lines)
    - Added 0x355F50 to tools/disasm/output/functions.json first
    - Generated: `py -3 -m tools.recomp "Burnout 3 Takedown/default.xbe" -f 0x355F50 > src/game/recomp/gen/recomp_355f50.c`
    - Added headers: `#define RECOMP_GENERATED_CODE`, `#include "recomp_funcs.h"`, etc.
    - Must `cmake -S . -B build` to pick up new file (GLOB pattern)
14. **recomp_0000.c**: #if 0 around sub_0003FEE0 (Session 42)
9. **recomp_stubs.c**: #if 0 around sub_00351A20
10. **recomp_dispatch.c**: add sub_001D1818/sub_001D2793 entries, size=22097
11. **recomp_funcs.h**: add sub_001D1818/sub_001D2793 declarations

### Manual Function Overrides (recomp_manual.c) - 40 functions
Including sub_000636D0 (physics force), sub_0003D9E0 (render orchestrator stub), sub_000110E0 (frame pump), sub_001AA100 (full phase state machine 1-9→0x13), sub_001AE6F0 (frontend render dispatch), sub_001DE900 (im2d render → bridge), sub_001DBDE0 (im2d driver entry → bridge), **sub_0003FEE0** (RW frame render, Session 42), **sub_0034F5B0, sub_003558A0, sub_0034D410** (D3D8LTCG mid-entry stubs, Session 42)
