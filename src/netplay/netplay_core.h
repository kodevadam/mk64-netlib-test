/**
 * netplay_core.h — Game-agnostic N64 netplay library.
 *
 * Works on SC64, 64Drive, and EverDrive via N64-NetLib.
 * Supports up to 8 online players with up to 4 local per console.
 *
 * To use in a new decomp game:
 * 1. Copy netplay_core.c/h, netlib.c/h, usb.c/h into your project
 * 2. Create a netplay_game.c that implements the game-specific callbacks
 * 3. Hook netplay_init/detect/update into your game loop
 * 4. Hook netplay_apply_controller_overrides after osContGetReadData
 */

#ifndef _NETPLAY_CORE_H_
#define _NETPLAY_CORE_H_

#include <ultra64.h>

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif


/*********************************
       NetLib Packet Types
*********************************/

// Client → Server: Connection & Lobby
#define PKTID_CONNECT        0x00  // [local_count:1] — request to join server
#define PKTID_LIST_ROOMS     0x05  // [] — request room list
#define PKTID_CREATE_ROOM    0x06  // [name_len:1][name:var][max_players:1][password_len:1][password:var]
#define PKTID_JOIN_ROOM      0x07  // [room_id:2][password_len:1][password:var]
#define PKTID_LEAVE_ROOM     0x08  // []

// Client → Server: In-Game
#define PKTID_PLAYER_INPUT   0x01  // [slot:1][frame:4][packed_input:4]
#define PKTID_READY          0x02  // []
#define PKTID_GAME_CONFIG    0x03  // [game-specific payload]
#define PKTID_HEARTBEAT_ACK  0x04  // []

// Server → Client: Connection & Lobby
#define PKTID_ASSIGN_PLAYER  0x10  // [player_num:1][player_count:1]
#define PKTID_ROOM_LIST      0x16  // [count:1] then per room: [id:2][name_len:1][name:var][cur:1][max:1][in_game:1]
#define PKTID_ROOM_JOINED    0x17  // [room_id:2][slot:1][player_count:1][host:1]
#define PKTID_ROOM_UPDATE    0x18  // [player_count:1][slot_that_changed:1][joined_or_left:1]
#define PKTID_ROOM_ERROR     0x19  // [error_code:1] (1=full, 2=not found, 3=wrong password, 4=in game)

// Server → Client: In-Game
#define PKTID_REMOTE_INPUT   0x11  // [player:1][frame:4][packed_input:4]
#define PKTID_ALL_READY      0x12  // []
#define PKTID_SERVER_CONFIG  0x13  // [game-specific payload]
#define PKTID_PLAYER_LEFT    0x14  // [player:1]
#define PKTID_HEARTBEAT      0x15  // []

/*********************************
         Input Packing
*********************************/

// N64 native big-endian: [buttons:16 | stick_x:8 | stick_y:8]
#define NP_PACK_INPUT(buttons, stick_x, stick_y) \
    (((u32)(buttons) << 16) | (((u32)(u8)(stick_x)) << 8) | ((u32)(u8)(stick_y)))

#define NP_UNPACK_BUTTONS(packed)  ((u16)((packed) >> 16))
#define NP_UNPACK_STICK_X(packed)  ((s8)(((packed) >> 8) & 0xFF))
#define NP_UNPACK_STICK_Y(packed)  ((s8)((packed) & 0xFF))

/*********************************
          Constants
*********************************/

#define NP_MAX_PLAYERS      8   // Max online players
#define NP_MAX_LOCAL        4   // Max physical controllers per console
#define NP_INPUT_DELAY_MAX  8   // Max frames of input delay
#define NP_FRAME_SYNC_TIMEOUT 50000 // Poll iterations to wait for bridge response

/*********************************
     SC64 Shared Memory Mode
*********************************/

#define NP_SHM_BASE         0x1FFE1F80
#define NP_MAGIC            0x4E455450  // "NETP"

// Register offsets
#define NP_REG_MAGIC        0x00
#define NP_REG_ENABLE       0x04
#define NP_REG_STATUS       0x08
#define NP_REG_FRAME        0x0C
#define NP_REG_OVERRIDE_0   0x10
#define NP_REG_LOCAL_0      0x20
#define NP_REG_CTRL_MASK    0x30
#define NP_REG_INPUT_DELAY  0x34
#define NP_REG_LOCAL_PLAYER 0x38
#define NP_REG_RNG_SEED     0x3C
#define NP_REG_PLAYER_COUNT 0x40
#define NP_REG_MCU_FRAME_RDY 0x44
#define NP_REG_GAME_MODE    0x48
#define NP_REG_COURSE_ID    0x4C
#define NP_REG_CHAR_SEL_0   0x50
#define NP_REG_RACE_START   0x60
#define NP_REG_DISCONNECT   0x64
#define NP_REG_CC_SELECT    0x68

// Status flags
#define NP_STATUS_N64_READY     0x01
#define NP_STATUS_IN_RACE       0x02
#define NP_STATUS_MENU_READY    0x08

/*********************************
          Netplay State
*********************************/

typedef enum {
    NP_MODE_DISABLED = 0,
    NP_MODE_NETLIB,       // N64-NetLib direct packets (primary)
    NP_MODE_SC64_SHM      // SC64 shared memory bridge (fallback)
} NetplayMode;

// Lobby room info (received from server)
#define NP_MAX_ROOMS        16
#define NP_ROOM_NAME_MAX    20

typedef struct {
    u16 id;
    char name[NP_ROOM_NAME_MAX];
    u8 currentPlayers;
    u8 maxPlayers;
    u8 inGame;             // 1 if room is mid-game
} NetplayRoom;

typedef enum {
    NP_LOBBY_DISCONNECTED = 0,
    NP_LOBBY_CONNECTED,    // Connected to server, not in a room
    NP_LOBBY_IN_ROOM,      // In a room, waiting for game start
    NP_LOBBY_IN_GAME       // Game is active
} NetplayLobbyState;

typedef struct {
    NetplayMode mode;
    // Lobby state
    NetplayLobbyState lobbyState;
    u8 isHost;             // 1 if we created the room
    u16 roomId;            // Current room ID (0 = not in a room)
    NetplayRoom rooms[NP_MAX_ROOMS]; // Cached room list
    u8 roomCount;          // Number of rooms in list
    u8 roomListReceived;   // Server sent room list
    u8 roomJoinError;      // Non-zero = error code from server
    // Player state
    u8 localPlayer;        // First local player's network slot (0-7)
    u8 localPlayerCount;   // Physical controllers on this console (1-4)
    u8 localSlots[NP_MAX_LOCAL]; // Network slot per local controller
    u8 playerCount;        // Total players in session (1-8)
    u8 ctrlMask;           // Bitmask of remote-controlled slots
    u8 enabled;
    u8 inRace;
    u8 allReady;
    u8 connected;
    u32 frameCounter;
    u32 rngSeed;
    u8 inputDelay;
    u8 disconnectMask;
    u32 remoteInputs[NP_MAX_PLAYERS];
    u32 remoteFrames[NP_MAX_PLAYERS];
    u32 inputDelayBuffer[NP_MAX_LOCAL][NP_INPUT_DELAY_MAX];
    u8 inputDelayHead;
    // Game config (populated by game-specific callback)
    u8 cfgMode;
    s16 cfgCourse;
    u8 cfgCC;
    s8 cfgCharacters[NP_MAX_PLAYERS];
    u8 configReceived;
} NetplayState;

/*********************************
     Core API (game-agnostic)
*********************************/

void netplay_init(void);
s32  netplay_detect(void);
void netplay_update(void);
void netplay_apply_controller_overrides(OSContPad *pads);
s32  netplay_wait_for_remote_inputs(void);
void netplay_send_local_input(u16 buttons, s8 stick_x, s8 stick_y);
s32  netplay_wait_for_race_start(void);
void netplay_handle_disconnects(void);
s32  netplay_should_allow_pause(s32 controllerIndex);
s32  netplay_is_active(void);
s32  netplay_is_local_player(s32 playerIndex);
u8   netplay_get_local_player(void);
u8   netplay_get_player_count(void);
u32  netplay_get_rng_seed(void);

// Lobby API (game-agnostic)
void netplay_request_room_list(void);
void netplay_create_room(const char *name, u8 maxPlayers);
void netplay_join_room(u16 roomId);
void netplay_leave_room(void);
s32  netplay_is_host(void);
u8   netplay_get_lobby_state(void);
u8   netplay_get_room_count(void);
NetplayRoom* netplay_get_room(u8 index);

// SC64 PI bus helpers (used by game-specific code for SHM fallback)
u32  np_pi_read(u32 offset);
void np_pi_write(u32 offset, u32 value);

extern NetplayState gNetplayState;

/*********************************
    Game-Specific Callbacks
  (Implement these per game)
*********************************/

// Called when server sends game config. Parse game-specific fields
// from the NetLib read stream into gNetplayState.cfg* fields.
extern void netplay_game_parse_config(size_t size);

// Apply network config to game globals (player count, mode, course, etc.)
extern void netplay_game_setup(void);

// Seed the game's RNG from gNetplayState.rngSeed.
extern void netplay_game_seed_rng(void);

// Host builds and sends game config packet from current menu selections.
extern void netplay_game_send_config(void);

#endif
