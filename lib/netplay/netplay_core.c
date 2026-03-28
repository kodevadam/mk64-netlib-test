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

extern s32 osPiRawReadIo(u32, u32 *);
extern s32 osPiRawWriteIo(u32, u32);

u32 np_pi_read(u32 offset) {
    u32 value = 0;
    osPiRawReadIo(NP_SHM_BASE + offset, &value);
    return value;
}

void np_pi_write(u32 offset, u32 value) {
    osPiRawWriteIo(NP_SHM_BASE + offset, value);
}

/*********************************
    NetLib Packet Callbacks
*********************************/

static void pkt_assign_player(size_t size) {
    u8 playerNum, playerCount;
    s32 i;
    u8 localMask;
    (void)size;

    netlib_readbyte(&playerNum);
    netlib_readbyte(&playerCount);

    gNetplayState.localPlayer = playerNum;
    gNetplayState.playerCount = playerCount;
    gNetplayState.connected = TRUE;
    if (gNetplayState.lobbyState < NP_LOBBY_CONNECTED) {
        gNetplayState.lobbyState = NP_LOBBY_CONNECTED;
    }

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
    u8 player;
    u32 frame, input;
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
    u8 player;
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

/*********************************
     Lobby Packet Callbacks
*********************************/

/**
 * Server sends list of available rooms.
 * Payload: [count:1] then per room: [id:2][name_len:1][name:var][cur:1][max:1][in_game:1]
 */
static void pkt_room_list(size_t size) {
    u8 count, nameLen, cur, max, inGame;
    u16 id;
    s32 i;
    (void)size;

    netlib_readbyte(&count);
    if (count > NP_MAX_ROOMS) count = NP_MAX_ROOMS;
    gNetplayState.roomCount = count;

    for (i = 0; i < count; i++) {
        netlib_readword(&id);
        netlib_readbyte(&nameLen);
        if (nameLen > NP_ROOM_NAME_MAX - 1) nameLen = NP_ROOM_NAME_MAX - 1;
        netlib_readbytes((byte *)gNetplayState.rooms[i].name, nameLen);
        gNetplayState.rooms[i].name[nameLen] = '\0';
        netlib_readbyte(&cur);
        netlib_readbyte(&max);
        netlib_readbyte(&inGame);
        gNetplayState.rooms[i].id = id;
        gNetplayState.rooms[i].currentPlayers = cur;
        gNetplayState.rooms[i].maxPlayers = max;
        gNetplayState.rooms[i].inGame = inGame;
    }
    gNetplayState.roomListReceived = TRUE;
}

/**
 * Server confirms we joined a room.
 * Payload: [room_id:2][slot:1][player_count:1][host:1]
 */
static void pkt_room_joined(size_t size) {
    u16 roomId;
    u8 slot, playerCount, host;
    (void)size;

    netlib_readword(&roomId);
    netlib_readbyte(&slot);
    netlib_readbyte(&playerCount);
    netlib_readbyte(&host);

    gNetplayState.roomId = roomId;
    gNetplayState.localPlayer = slot;
    gNetplayState.playerCount = playerCount;
    gNetplayState.isHost = host;
    gNetplayState.lobbyState = NP_LOBBY_IN_ROOM;
    gNetplayState.roomJoinError = 0;

    netlib_setclient(slot + 1);
}

/**
 * Room state updated (player joined or left).
 * Payload: [player_count:1][slot:1][joined_or_left:1]
 */
static void pkt_room_update(size_t size) {
    u8 playerCount, slot, action;
    s32 i;
    u8 localMask;
    (void)size;

    netlib_readbyte(&playerCount);
    netlib_readbyte(&slot);
    netlib_readbyte(&action);

    gNetplayState.playerCount = playerCount;

    if (action == 0) {
        // Player left — mark disconnected
        if (slot < NP_MAX_PLAYERS) {
            gNetplayState.disconnectMask |= (1 << slot);
        }
    }

    // Rebuild control mask
    localMask = 0;
    for (i = 0; i < gNetplayState.localPlayerCount; i++) {
        localMask |= (1 << gNetplayState.localSlots[i]);
    }
    gNetplayState.ctrlMask = (u8)((1 << playerCount) - 1) & ~localMask;
}

/**
 * Room operation error.
 * Payload: [error_code:1]
 */
static void pkt_room_error(size_t size) {
    u8 code;
    (void)size;
    netlib_readbyte(&code);
    gNetplayState.roomJoinError = code;
}

static void netplay_register_callbacks(void) {
    // Lobby
    netlib_register(PKTID_ROOM_LIST,     pkt_room_list);
    netlib_register(PKTID_ROOM_JOINED,   pkt_room_joined);
    netlib_register(PKTID_ROOM_UPDATE,   pkt_room_update);
    netlib_register(PKTID_ROOM_ERROR,    pkt_room_error);
    // In-game
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
    bzero(&gNetplayState, sizeof(NetplayState));
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

    // Try N64-NetLib first (works on SC64, 64Drive, EverDrive).
    // Only detect the cart — do NOT send any USB data here.
    // NetLib Browser initiates communication from the PC side.
    netlib_initialize();
    if (usb_getcart() != CART_NONE) {
        gNetplayState.mode = NP_MODE_NETLIB;
        gNetplayState.enabled = TRUE;
        netplay_register_callbacks();
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
            netlib_writebyte((u8)gNetplayState.localSlots[i]);
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
        netlib_writebyte((u8)gNetplayState.localPlayer);
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
          Lobby API
*********************************/

void netplay_request_room_list(void) {
    if (gNetplayState.mode != NP_MODE_NETLIB) return;
    gNetplayState.roomListReceived = FALSE;
    netlib_start(PKTID_LIST_ROOMS);
    netlib_sendtoserver();
}

void netplay_create_room(const char *name, u8 maxPlayers) {
    u8 nameLen;
    if (gNetplayState.mode != NP_MODE_NETLIB) return;

    nameLen = 0;
    while (name[nameLen] != '\0' && nameLen < NP_ROOM_NAME_MAX - 1) nameLen++;

    netlib_start(PKTID_CREATE_ROOM);
    netlib_writebyte(nameLen);
    netlib_writebytes((byte *)name, nameLen);
    netlib_writebyte(maxPlayers);
    netlib_writebyte(0); // no password
    netlib_sendtoserver();
}

void netplay_join_room(u16 roomId) {
    if (gNetplayState.mode != NP_MODE_NETLIB) return;
    gNetplayState.roomJoinError = 0;
    netlib_start(PKTID_JOIN_ROOM);
    netlib_writeword(roomId);
    netlib_writebyte(0); // no password
    netlib_sendtoserver();
}

void netplay_leave_room(void) {
    if (gNetplayState.mode != NP_MODE_NETLIB) return;
    netlib_start(PKTID_LEAVE_ROOM);
    netlib_sendtoserver();
    gNetplayState.lobbyState = NP_LOBBY_CONNECTED;
    gNetplayState.roomId = 0;
    gNetplayState.isHost = FALSE;
}

s32 netplay_is_host(void) {
    return gNetplayState.isHost;
}

u8 netplay_get_lobby_state(void) {
    return (u8)gNetplayState.lobbyState;
}

u8 netplay_get_room_count(void) {
    return gNetplayState.roomCount;
}

NetplayRoom* netplay_get_room(u8 index) {
    if (index >= gNetplayState.roomCount) return NULL;
    return &gNetplayState.rooms[index];
}

/*********************************
        State Queries
*********************************/

s32 netplay_is_active(void) {
    return gNetplayState.enabled && gNetplayState.connected;
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
