/**
 * netplay_core.c — Game-agnostic N64 netplay engine.
 *
 * Handles: connection, packet I/O, controller overrides, frame sync,
 * input delay, multi-local support, disconnect, pause coordination.
 *
 * Does NOT contain any game-specific logic. Game hooks are called via
 * the extern callbacks declared in netplay_core.h:
 *   netplay_game_parse_config()
 *   netplay_game_setup()
 *   netplay_game_seed_rng()
 *   netplay_game_send_config()
 */

#include <ultra64.h>
#include <PR/os.h>
#include <string.h>

#include "netplay_core.h"
#include "netlib.h"
#include "usb.h"

// gControllerBits from libultra (set by osContInit)
extern u8 gControllerBits;

/*********************************
            Globals
*********************************/

NetplayState gNetplayState;

/*********************************
     SC64 PI Bus Access
*********************************/

u32 np_pi_read(u32 offset) {
    u32 value = 0;
    osPiReadIo(NP_SHM_BASE + offset, &value);
    return value;
}

void np_pi_write(u32 offset, u32 value) {
    osPiWriteIo(NP_SHM_BASE + offset, value);
}

/*********************************
    NetLib Packet Callbacks
*********************************/

static void pkt_assign_player(size_t size) {
    uint8_t playerNum, playerCount;
    s32 i;
    u8 localMask;
    (void)size;

    netlib_readbyte(&playerNum);
    netlib_readbyte(&playerCount);

    gNetplayState.localPlayer = playerNum;
    gNetplayState.playerCount = playerCount;
    gNetplayState.connected = TRUE;

    netlib_setclient(playerNum + 1);

    for (i = 0; i < gNetplayState.localPlayerCount; i++) {
        gNetplayState.localSlots[i] = playerNum + i;
    }

    localMask = 0;
    for (i = 0; i < gNetplayState.localPlayerCount; i++) {
        localMask |= (1 << gNetplayState.localSlots[i]);
    }
    gNetplayState.ctrlMask = (u8)((1 << playerCount) - 1) & ~localMask;
}

static void pkt_remote_input(size_t size) {
    uint8_t player;
    uint32_t frame, input;
    (void)size;

    netlib_readbyte(&player);
    netlib_readdword(&frame);
    netlib_readdword(&input);

    if (player < NP_MAX_PLAYERS) {
        gNetplayState.remoteInputs[player] = input;
        gNetplayState.remoteFrames[player] = frame;
    }
}

static void pkt_all_ready(size_t size) {
    (void)size;
    gNetplayState.allReady = TRUE;
}

static void pkt_server_config(size_t size) {
    // Delegate to game-specific parser
    netplay_game_parse_config(size);
}

static void pkt_player_left(size_t size) {
    uint8_t player;
    (void)size;

    netlib_readbyte(&player);
    if (player < NP_MAX_PLAYERS) {
        gNetplayState.disconnectMask |= (1 << player);
    }
}

static void pkt_heartbeat(size_t size) {
    (void)size;
    netlib_start(PKTID_HEARTBEAT_ACK);
    netlib_sendtoserver();
}

static void netplay_register_callbacks(void) {
    netlib_register(PKTID_ASSIGN_PLAYER, pkt_assign_player);
    netlib_register(PKTID_REMOTE_INPUT,  pkt_remote_input);
    netlib_register(PKTID_ALL_READY,     pkt_all_ready);
    netlib_register(PKTID_SERVER_CONFIG,  pkt_server_config);
    netlib_register(PKTID_PLAYER_LEFT,   pkt_player_left);
    netlib_register(PKTID_HEARTBEAT,     pkt_heartbeat);
}

/*********************************
        Initialization
*********************************/

void netplay_init(void) {
    s32 i, j;
    memset(&gNetplayState, 0, sizeof(NetplayState));
    gNetplayState.mode = NP_MODE_DISABLED;
    gNetplayState.enabled = FALSE;
    gNetplayState.localPlayer = 0;
    gNetplayState.localPlayerCount = 1;
    gNetplayState.playerCount = 1;
    gNetplayState.frameCounter = 0;
    gNetplayState.inputDelayHead = 0;
    for (i = 0; i < NP_MAX_LOCAL; i++) {
        gNetplayState.localSlots[i] = (u8)i;
        for (j = 0; j < NP_INPUT_DELAY_MAX; j++) {
            gNetplayState.inputDelayBuffer[i][j] = 0;
        }
    }
}

s32 netplay_detect(void) {
    s32 i;

    // Count physical controllers
    gNetplayState.localPlayerCount = 0;
    for (i = 0; i < NP_MAX_LOCAL; i++) {
        if (gControllerBits & (1 << i)) {
            gNetplayState.localPlayerCount++;
        }
    }
    if (gNetplayState.localPlayerCount == 0) {
        gNetplayState.localPlayerCount = 1;
    }

    // Try N64-NetLib first (works on SC64, 64Drive, EverDrive)
    netlib_initialize();
    if (usb_getcart() != CART_NONE) {
        s32 pollCount;

        gNetplayState.mode = NP_MODE_NETLIB;
        gNetplayState.enabled = TRUE;

        netplay_register_callbacks();

        netlib_start(PKTID_CONNECT);
        netlib_sendtoserver();

        for (pollCount = 0; pollCount < 30; pollCount++) {
            netlib_poll();
            if (gNetplayState.connected) {
                return TRUE;
            }
        }
        return TRUE;
    }

    // Fallback: SC64 shared memory bridge
    {
        u32 magic = np_pi_read(NP_REG_MAGIC);
        if (magic == NP_MAGIC) {
            gNetplayState.mode = NP_MODE_SC64_SHM;
            gNetplayState.enabled = TRUE;
            gNetplayState.localPlayer = (u8)np_pi_read(NP_REG_LOCAL_PLAYER);
            gNetplayState.playerCount = (u8)np_pi_read(NP_REG_PLAYER_COUNT);
            gNetplayState.ctrlMask = (u8)np_pi_read(NP_REG_CTRL_MASK);
            gNetplayState.inputDelay = (u8)np_pi_read(NP_REG_INPUT_DELAY);
            gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);
            gNetplayState.connected = TRUE;
            if (gNetplayState.inputDelay > NP_INPUT_DELAY_MAX) {
                gNetplayState.inputDelay = NP_INPUT_DELAY_MAX;
            }
            np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY);
            np_pi_write(NP_REG_ENABLE, 1);
            return TRUE;
        }
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

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        netlib_poll();
    } else {
        np_pi_write(NP_REG_FRAME, gNetplayState.frameCounter);
        for (i = 0; i < NP_MAX_PLAYERS; i++) {
            if (i != gNetplayState.localPlayer && (gNetplayState.ctrlMask & (1 << i))) {
                gNetplayState.remoteInputs[i] = np_pi_read(NP_REG_OVERRIDE_0 + (i * 4));
            }
        }
        gNetplayState.ctrlMask = (u8)np_pi_read(NP_REG_CTRL_MASK);
        gNetplayState.disconnectMask = (u8)np_pi_read(NP_REG_DISCONNECT);
        if (gNetplayState.inRace) {
            np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY | NP_STATUS_IN_RACE);
        }
    }
}

/*********************************
     Controller Override
*********************************/

static s32 np_local_index_for_slot(s32 slot) {
    s32 i;
    for (i = 0; i < gNetplayState.localPlayerCount; i++) {
        if (gNetplayState.localSlots[i] == slot) {
            return i;
        }
    }
    return -1;
}

void netplay_apply_controller_overrides(OSContPad *pads) {
    OSContPad localPads[NP_MAX_LOCAL];
    s32 i, localIdx;
    u32 packed, delayedInput;

    if (!gNetplayState.enabled) {
        return;
    }

    // Save and send all local players' physical inputs
    for (i = 0; i < gNetplayState.localPlayerCount; i++) {
        localPads[i] = pads[i];
        packed = NP_PACK_INPUT(localPads[i].button, localPads[i].stick_x, localPads[i].stick_y);

        if (gNetplayState.mode == NP_MODE_NETLIB) {
            netlib_start(PKTID_PLAYER_INPUT);
            netlib_writebyte((uint8_t)gNetplayState.localSlots[i]);
            netlib_writedword(gNetplayState.frameCounter);
            netlib_writedword(packed);
            netlib_broadcast();
        } else {
            np_pi_write(NP_REG_LOCAL_0 + (gNetplayState.localSlots[i] * 4), packed);
        }

        if (gNetplayState.inputDelay > 0) {
            gNetplayState.inputDelayBuffer[i][gNetplayState.inputDelayHead] = packed;
        }
    }
    if (gNetplayState.inputDelay > 0) {
        gNetplayState.inputDelayHead = (gNetplayState.inputDelayHead + 1) % NP_INPUT_DELAY_MAX;
    }

    // Fill all game slots (max 4 for OSContPad array size)
    for (i = 0; i < 4 && i < gNetplayState.playerCount; i++) {
        localIdx = np_local_index_for_slot(i);

        if (localIdx >= 0) {
            if (gNetplayState.inputDelay > 0) {
                s32 readPos = (gNetplayState.inputDelayHead + NP_INPUT_DELAY_MAX
                               - gNetplayState.inputDelay) % NP_INPUT_DELAY_MAX;
                delayedInput = gNetplayState.inputDelayBuffer[localIdx][readPos];
                pads[i].button = NP_UNPACK_BUTTONS(delayedInput);
                pads[i].stick_x = NP_UNPACK_STICK_X(delayedInput);
                pads[i].stick_y = NP_UNPACK_STICK_Y(delayedInput);
            } else {
                pads[i].button = localPads[localIdx].button;
                pads[i].stick_x = localPads[localIdx].stick_x;
                pads[i].stick_y = localPads[localIdx].stick_y;
            }
            pads[i].errno = 0;
        } else if (gNetplayState.disconnectMask & (1 << i)) {
            pads[i].button = 0;
            pads[i].stick_x = 0;
            pads[i].stick_y = 0;
            pads[i].errno = 0;
        } else if (gNetplayState.ctrlMask & (1 << i)) {
            packed = gNetplayState.remoteInputs[i];
            pads[i].button = NP_UNPACK_BUTTONS(packed);
            pads[i].stick_x = NP_UNPACK_STICK_X(packed);
            pads[i].stick_y = NP_UNPACK_STICK_Y(packed);
            pads[i].errno = 0;
        } else {
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
    s32 timeout, i;

    if (!gNetplayState.enabled || !gNetplayState.inRace) {
        return TRUE;
    }

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        for (timeout = 0; timeout < NP_FRAME_SYNC_TIMEOUT; timeout++) {
            s32 ready = TRUE;
            for (i = 0; i < NP_MAX_PLAYERS; i++) {
                if (np_local_index_for_slot(i) >= 0) continue;
                if (gNetplayState.disconnectMask & (1 << i)) continue;
                if (!(gNetplayState.ctrlMask & (1 << i))) continue;
                if (gNetplayState.remoteFrames[i] < gNetplayState.frameCounter) {
                    ready = FALSE;
                    break;
                }
            }
            if (ready) return TRUE;
            netlib_poll();
        }
        return FALSE;
    } else {
        for (timeout = 0; timeout < NP_FRAME_SYNC_TIMEOUT; timeout++) {
            if (np_pi_read(NP_REG_MCU_FRAME_RDY) >= gNetplayState.frameCounter) {
                return TRUE;
            }
        }
        return FALSE;
    }
}

/*********************************
       Local Input Send
*********************************/

void netplay_send_local_input(u16 buttons, s8 stick_x, s8 stick_y) {
    u32 packed;

    if (!gNetplayState.enabled) {
        return;
    }

    packed = NP_PACK_INPUT(buttons, stick_x, stick_y);

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        netlib_start(PKTID_PLAYER_INPUT);
        netlib_writebyte((uint8_t)gNetplayState.localPlayer);
        netlib_writedword(gNetplayState.frameCounter);
        netlib_writedword(packed);
        netlib_broadcast();
    } else {
        np_pi_write(NP_REG_LOCAL_0 + (gNetplayState.localPlayer * 4), packed);
    }
}

/*********************************
      Race Start Sync
*********************************/

s32 netplay_wait_for_race_start(void) {
    if (!gNetplayState.enabled) {
        return TRUE;
    }
    if (gNetplayState.mode == NP_MODE_NETLIB) {
        if (gNetplayState.allReady) return TRUE;
        netlib_start(PKTID_READY);
        netlib_sendtoserver();
        netlib_poll();
        return gNetplayState.allReady;
    } else {
        if (np_pi_read(NP_REG_RACE_START) >= 1) return TRUE;
        np_pi_write(NP_REG_RACE_START, 1);
        return FALSE;
    }
}

/*********************************
     Disconnect Handling
*********************************/

void netplay_handle_disconnects(void) {
    if (!gNetplayState.enabled) return;
    if (gNetplayState.mode == NP_MODE_SC64_SHM) {
        gNetplayState.disconnectMask = (u8)np_pi_read(NP_REG_DISCONNECT);
    }
}

/*********************************
      Pause Coordination
*********************************/

s32 netplay_should_allow_pause(s32 controllerIndex) {
    if (!gNetplayState.enabled) return TRUE;
    return (np_local_index_for_slot(controllerIndex) >= 0);
}

/*********************************
        State Queries
*********************************/

s32 netplay_is_active(void) {
    return gNetplayState.enabled;
}

s32 netplay_is_local_player(s32 playerIndex) {
    if (!gNetplayState.enabled) return TRUE;
    return (np_local_index_for_slot(playerIndex) >= 0);
}

u8 netplay_get_local_player(void) {
    return gNetplayState.localPlayer;
}

u8 netplay_get_player_count(void) {
    if (!gNetplayState.enabled) return 1;
    return gNetplayState.playerCount;
}

u32 netplay_get_rng_seed(void) {
    return gNetplayState.rngSeed;
}
