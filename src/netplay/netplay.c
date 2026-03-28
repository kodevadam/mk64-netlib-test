#include <ultra64.h>
#include <PR/os.h>
#include <string.h>
#include <defines.h>
#include <course.h>

#include "netplay.h"
#include "main.h"
#include "menus.h"
#include "buffers/random.h"

/*********************************
            Globals
*********************************/

NetplayState gNetplayState;

/*********************************
     SC64 PI Bus Access
*********************************/

static u32 np_pi_read(u32 offset) {
    u32 value = 0;
    osPiReadIo(NP_SHM_BASE + offset, &value);
    return value;
}

static void np_pi_write(u32 offset, u32 value) {
    osPiWriteIo(NP_SHM_BASE + offset, value);
}

/*********************************
        Initialization
*********************************/

void netplay_init(void) {
    s32 i;
    memset(&gNetplayState, 0, sizeof(NetplayState));
    gNetplayState.mode = NP_MODE_DISABLED;
    gNetplayState.enabled = FALSE;
    gNetplayState.localPlayer = 0;
    gNetplayState.playerCount = 1;
    gNetplayState.ctrlMask = 0;
    gNetplayState.frameCounter = 0;
    gNetplayState.rngSeed = 0;
    gNetplayState.inputDelay = 0;
    gNetplayState.inputDelayHead = 0;
    gNetplayState.disconnectMask = 0;
    gNetplayState.raceStartSync = NP_RACE_NOT_STARTED;
    gNetplayState.inRace = FALSE;
    for (i = 0; i < NP_INPUT_DELAY_MAX; i++) {
        gNetplayState.inputDelayBuffer[i] = 0;
    }
}

/**
 * Detect if SC64 bridge is connected by checking for magic value
 * in shared memory. Returns 1 if bridge is detected, 0 otherwise.
 */
s32 netplay_detect(void) {
    u32 magic;

    magic = np_pi_read(NP_REG_MAGIC);

    if (magic == NP_MAGIC) {
        gNetplayState.mode = NP_MODE_SC64_SHM;
        gNetplayState.enabled = TRUE;

        // Read configuration from bridge
        gNetplayState.localPlayer = (u8)np_pi_read(NP_REG_LOCAL_PLAYER);
        gNetplayState.playerCount = (u8)np_pi_read(NP_REG_PLAYER_COUNT);
        gNetplayState.ctrlMask = (u8)np_pi_read(NP_REG_CTRL_MASK);
        gNetplayState.inputDelay = (u8)np_pi_read(NP_REG_INPUT_DELAY);
        gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);

        // Clamp input delay
        if (gNetplayState.inputDelay > NP_INPUT_DELAY_MAX) {
            gNetplayState.inputDelay = NP_INPUT_DELAY_MAX;
        }

        // Signal to bridge that N64 is ready
        np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY);
        np_pi_write(NP_REG_ENABLE, 1);

        return TRUE;
    }

    gNetplayState.mode = NP_MODE_DISABLED;
    gNetplayState.enabled = FALSE;
    return FALSE;
}

/*********************************
       Per-Frame Update
*********************************/

void netplay_update(void) {
    s32 i;

    if (!gNetplayState.enabled) {
        return;
    }

    gNetplayState.frameCounter++;
    np_pi_write(NP_REG_FRAME, gNetplayState.frameCounter);

    // Read remote controller overrides
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        if (i != gNetplayState.localPlayer && (gNetplayState.ctrlMask & (1 << i))) {
            gNetplayState.remoteInputs[i] = np_pi_read(NP_REG_OVERRIDE_0 + (i * 4));
        }
    }

    // Re-read dynamic state from bridge
    gNetplayState.ctrlMask = (u8)np_pi_read(NP_REG_CTRL_MASK);
    gNetplayState.disconnectMask = (u8)np_pi_read(NP_REG_DISCONNECT);

    // Update race status to bridge
    if (gNetplayState.inRace) {
        np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY | NP_STATUS_IN_RACE);
    }
}

/*********************************
     Controller Override
*********************************/

/**
 * Apply netplay controller overrides to the raw OSContPad data.
 * The local player's physical controller (always port 0) gets mapped
 * to their assigned player slot. Remote/disconnected slots are filled
 * from shared memory or with neutral inputs.
 */
void netplay_apply_controller_overrides(OSContPad *pads) {
    OSContPad localPad;
    s32 i;
    u32 packed;
    u32 delayedInput;

    if (!gNetplayState.enabled) {
        return;
    }

    // Save local player's physical input (always from port 0)
    localPad = pads[0];

    // Pack and send local input to bridge
    netplay_send_local_input(localPad.button, localPad.stick_x, localPad.stick_y);

    // If input delay is active, buffer the local input
    packed = NP_PACK_INPUT(localPad.button, localPad.stick_x, localPad.stick_y);
    if (gNetplayState.inputDelay > 0) {
        // Write current input to ring buffer
        gNetplayState.inputDelayBuffer[gNetplayState.inputDelayHead] = packed;
        gNetplayState.inputDelayHead = (gNetplayState.inputDelayHead + 1) % NP_INPUT_DELAY_MAX;

        // Read delayed input from buffer
        i = (gNetplayState.inputDelayHead + NP_INPUT_DELAY_MAX - gNetplayState.inputDelay) % NP_INPUT_DELAY_MAX;
        delayedInput = gNetplayState.inputDelayBuffer[i];
    } else {
        delayedInput = packed;
    }

    // Fill all player slots
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        if (i == gNetplayState.localPlayer) {
            // This is our slot - use (possibly delayed) physical controller input
            pads[i].button = NP_UNPACK_BUTTONS(delayedInput);
            pads[i].stick_x = NP_UNPACK_STICK_X(delayedInput);
            pads[i].stick_y = NP_UNPACK_STICK_Y(delayedInput);
            pads[i].errno = 0;
        } else if (gNetplayState.disconnectMask & (1 << i)) {
            // Disconnected player - neutral input
            pads[i].button = 0;
            pads[i].stick_x = 0;
            pads[i].stick_y = 0;
            pads[i].errno = 0;
        } else if (gNetplayState.ctrlMask & (1 << i)) {
            // Remote player - unpack override data
            packed = gNetplayState.remoteInputs[i];
            pads[i].button = NP_UNPACK_BUTTONS(packed);
            pads[i].stick_x = NP_UNPACK_STICK_X(packed);
            pads[i].stick_y = NP_UNPACK_STICK_Y(packed);
            pads[i].errno = 0;
        } else {
            // Slot not in use
            pads[i].button = 0;
            pads[i].stick_x = 0;
            pads[i].stick_y = 0;
            pads[i].errno = 0;
        }
    }
}

/*********************************
        Frame Sync
*********************************/

s32 netplay_wait_for_remote_inputs(void) {
    s32 timeout;
    u32 bridgeFrame;

    if (!gNetplayState.enabled || !gNetplayState.inRace) {
        return TRUE;
    }

    for (timeout = 0; timeout < NP_FRAME_SYNC_TIMEOUT; timeout++) {
        bridgeFrame = np_pi_read(NP_REG_MCU_FRAME_RDY);
        if (bridgeFrame >= gNetplayState.frameCounter) {
            return TRUE;
        }
    }

    // Timed out - use last known inputs (don't freeze the game)
    return FALSE;
}

/*********************************
       Local Input Send
*********************************/

void netplay_send_local_input(u16 buttons, s8 stick_x, s8 stick_y) {
    u32 packed;
    u32 offset;

    if (!gNetplayState.enabled) {
        return;
    }

    packed = NP_PACK_INPUT(buttons, stick_x, stick_y);
    offset = NP_REG_LOCAL_0 + (gNetplayState.localPlayer * 4);
    np_pi_write(offset, packed);
}

/*********************************
      Game Setup from Bridge
*********************************/

/**
 * Force game configuration from bridge settings.
 * Sets player count, screen mode, game mode, character selections,
 * course, and CC. Call this before entering race state to override
 * menu selections with network-coordinated values.
 */
void netplay_setup_game(void) {
    s32 i;
    u32 charId;

    if (!gNetplayState.enabled) {
        return;
    }

    // Force player count
    gPlayerCount = gNetplayState.playerCount;
    gPlayerCountSelection1 = gNetplayState.playerCount;

    // Set screen mode based on player count
    switch (gNetplayState.playerCount) {
        case 1:
            gScreenModeSelection = SCREEN_MODE_1P;
            break;
        case 2:
            gScreenModeSelection = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;
            break;
        case 3:
        case 4:
            gScreenModeSelection = SCREEN_MODE_3P_4P_SPLITSCREEN;
            break;
    }
    gActiveScreenMode = gScreenModeSelection;

    // Set game mode from bridge (default to VERSUS for netplay)
    gModeSelection = (s32)np_pi_read(NP_REG_GAME_MODE);
    if (gModeSelection < GRAND_PRIX || gModeSelection > BATTLE) {
        gModeSelection = VERSUS;
    }

    // Set CC selection
    gCCSelection = (s32)np_pi_read(NP_REG_CC_SELECT);
    if (gCCSelection < CC_50 || gCCSelection > CC_150) {
        gCCSelection = CC_100;
    }

    // Set character selections from bridge
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        charId = np_pi_read(NP_REG_CHAR_SEL_0 + (i * 4));
        if (charId <= BOWSER) {
            gCharacterSelections[i] = (s8)charId;
        }
    }

    // Set course from bridge
    gCurrentCourseId = (s16)np_pi_read(NP_REG_COURSE_ID);

    // Signal menu ready to bridge
    np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY | NP_STATUS_MENU_READY);
}

/*********************************
      Race Start Sync
*********************************/

/**
 * Wait for bridge to signal that all players are ready to start.
 * Returns 1 when ready, 0 if still waiting.
 */
s32 netplay_wait_for_race_start(void) {
    u32 raceState;

    if (!gNetplayState.enabled) {
        return TRUE;
    }

    raceState = np_pi_read(NP_REG_RACE_START);
    gNetplayState.raceStartSync = (u8)raceState;

    if (raceState >= NP_RACE_ALL_READY) {
        return TRUE;
    }

    // Signal that this N64 is ready to start
    np_pi_write(NP_REG_RACE_START, NP_RACE_ALL_READY);
    return FALSE;
}

/*********************************
     Disconnect Handling
*********************************/

/**
 * Check for disconnected players. The bridge writes a bitmask
 * to NP_REG_DISCONNECT when a player drops.
 */
void netplay_handle_disconnects(void) {
    if (!gNetplayState.enabled) {
        return;
    }
    gNetplayState.disconnectMask = (u8)np_pi_read(NP_REG_DISCONNECT);
}

/*********************************
      Pause Coordination
*********************************/

/**
 * Only allow the local player's controller to trigger pause.
 * Remote players' START presses should not cause a local pause.
 */
s32 netplay_should_allow_pause(s32 controllerIndex) {
    if (!gNetplayState.enabled) {
        return TRUE;
    }
    return (controllerIndex == gNetplayState.localPlayer);
}

/*********************************
        RNG Synchronization
*********************************/

/**
 * Seed the game's LFSR RNG with the netplay-synchronized seed.
 * Call at race start to ensure all players have identical RNG sequences.
 * The seed comes from the gopher64 server's RNG exchange.
 */
void netplay_seed_rng(void) {
    if (!gNetplayState.enabled) {
        return;
    }
    // Re-read seed in case bridge updated it
    gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);
    // MK64's RNG uses a 16-bit seed
    gRandomSeed16 = (u16)(gNetplayState.rngSeed & 0xFFFF);
}

/*********************************
        State Queries
*********************************/

s32 netplay_is_active(void) {
    return gNetplayState.enabled;
}

s32 netplay_is_local_player(s32 playerIndex) {
    if (!gNetplayState.enabled) {
        return TRUE;
    }
    return (playerIndex == gNetplayState.localPlayer);
}

u8 netplay_get_local_player(void) {
    return gNetplayState.localPlayer;
}

u8 netplay_get_player_count(void) {
    if (!gNetplayState.enabled) {
        return 1;
    }
    return gNetplayState.playerCount;
}

u32 netplay_get_rng_seed(void) {
    return gNetplayState.rngSeed;
}
