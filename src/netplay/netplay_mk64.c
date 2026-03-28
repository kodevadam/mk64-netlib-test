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
#include <common_structs.h>

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
   Extended Controller Update
*********************************/

/**
 * Separate Controller structs for network players 5-8 (slots 4-7).
 *
 * gControllers[4..7] can't be used because MK64 repurposes them:
 *   gControllers[4] = gControllerFive = OR of all physical controllers
 *   gControllers[5] = gControllerSix  = time trial ghost 1
 *   gControllers[6] = gControllerSeven = time trial ghost 2
 *   gControllers[7] = gControllerEight = time trial replay
 *
 * So we maintain our own array. handle_a_press_for_player_during_race()
 * can accept any Controller* — it doesn't have to be from gControllers[].
 */
struct Controller gNetplayExtControllers[4]; // For slots 4, 5, 6, 7

/**
 * Get the Controller struct for a given player slot.
 * Slots 0-3: gControllers[slot] (standard).
 * Slots 4-7: gNetplayExtControllers[slot-4] (netplay extended).
 */
struct Controller* netplay_get_controller(s32 slot) {
    if (slot < 4) {
        return &gControllers[slot];
    }
    if (slot < 8) {
        return &gNetplayExtControllers[slot - 4];
    }
    return &gControllers[0]; // fallback
}

/**
 * Update extended controllers for network players in slots 4-7.
 * Writes packed remote inputs into gNetplayExtControllers[], computing
 * pressed/depressed/stick the same way update_controller() does.
 *
 * Call once per frame after read_controllers() and netplay_update().
 */
void netplay_update_extended_controllers(void) {
    s32 i;
    u32 packed;
    struct Controller *ctrl;
    u16 newButtons, stick;

    if (!gNetplayState.enabled || gNetplayState.playerCount <= 4) {
        return;
    }

    for (i = 4; i < gNetplayState.playerCount && i < NP_MAX_PLAYERS; i++) {
        ctrl = &gNetplayExtControllers[i - 4];

        if (gNetplayState.disconnectMask & (1 << i)) {
            memset(ctrl, 0, sizeof(struct Controller));
            continue;
        }

        if (!(gNetplayState.ctrlMask & (1 << i))) {
            continue;
        }

        // Unpack remote input
        packed = gNetplayState.remoteInputs[i];
        newButtons = NP_UNPACK_BUTTONS(packed);

        ctrl->rawStickX = (s16)NP_UNPACK_STICK_X(packed);
        ctrl->rawStickY = (s16)NP_UNPACK_STICK_Y(packed);

        // Compute pressed/depressed (same logic as update_controller)
        ctrl->buttonPressed = newButtons & (newButtons ^ ctrl->button);
        ctrl->buttonDepressed = ctrl->button & (newButtons ^ ctrl->button);
        ctrl->button = newButtons;

        // Compute stick direction (same thresholds as update_controller)
        stick = 0;
        if (ctrl->rawStickX < -50) stick |= L_JPAD;
        if (ctrl->rawStickX > 50)  stick |= R_JPAD;
        if (ctrl->rawStickY < -50) stick |= D_JPAD;
        if (ctrl->rawStickY > 50)  stick |= U_JPAD;
        ctrl->stickPressed = stick & (stick ^ ctrl->stickDirection);
        ctrl->stickDepressed = ctrl->stickDirection & (stick ^ ctrl->stickDirection);
        ctrl->stickDirection = stick;
    }
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
/*********************************
     Camera / Player Remap
*********************************/

/**
 * Remap the camera→player pointers so that local viewports follow
 * the correct network player slots.
 *
 * MK64 hardcodes: camera1 → gPlayerOneCopy (gPlayers[0])
 *                 camera2 → gPlayerTwoCopy (gPlayers[1])
 *
 * For netplay, if our local player is slot 3 in 1P mode, we need
 * camera1 to follow gPlayers[3]. We achieve this by remapping the
 * gPlayerOneCopy/gPlayerTwoCopy pointers.
 *
 * Call this after setup_race()/spawn_players completes.
 */
/**
 * After spawn_players runs, upgrade remote netplay players from CPU to HUMAN.
 *
 * MK64 spawns players 5-8 as CPU with hardcoded characters. For 8-player
 * netplay, we need those slots to be HUMAN-controlled (via network input)
 * with the correct character selections from the game config.
 *
 * This also fixes character assignments for all remote players since
 * spawn_players may have assigned wrong characters for slots > 4.
 */
static void netplay_fixup_remote_players(void) {
    s32 i;
    Player *players[NP_MAX_PLAYERS];

    players[0] = gPlayerOne;
    players[1] = gPlayerTwo;
    players[2] = gPlayerThree;
    players[3] = gPlayerFour;
    players[4] = gPlayerFive;
    players[5] = gPlayerSix;
    players[6] = gPlayerSeven;
    players[7] = gPlayerEight;

    for (i = 0; i < gNetplayState.playerCount && i < NP_MAX_PLAYERS; i++) {
        if (!(players[i]->type & PLAYER_EXISTS)) {
            continue;
        }
        // If this slot is a network player (local or remote), make it HUMAN
        if (netplay_is_local_player(i) || (gNetplayState.ctrlMask & (1 << i))) {
            // Clear CPU flag, set HUMAN flag
            players[i]->type &= ~PLAYER_CPU;
            players[i]->type |= PLAYER_HUMAN;
        }
    }
}

void netplay_remap_cameras(void) {
    s32 i;
    Player** copyPtrs[2];

    if (!gNetplayState.enabled) {
        return;
    }

    // First, upgrade remote netplay players from CPU to HUMAN
    netplay_fixup_remote_players();

    // Remap camera→player pointers so viewports follow local slots.
    // gPlayerOneCopy is what camera1 / render_player_one_*() follows.
    // gPlayerTwoCopy is what camera2 / render_player_two_*() follows.
    copyPtrs[0] = &gPlayerOneCopy;
    copyPtrs[1] = &gPlayerTwoCopy;

    for (i = 0; i < gNetplayState.localPlayerCount && i < 2; i++) {
        u8 slot = gNetplayState.localSlots[i];
        if (slot < NUM_PLAYERS) {
            *copyPtrs[i] = &gPlayers[slot];
        }
    }
}

/*********************************
    Host Config Broadcast
*********************************/

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
