/*
 * recomp_env_game.h - Burnout 3's keys and defaults for the fork toolkit's
 * environment table (xboxrecomp src/platform/recomp_env.h includes this file when
 * RECOMP_ENV_HAVE_GAME_KEYS is defined; CMakeLists.txt sets it on the
 * recomp_env target when the toolkit has one). Upstream's toolkit reads raw
 * getenv and has no such target, so there main.c's own defaults apply.
 *
 * Defaults are D(ID, "value"), used when neither the key nor its old variable
 * is set in the environment.
 *   VBLANK      D3D's present waits on the vblank event the GPU ISR (vector 3)
 *               signals; without vblank delivery the boot stops after flip 1.
 *   GUEST_CPUS  one: every guest thread on one host core, as on the console
 *               (the toolkit's own default too; stated here so this game's
 *               pinning does not depend on it). RECOMP_GUEST_CPUS=all is
 *               the A/B.
 *   USB_PADS    two pads: the split-screen modes need a second controller.
 * The DSOUND GP doorbell ack needs no default: both toolkits find it
 * (SGE[0] of NV_PAPU_GPSADDR + 0x810) wherever the allocator put it.
 */
#ifndef RECOMP_ENV_GAME_H
#define RECOMP_ENV_GAME_H

/* This game's own keys, added to the toolkit's table (same row format:
 * X(ID, TIER, NAME, LEGACY, HELP); rows in docs/running.md).
 * RECOMP_GAME_FILES and RECOMP_HDD_DIR, which the packaged launchers set
 * because they start the game from its logs folder, are toolkit rows on
 * the fork's blinx2-v0.4.0 and newer, which then define
 * RECOMP_ENV_HAVE_FOLDER_ROWS; a key defined twice does not compile, so
 * the rows here are only for an older fork toolkit. main.c reads both
 * with plain getenv on every toolkit, and holds their defaults.
 *
 * The enhancements layer's keys (RECOMP_CONFIG, RECOMP_RENDER_SCALE,
 * RECOMP_PRESENT_*, ...) are the toolkit's own rows: CMakeLists.txt turns
 * XBOXRECOMP_ENHANCE on, which defines RECOMP_ENV_HAVE_ENHANCE_KEYS on the
 * recomp_env target and adds them to the table. A key of this game's own
 * for the layer would go here as a CONFIG row named RECOMP_ + the dotted
 * key, read in main.c's host_enhance_init. */
#ifndef RECOMP_ENV_HAVE_FOLDER_ROWS
#define RECOMP_ENV_GAME_KEYS(X) \
    X(GAME_FILES, CONFIG, "RECOMP_GAME_FILES", NULL, "game files directory (default: Burnout 3 Takedown in the working directory)") \
    X(HDD_DIR,    CONFIG, "RECOMP_HDD_DIR",    NULL, "emulated hard disk: partition images, saves (default: saves in the working directory)")
#else
#define RECOMP_ENV_GAME_KEYS(X)
#endif

#define RECOMP_ENV_GAME_DEFAULTS(D) \
    D(VBLANK,     "1") \
    D(GUEST_CPUS, "one") \
    D(USB_PADS,   "2")

#endif /* RECOMP_ENV_GAME_H */
