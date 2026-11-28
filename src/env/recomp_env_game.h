/*
 * recomp_env_game.h - Burnout 3's keys and defaults for the fork toolkit's
 * environment table (xboxrecomp src/platform/recomp_env.h includes this file when
 * RECOMP_ENV_HAVE_GAME_KEYS is defined; CMakeLists.txt sets it on the
 * recomp_env target when the toolkit has one). Upstream's toolkit reads raw
 * getenv and has no such target, so there main.c's _putenv_s defaults apply.
 *
 * Defaults are D(ID, "value"), used when neither the key nor its old variable
 * is set in the environment.
 *   VBLANK      D3D's present waits on the vblank event the GPU ISR (vector 3)
 *               signals; without vblank delivery the boot stops after flip 1.
 *               paced: the 60 Hz schedule with the KickOff ack watch; since
 *               the toolkit's v0.16 sync "1" is upstream's plain tick.
 *   GUEST_CPUS  left at the toolkit's default, every core, as upstream's
 *               (the user, 2026-11-05, as BLiNX 2 in the tracker item). The game set
 *               "one" (as on the console) from BLiNX 2's default when the
 *               toolkit went title-neutral, never for a race of its own; on
 *               native Windows the pin starved BLiNX 2's movie-audio
 *               workers on the one core. RECOMP_GUEST_CPUS=one is the A/B.
 *   DISPATCH_GATE  1: the one-CPU DISPATCH gate, so device interrupts and
 *               queued DPCs run at the guest holder's safe points and never
 *               beside its guest code, as on the console's one CPU.
 *   FLIP_PACING 1: the pushbuffer walker and the KickOff ack hold after a
 *               flip until the guest vblank reaches the swap's interval, as
 *               the NV2A's FLIP_STALL does.
 *               The gate and flip pacing were the fork toolkit's fixed
 *               behaviour before v0.16; upstream made both opt-in.
 *   USB_PADS    two pads: the split-screen modes need a second controller.
 * The DSOUND GP doorbell ack (apu_dsp_ack=auto: SGE[0] of NV_PAPU_GPSADDR
 * + 0x810, wherever the contiguous allocator put it; the upstream allocator
 * gave 0x825F8810, the fork's 0x80BBC810) was a default here until the fork
 * toolkit made `auto` its own default.
 */
#ifndef RECOMP_ENV_GAME_H
#define RECOMP_ENV_GAME_H

/* This game's own keys, added to the toolkit's table (same row format:
 * X(ID, TIER, NAME, LEGACY, HELP); rows in docs/running.md).
 * RECOMP_GAME_FILES and RECOMP_HDD_DIR, which the packaged launchers set
 * because they start the game from its logs folder, are toolkit rows since
 * b3-public-release (every game's launcher sets them), and the toolkit then
 * defines RECOMP_ENV_HAVE_FOLDER_ROWS. A key defined twice does not compile,
 * so the two rows stay here for an older toolkit (the public tag the public
 * game.toml pins); drop them once the pin is past the toolkit merge. main.c
 * holds their defaults, upstream's folders, either way.
 *
 * The enhancements layer's keys (RECOMP_CONFIG, RECOMP_RENDER_SCALE,
 * RECOMP_PRESENT_*, ...) are the toolkit's own rows: CMakeLists.txt turns
 * XBOXRECOMP_ENHANCE on, which defines RECOMP_ENV_HAVE_ENHANCE_KEYS on the
 * recomp_env target and adds them to the table. A key of this game's own
 * for the layer (cat's fps.mode is one) would go here as a CONFIG row named
 * RECOMP_ + the dotted key, read in main.c's host_enhance_init. */
#ifndef RECOMP_ENV_HAVE_FOLDER_ROWS
#define RECOMP_ENV_GAME_KEYS(X) \
    X(GAME_FILES, CONFIG, "RECOMP_GAME_FILES", NULL, "game files directory (default: Burnout 3 Takedown in the working directory)") \
    X(HDD_DIR,    CONFIG, "RECOMP_HDD_DIR",    NULL, "emulated hard disk: partition images, saves (default: saves in the working directory)")
#else
#define RECOMP_ENV_GAME_KEYS(X)
#endif

/* DISPATCH_GATE and FLIP_PACING are toolkit rows since its v0.16 sync
 * (which also defines RECOMP_ENV_HAVE_RUMBLE_ROW); a toolkit before it has
 * neither and already runs this way, so they are left out there. Drop the
 * guard once the pin is past that merge. */
#ifdef RECOMP_ENV_HAVE_RUMBLE_ROW
#define B3_ENV_ONE_CPU_DEFAULTS(D) \
    D(DISPATCH_GATE, "1") \
    D(FLIP_PACING,   "1")
#else
#define B3_ENV_ONE_CPU_DEFAULTS(D)
#endif

/* RUN_AHEAD 0: the KickOff lockstep. With the toolkit's default run-ahead
 * (64) the race_gate route stops flipping at flip ~5100-5245, just
 * after the front end opens _FEMain.xmv, twice in two runs on native Windows
 * D3D11, while the vblanks go on; every run-ahead counter stays 0 (no
 * checkpoint or notify), so it is the flip hold or the ack-thread pass, not
 * the early ack. The lockstep runs the route to the end. Follow-up:
 * The tracker item; RECOMP_DEBUG=run_ahead=64 is the A/B. Tested first, since the
 * public pin may predate the default. */
#ifdef RECOMP_ENV_HAVE_RUN_AHEAD_DEFAULT
#define B3_ENV_RUN_AHEAD_DEFAULT(D) D(RUN_AHEAD, "0")
#else
#define B3_ENV_RUN_AHEAD_DEFAULT(D)
#endif

#define RECOMP_ENV_GAME_DEFAULTS(D) \
    D(VBLANK,        "paced") \
    B3_ENV_ONE_CPU_DEFAULTS(D) \
    B3_ENV_RUN_AHEAD_DEFAULT(D) \
    D(USB_PADS,      "2")

#endif /* RECOMP_ENV_GAME_H */
