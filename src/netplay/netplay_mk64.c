/**
 * netplay_mk64.c — MK64-specific netplay hooks.
 *
 * Implements the four game-specific callbacks required by netplay_core:
 *   netplay_game_parse_config()  — read config from NetLib packet
 *   netplay_game_setup()         — apply config to MK64 globals
 *   netplay_game_seed_rng()      — seed MK64's LFSR RNG
 *   netplay_game_send_config()   — host sends config from menu selections
 *
 * To adapt for another game, copy this file and change the game-specific
 * globals (gPlayerCount, gModeSelection, gCharacterSelections, etc.)
 * to match your decomp's variable names.
 */

#include <ultra64.h>
#include <defines.h>
#include <course.h>

#include "netplay_core.h"
#include "netlib.h"
#include "main.h"
#include "menus.h"
#include "buffers/random.h"

/*********************************
      Config Packet Parsing
*********************************/

/**
 * Parse game config from a SERVER_CONFIG packet.
 * Called by netplay_core when the packet arrives from the server.
 *
 * MK64 config payload:
 *   [mode:1][course:2][cc:1][char0..char7:8][rng_seed:4][input_delay:1]
 */
void netplay_game_parse_config(size_t size) {
    uint8_t mode, cc, delay, ch;
    uint16_t course;
    uint32_t seed;
    s32 i;
    (void)size;

    netlib_readbyte(&mode);
    netlib_readword(&course);
    netlib_readbyte(&cc);
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        netlib_readbyte(&ch);
        gNetplayState.cfgCharacters[i] = (s8)ch;
    }
    netlib_readdword(&seed);
    netlib_readbyte(&delay);

    gNetplayState.cfgMode = mode;
    gNetplayState.cfgCourse = (s16)course;
    gNetplayState.cfgCC = cc;
    gNetplayState.rngSeed = seed;
    gNetplayState.inputDelay = (delay > NP_INPUT_DELAY_MAX) ? NP_INPUT_DELAY_MAX : delay;
    gNetplayState.configReceived = TRUE;
}

/*********************************
     Apply Config to Game
*********************************/

/**
 * Apply network config to MK64 game globals.
 * Called from setup_race() when netplay is active.
 */
void netplay_game_setup(void) {
    s32 i;

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        // Wait for config from server if not yet received
        if (!gNetplayState.configReceived) {
            for (i = 0; i < 60; i++) {
                netlib_poll();
                if (gNetplayState.configReceived) break;
            }
        }
    } else if (gNetplayState.mode == NP_MODE_SC64_SHM) {
        // Read config from SC64 shared memory registers
        u32 charId;
        gNetplayState.cfgMode = (u8)np_pi_read(NP_REG_GAME_MODE);
        gNetplayState.cfgCourse = (s16)np_pi_read(NP_REG_COURSE_ID);
        gNetplayState.cfgCC = (u8)np_pi_read(NP_REG_CC_SELECT);
        for (i = 0; i < NP_MAX_PLAYERS; i++) {
            charId = np_pi_read(NP_REG_CHAR_SEL_0 + (i * 4));
            gNetplayState.cfgCharacters[i] = (charId <= BOWSER) ? (s8)charId : 0;
        }
        gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);
        np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY | NP_STATUS_MENU_READY);
    }

    // --- Apply to MK64 globals ---

    // Total session players (all consoles)
    gPlayerCount = gNetplayState.playerCount;
    gPlayerCountSelection1 = gNetplayState.playerCount;

    // Screen mode based on LOCAL player count (this console's split-screen)
    switch (gNetplayState.localPlayerCount) {
        case 1:
            gScreenModeSelection = SCREEN_MODE_1P;
            break;
        case 2:
            gScreenModeSelection = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;
            break;
        default:
            gScreenModeSelection = SCREEN_MODE_3P_4P_SPLITSCREEN;
            break;
    }
    gActiveScreenMode = gScreenModeSelection;

    // Game mode
    gModeSelection = gNetplayState.cfgMode;
    if (gModeSelection < GRAND_PRIX || gModeSelection > BATTLE) {
        gModeSelection = VERSUS;
    }

    // CC
    gCCSelection = gNetplayState.cfgCC;
    if (gCCSelection < CC_50 || gCCSelection > CC_150) {
        gCCSelection = CC_100;
    }

    // Character selections
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        if (gNetplayState.cfgCharacters[i] >= 0 && gNetplayState.cfgCharacters[i] <= BOWSER) {
            gCharacterSelections[i] = gNetplayState.cfgCharacters[i];
        }
    }

    // Course
    gCurrentCourseId = gNetplayState.cfgCourse;
}

/*********************************
     RNG Synchronization
*********************************/

/**
 * Seed MK64's 16-bit LFSR RNG from the network-synchronized seed.
 */
void netplay_game_seed_rng(void) {
    if (gNetplayState.mode == NP_MODE_SC64_SHM) {
        gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);
    }
    gRandomSeed16 = (u16)(gNetplayState.rngSeed & 0xFFFF);
}

/*********************************
    Host Config Broadcast
*********************************/

/**
 * Host (player 0) sends current menu selections to the server.
 * Server relays to all connected players via PKTID_SERVER_CONFIG.
 *
 * MK64 config payload:
 *   [mode:1][course:2][cc:1][char0..char7:8][rng_seed:4][input_delay:1]
 */
void netplay_game_send_config(void) {
    s32 i;
    u32 seed;

    if (gNetplayState.localPlayer != 0) {
        return; // Only host sends
    }

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        seed = (u32)osGetCount();
        gNetplayState.rngSeed = seed;

        netlib_start(PKTID_GAME_CONFIG);
        netlib_writebyte((uint8_t)gModeSelection);
        netlib_writeword((uint16_t)gCurrentCourseId);
        netlib_writebyte((uint8_t)gCCSelection);
        for (i = 0; i < NP_MAX_PLAYERS; i++) {
            netlib_writebyte((uint8_t)gCharacterSelections[i]);
        }
        netlib_writedword(seed);
        netlib_writebyte((uint8_t)gNetplayState.inputDelay);
        netlib_sendtoserver();
    }
}
