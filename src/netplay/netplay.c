#include <ultra64.h>
#include <PR/os.h>
#include <string.h>
#include <defines.h>
#include <course.h>

#include "netplay.h"
#include "netlib.h"
#include "usb.h"
#include "main.h"
#include "menus.h"
#include "buffers/random.h"

/*********************************
            Globals
*********************************/

NetplayState gNetplayState;

/*********************************
     SC64 PI Bus Access
  (Used for SC64 SHM fallback)
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
    NetLib Packet Callbacks
 (Called by netlib_poll() when
  packets arrive from server)
*********************************/

/**
 * Server assigns our player slot and tells us how many players total.
 * Payload: [player_num:1][player_count:1]
 */
static void pkt_assign_player(size_t size) {
    uint8_t playerNum, playerCount;
    (void)size;

    netlib_readbyte(&playerNum);
    netlib_readbyte(&playerCount);

    gNetplayState.localPlayer = playerNum;
    gNetplayState.playerCount = playerCount;
    gNetplayState.connected = TRUE;

    netlib_setclient(playerNum + 1); // NetLib clients are 1-indexed

    // Build control mask: all players except us
    gNetplayState.ctrlMask = (u8)((1 << playerCount) - 1) & ~(1 << playerNum);
}

/**
 * Receive another player's controller input.
 * Payload: [player:1][frame:4][packed_input:4]
 */
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

/**
 * Server signals all players are ready — start the countdown.
 */
static void pkt_all_ready(size_t size) {
    (void)size;
    gNetplayState.allReady = TRUE;
}

/**
 * Receive game configuration from the host player via server.
 * Payload: [mode:1][course:2][cc:1][char0:1][char1:1][char2:1][char3:1][rng_seed:4][input_delay:1]
 */
static void pkt_server_config(size_t size) {
    uint8_t mode, cc, delay;
    uint16_t course;
    uint32_t seed;
    uint8_t ch;
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

/**
 * A player disconnected.
 * Payload: [player:1]
 */
static void pkt_player_left(size_t size) {
    uint8_t player;
    (void)size;

    netlib_readbyte(&player);
    if (player < NP_MAX_PLAYERS) {
        gNetplayState.disconnectMask |= (1 << player);
    }
}

/**
 * Server heartbeat — respond immediately.
 */
static void pkt_heartbeat(size_t size) {
    (void)size;
    netlib_start(PKTID_HEARTBEAT_ACK);
    netlib_sendtoserver();
}

/**
 * Register all packet callbacks with NetLib.
 */
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
    gNetplayState.allReady = FALSE;
    gNetplayState.connected = FALSE;
    gNetplayState.configReceived = FALSE;
    gNetplayState.inRace = FALSE;
    for (i = 0; i < NP_INPUT_DELAY_MAX; i++) {
        gNetplayState.inputDelayBuffer[i] = 0;
    }
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        gNetplayState.remoteFrames[i] = 0;
    }
}

/**
 * Try to detect and connect to a netplay server.
 * First tries N64-NetLib (USB direct to server via any flashcart).
 * Falls back to SC64 shared memory (bridge mode) if no USB server found.
 *
 * Returns TRUE if a connection was established.
 */
s32 netplay_detect(void) {
    // Try N64-NetLib first — works on SC64, 64Drive, and EverDrive
    netlib_initialize();
    if (usb_getcart() != CART_NONE) {
        s32 pollCount;

        gNetplayState.mode = NP_MODE_NETLIB;
        gNetplayState.enabled = TRUE;

        netplay_register_callbacks();

        // Send connection request to server
        netlib_start(PKTID_CONNECT);
        netlib_sendtoserver();

        // Poll briefly to get player assignment.
        // Server responds with PKTID_ASSIGN_PLAYER.
        // Give it a few poll cycles since USB round-trip takes time.
        for (pollCount = 0; pollCount < 30; pollCount++) {
            netlib_poll();
            if (gNetplayState.connected) {
                return TRUE;
            }
        }

        // Flashcart detected but no server responded — stay in NetLib
        // mode so future polls can still pick up the assignment.
        return TRUE;
    }

    // Fallback: check for SC64 bridge via shared memory magic
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
        // Poll for incoming packets — this processes all callbacks
        netlib_poll();
    } else {
        // SC64 shared memory mode
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

    // Send local input
    netplay_send_local_input(localPad.button, localPad.stick_x, localPad.stick_y);

    // Input delay buffering
    packed = NP_PACK_INPUT(localPad.button, localPad.stick_x, localPad.stick_y);
    if (gNetplayState.inputDelay > 0) {
        gNetplayState.inputDelayBuffer[gNetplayState.inputDelayHead] = packed;
        gNetplayState.inputDelayHead = (gNetplayState.inputDelayHead + 1) % NP_INPUT_DELAY_MAX;
        i = (gNetplayState.inputDelayHead + NP_INPUT_DELAY_MAX - gNetplayState.inputDelay) % NP_INPUT_DELAY_MAX;
        delayedInput = gNetplayState.inputDelayBuffer[i];
    } else {
        delayedInput = packed;
    }

    // Fill all player slots
    for (i = 0; i < NP_MAX_PLAYERS; i++) {
        if (i == gNetplayState.localPlayer) {
            pads[i].button = NP_UNPACK_BUTTONS(delayedInput);
            pads[i].stick_x = NP_UNPACK_STICK_X(delayedInput);
            pads[i].stick_y = NP_UNPACK_STICK_Y(delayedInput);
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
    s32 timeout;
    s32 i;

    if (!gNetplayState.enabled || !gNetplayState.inRace) {
        return TRUE;
    }

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        // Check if we have inputs from all remote players for the current frame.
        // If not, poll a few more times to give them a chance to arrive.
        for (timeout = 0; timeout < NP_FRAME_SYNC_TIMEOUT; timeout++) {
            s32 allReady = TRUE;
            for (i = 0; i < NP_MAX_PLAYERS; i++) {
                if (i == gNetplayState.localPlayer) continue;
                if (gNetplayState.disconnectMask & (1 << i)) continue;
                if (!(gNetplayState.ctrlMask & (1 << i))) continue;
                if (gNetplayState.remoteFrames[i] < gNetplayState.frameCounter) {
                    allReady = FALSE;
                    break;
                }
            }
            if (allReady) return TRUE;
            netlib_poll();
        }
        return FALSE;
    } else {
        // SC64 shared memory mode
        for (timeout = 0; timeout < NP_FRAME_SYNC_TIMEOUT; timeout++) {
            u32 bridgeFrame = np_pi_read(NP_REG_MCU_FRAME_RDY);
            if (bridgeFrame >= gNetplayState.frameCounter) {
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
        // Send input packet to server for relay to other players
        netlib_start(PKTID_PLAYER_INPUT);
        netlib_writedword(gNetplayState.frameCounter);
        netlib_writedword(packed);
        netlib_broadcast();
    } else {
        // SC64 shared memory
        u32 offset = NP_REG_LOCAL_0 + (gNetplayState.localPlayer * 4);
        np_pi_write(offset, packed);
    }
}

/*********************************
      Game Setup from Network
*********************************/

void netplay_setup_game(void) {
    s32 i;

    if (!gNetplayState.enabled) {
        return;
    }

    if (gNetplayState.mode == NP_MODE_NETLIB) {
        // Use config received from server via PKTID_SERVER_CONFIG callback
        if (!gNetplayState.configReceived) {
            // Poll until we get config (with timeout)
            for (i = 0; i < 60; i++) {
                netlib_poll();
                if (gNetplayState.configReceived) break;
            }
        }

        gPlayerCount = gNetplayState.playerCount;
        gPlayerCountSelection1 = gNetplayState.playerCount;

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

        gModeSelection = gNetplayState.cfgMode;
        if (gModeSelection < GRAND_PRIX || gModeSelection > BATTLE) {
            gModeSelection = VERSUS;
        }

        gCCSelection = gNetplayState.cfgCC;
        if (gCCSelection < CC_50 || gCCSelection > CC_150) {
            gCCSelection = CC_100;
        }

        for (i = 0; i < NP_MAX_PLAYERS; i++) {
            if (gNetplayState.cfgCharacters[i] >= 0 && gNetplayState.cfgCharacters[i] <= BOWSER) {
                gCharacterSelections[i] = gNetplayState.cfgCharacters[i];
            }
        }

        gCurrentCourseId = gNetplayState.cfgCourse;

    } else {
        // SC64 shared memory mode — read config from bridge registers
        u32 charId;

        gPlayerCount = gNetplayState.playerCount;
        gPlayerCountSelection1 = gNetplayState.playerCount;

        switch (gNetplayState.playerCount) {
            case 1: gScreenModeSelection = SCREEN_MODE_1P; break;
            case 2: gScreenModeSelection = SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL; break;
            case 3: case 4: gScreenModeSelection = SCREEN_MODE_3P_4P_SPLITSCREEN; break;
        }
        gActiveScreenMode = gScreenModeSelection;

        gModeSelection = (s32)np_pi_read(NP_REG_GAME_MODE);
        if (gModeSelection < GRAND_PRIX || gModeSelection > BATTLE) {
            gModeSelection = VERSUS;
        }

        gCCSelection = (s32)np_pi_read(NP_REG_CC_SELECT);
        if (gCCSelection < CC_50 || gCCSelection > CC_150) {
            gCCSelection = CC_100;
        }

        for (i = 0; i < NP_MAX_PLAYERS; i++) {
            charId = np_pi_read(NP_REG_CHAR_SEL_0 + (i * 4));
            if (charId <= BOWSER) {
                gCharacterSelections[i] = (s8)charId;
            }
        }

        gCurrentCourseId = (s16)np_pi_read(NP_REG_COURSE_ID);
        np_pi_write(NP_REG_STATUS, NP_STATUS_N64_READY | NP_STATUS_MENU_READY);
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
        if (gNetplayState.allReady) {
            return TRUE;
        }
        // Signal we're ready and poll for ALL_READY from server
        netlib_start(PKTID_READY);
        netlib_sendtoserver();
        netlib_poll();
        return gNetplayState.allReady;
    } else {
        u32 raceState = np_pi_read(NP_REG_RACE_START);
        if (raceState >= 1) {
            return TRUE;
        }
        np_pi_write(NP_REG_RACE_START, 1);
        return FALSE;
    }
}

/*********************************
     Disconnect Handling
*********************************/

void netplay_handle_disconnects(void) {
    if (!gNetplayState.enabled) {
        return;
    }
    if (gNetplayState.mode == NP_MODE_SC64_SHM) {
        gNetplayState.disconnectMask = (u8)np_pi_read(NP_REG_DISCONNECT);
    }
    // In NetLib mode, disconnects are handled by the pkt_player_left callback
}

/*********************************
      Pause Coordination
*********************************/

s32 netplay_should_allow_pause(s32 controllerIndex) {
    if (!gNetplayState.enabled) {
        return TRUE;
    }
    return (controllerIndex == gNetplayState.localPlayer);
}

/*********************************
    RNG Synchronization
*********************************/

void netplay_seed_rng(void) {
    if (!gNetplayState.enabled) {
        return;
    }
    if (gNetplayState.mode == NP_MODE_SC64_SHM) {
        gNetplayState.rngSeed = np_pi_read(NP_REG_RNG_SEED);
    }
    // In NetLib mode, rngSeed was set by pkt_server_config callback
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
