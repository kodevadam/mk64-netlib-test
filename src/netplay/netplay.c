#include <ultra64.h>
#include <PR/os.h>
#include <string.h>

#include "netplay.h"
#include "main.h"

/*********************************
            Globals
*********************************/

NetplayState gNetplayState;

/*********************************
     SC64 PI Bus Access
  (Uncached memory-mapped I/O)
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
    memset(&gNetplayState, 0, sizeof(NetplayState));
    gNetplayState.mode = NP_MODE_DISABLED;
    gNetplayState.enabled = FALSE;
    gNetplayState.localPlayer = 0;
    gNetplayState.playerCount = 1;
    gNetplayState.ctrlMask = 0;
    gNetplayState.frameCounter = 0;
    gNetplayState.rngSeed = 0;
    gNetplayState.inputDelay = 0;
    gNetplayState.inRace = FALSE;
}

/**
 * Detect if SC64 bridge is connected by checking for magic value
 * in shared memory. Returns 1 if bridge is detected, 0 otherwise.
 */
s32 netplay_detect(void) {
    u32 magic;

    magic = np_pi_read(NP_REG_MAGIC);

    if (magic == NP_MAGIC) {
        // Bridge is connected and ready
        gNetplayState.mode = NP_MODE_SC64_SHM;
        gNetplayState.enabled = TRUE;

        // Read configuration from bridge
        gNetplayState.localPlayer = (u8)np_pi_read(NP_REG_LOCAL_PLAYER);
        gNetplayState.playerCount = (u8)np_pi_read(NP_REG_PLAYER_COUNT);
        gNetplayState.ctrlMask = (u8)np_pi_read(NP_REG_CTRL_MASK);
        gNetplayState.inputDelay = (u8)np_pi_read(NP_REG_INPUT_DELAY);
        gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);

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

/**
 * Called once per game frame before controller processing.
 * Reads the latest remote controller data from SC64 shared memory.
 */
void netplay_update(void) {
    s32 i;

    if (!gNetplayState.enabled) {
        return;
    }

    // Update frame counter
    gNetplayState.frameCounter++;
    np_pi_write(NP_REG_FRAME, gNetplayState.frameCounter);

    // Read remote controller overrides from shared memory
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        if (i != gNetplayState.localPlayer && (gNetplayState.ctrlMask & (1 << i))) {
            gNetplayState.remoteInputs[i] = np_pi_read(NP_REG_OVERRIDE_0 + (i * 4));
        }
    }

    // Re-read control mask in case bridge updated it
    gNetplayState.ctrlMask = (u8)np_pi_read(NP_REG_CTRL_MASK);
}

/*********************************
     Controller Override
*********************************/

/**
 * Apply netplay controller overrides to the raw OSContPad data.
 * Called from read_controllers() after osContGetReadData().
 *
 * The local player's physical controller (always port 0 on the N64)
 * gets mapped to their assigned player slot. Remote player data
 * from the bridge fills the other slots.
 */
void netplay_apply_controller_overrides(OSContPad *pads) {
    OSContPad localPad;
    s32 i;
    u32 packed;

    if (!gNetplayState.enabled) {
        return;
    }

    // Save local player's physical input (always from port 0)
    localPad = pads[0];

    // Send local input to bridge via shared memory
    netplay_send_local_input(localPad.button, localPad.stick_x, localPad.stick_y);

    // Fill all player slots
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        if (i == gNetplayState.localPlayer) {
            // This is our slot - use physical controller input
            pads[i].button = localPad.button;
            pads[i].stick_x = localPad.stick_x;
            pads[i].stick_y = localPad.stick_y;
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

/**
 * Wait for the bridge to confirm that remote input data is available
 * for the current frame. Returns 1 if data is ready, 0 if timed out.
 *
 * This implements frame-locked input delivery: the game will not
 * advance past frame N until the bridge has written remote inputs
 * for frame N.
 */
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
        // Brief wait before checking again
        // Each iteration is roughly one PI bus access cycle
    }

    // Timed out - use last known inputs (don't freeze the game)
    return FALSE;
}

/*********************************
       Local Input Send
*********************************/

/**
 * Write local controller input to SC64 shared memory so the bridge
 * can read it and send to the gopher64 server.
 */
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
        State Queries
*********************************/

s32 netplay_is_active(void) {
    return gNetplayState.enabled;
}

s32 netplay_is_local_player(s32 playerIndex) {
    if (!gNetplayState.enabled) {
        return TRUE; // When no netplay, all players are local
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
