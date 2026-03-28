/**
 * netplay_game_template.c — Template for game-specific netplay hooks.
 *
 * Copy this file into your decomp's src/netplay/ directory and rename it
 * to netplay_yourgame.c. Then fill in the 4 callback functions with your
 * game's specific globals and config format.
 *
 * You also need a thin netplay.h and netplay.c wrapper — see the MK64
 * implementation for reference.
 */

#include <ultra64.h>
#include "netplay_core.h"
#include "netlib.h"

// TODO: Include your game's headers here
// #include "your_game_main.h"
// #include "your_game_rng.h"

/*********************************
      Config Packet Parsing
*********************************/

/**
 * Parse game config from a SERVER_CONFIG packet.
 *
 * Design your payload format to include everything needed to set up
 * a match: game mode, level/course, difficulty, character selections,
 * RNG seed, and input delay.
 *
 * The format must match what netplay_game_send_config() writes and
 * what GamePacketHandler.java on the server relays.
 *
 * Example payload: [mode:1][level:2][chars:N][rng_seed:4][input_delay:1]
 */
void netplay_game_parse_config(size_t size) {
    (void)size;

    // TODO: Read your game's config fields from the packet
    // uint8_t mode;
    // uint16_t level;
    // uint32_t seed;
    // uint8_t delay;
    //
    // netlib_readbyte(&mode);
    // netlib_readword(&level);
    // netlib_readdword(&seed);
    // netlib_readbyte(&delay);
    //
    // gNetplayState.cfgMode = mode;
    // gNetplayState.cfgCourse = (s16)level;
    // gNetplayState.rngSeed = seed;
    // gNetplayState.inputDelay = (delay > NP_INPUT_DELAY_MAX) ? NP_INPUT_DELAY_MAX : delay;
    // gNetplayState.configReceived = TRUE;
}

/*********************************
     Apply Config to Game
*********************************/

/**
 * Apply network config to your game's globals.
 *
 * This is called from your match/race setup function when netplay
 * is active. It should:
 * 1. Set player count from gNetplayState.playerCount
 * 2. Set screen mode from gNetplayState.localPlayerCount
 * 3. Set game mode, level, characters, difficulty from cfg* fields
 */
void netplay_game_setup(void) {
    // TODO: Wait for config if using NetLib mode
    // if (gNetplayState.mode == NP_MODE_NETLIB && !gNetplayState.configReceived) {
    //     s32 i;
    //     for (i = 0; i < 60; i++) {
    //         netlib_poll();
    //         if (gNetplayState.configReceived) break;
    //     }
    // }

    // TODO: Apply to your game's globals
    // your_player_count = gNetplayState.playerCount;
    //
    // switch (gNetplayState.localPlayerCount) {
    //     case 1: your_screen_mode = FULLSCREEN; break;
    //     case 2: your_screen_mode = SPLIT_2P; break;
    //     default: your_screen_mode = SPLIT_4P; break;
    // }
    //
    // your_game_mode = gNetplayState.cfgMode;
    // your_level_id = gNetplayState.cfgCourse;
}

/*********************************
     RNG Synchronization
*********************************/

/**
 * Seed your game's RNG from the network-synchronized seed.
 * Called at match start to ensure deterministic gameplay.
 */
void netplay_game_seed_rng(void) {
    // TODO: Set your game's RNG seed
    // If SC64 SHM mode, re-read from shared memory first:
    // if (gNetplayState.mode == NP_MODE_SC64_SHM) {
    //     gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);
    // }
    //
    // your_rng_seed = (u16)(gNetplayState.rngSeed & 0xFFFF);
}

/*********************************
    Host Config Broadcast
*********************************/

/**
 * Host (player 0) sends current menu selections to the server.
 * Format must match netplay_game_parse_config().
 */
void netplay_game_send_config(void) {
    // TODO: Only host sends
    // if (gNetplayState.localPlayer != 0) return;
    //
    // if (gNetplayState.mode == NP_MODE_NETLIB) {
    //     u32 seed = (u32)osGetCount();
    //     gNetplayState.rngSeed = seed;
    //
    //     netlib_start(PKTID_GAME_CONFIG);
    //     netlib_writebyte((uint8_t)your_game_mode);
    //     netlib_writeword((uint16_t)your_level_id);
    //     netlib_writedword(seed);
    //     netlib_writebyte((uint8_t)gNetplayState.inputDelay);
    //     netlib_sendtoserver();
    // }
}
