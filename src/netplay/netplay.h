#ifndef _NETPLAY_H_
#define _NETPLAY_H_

#include <ultra64.h>
#include <common_structs.h>

/*********************************
       NetLib Packet Types

  Sent between N64 clients via
  the N64-NetLib Java server.
  Works on SC64, 64Drive, and
  EverDrive flashcarts.
*********************************/

// Client → Server packets
#define PKTID_CONNECT        0x00  // Request to join, no payload
#define PKTID_PLAYER_INPUT   0x01  // Controller input: [frame:4][packed_input:4]
#define PKTID_READY          0x02  // Signal ready for race start
#define PKTID_GAME_CONFIG    0x03  // Host sends game setup to server
#define PKTID_HEARTBEAT_ACK  0x04  // Response to server heartbeat

// Server → Client packets
#define PKTID_ASSIGN_PLAYER  0x10  // Server assigns player slot: [player_num:1][player_count:1]
#define PKTID_REMOTE_INPUT   0x11  // Another player's input: [player:1][frame:4][packed_input:4]
#define PKTID_ALL_READY      0x12  // All players ready, start countdown
#define PKTID_SERVER_CONFIG  0x13  // Game config from host: [mode:1][course:2][cc:1][char0-3:4][rng_seed:4][input_delay:1]
#define PKTID_PLAYER_LEFT    0x14  // A player disconnected: [player:1]
#define PKTID_HEARTBEAT      0x15  // Server keepalive

// Input format: [buttons:16 | stick_x:8 | stick_y:8] (N64 native big-endian)
#define NP_PACK_INPUT(buttons, stick_x, stick_y) \
    (((u32)(buttons) << 16) | (((u32)(u8)(stick_x)) << 8) | ((u32)(u8)(stick_y)))

#define NP_UNPACK_BUTTONS(packed)  ((u16)((packed) >> 16))
#define NP_UNPACK_STICK_X(packed)  ((s8)(((packed) >> 8) & 0xFF))
#define NP_UNPACK_STICK_Y(packed)  ((s8)((packed) & 0xFF))

// Max players supported over netplay
#define NP_MAX_PLAYERS 4

// Input delay ring buffer size (max frames of delay)
#define NP_INPUT_DELAY_MAX 8

// Frame sync timeout (poll iterations before giving up)
#define NP_FRAME_SYNC_TIMEOUT 3

/*********************************
     SC64 Shared Memory Mode
   (Alternative: bridge-based)
*********************************/

// SC64 shared memory base address (N64 side, PI bus)
#define NP_SHM_BASE         0x1FFE1F80

// Register offsets (see netplay.c for full documentation)
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

#define NP_MAGIC            0x4E455450  // "NETP"
#define NP_STATUS_N64_READY     0x01
#define NP_STATUS_IN_RACE       0x02
#define NP_STATUS_MENU_READY    0x08

/*********************************
          Netplay State
*********************************/

typedef enum {
    NP_MODE_DISABLED = 0, // No netplay, normal local play
    NP_MODE_NETLIB,       // N64-NetLib direct packets (primary)
    NP_MODE_SC64_SHM      // SC64 shared memory bridge (alternative)
} NetplayMode;

typedef struct {
    NetplayMode mode;
    u8 localPlayer;       // Which player slot is ours (0-3)
    u8 playerCount;       // Total number of players in session
    u8 ctrlMask;          // Bitmask of remote-controlled slots
    u8 enabled;           // Whether netplay is currently active
    u8 inRace;            // Whether we're currently racing
    u8 allReady;          // All players ready for race start
    u8 connected;         // Successfully connected to server
    u32 frameCounter;     // Current frame number
    u32 rngSeed;          // Synchronized RNG seed
    u8 inputDelay;        // Input delay frames
    u8 disconnectMask;    // Bitmask of disconnected players
    u32 remoteInputs[NP_MAX_PLAYERS]; // Cached remote controller inputs
    u32 remoteFrames[NP_MAX_PLAYERS]; // Frame number for each remote input
    // Input delay ring buffer for local input
    u32 inputDelayBuffer[NP_INPUT_DELAY_MAX];
    u8 inputDelayHead;    // Write position in ring buffer
    // Game config (received from server/host)
    u8 cfgMode;           // Game mode (VERSUS, BATTLE, etc.)
    s16 cfgCourse;        // Course ID
    u8 cfgCC;             // CC selection
    s8 cfgCharacters[NP_MAX_PLAYERS]; // Character selections
    u8 configReceived;    // Whether server config has arrived
} NetplayState;

/*********************************
       Function Declarations
*********************************/

// Initialization - tries NetLib first, falls back to SC64 shared memory
void netplay_init(void);
s32  netplay_detect(void);

// Per-frame update - polls NetLib or reads SC64 shared memory
void netplay_update(void);

// Controller override - called from read_controllers
void netplay_apply_controller_overrides(OSContPad *pads);

// Frame sync - returns 1 when remote inputs are ready, 0 if timed out
s32  netplay_wait_for_remote_inputs(void);

// Send local input
void netplay_send_local_input(u16 buttons, s8 stick_x, s8 stick_y);

// RNG
u32  netplay_get_rng_seed(void);
void netplay_seed_rng(void);

// Force game setup from network config
void netplay_setup_game(void);

// Race start sync - returns 1 when all players ready
s32  netplay_wait_for_race_start(void);

// Disconnect check
void netplay_handle_disconnects(void);

// Pause coordination - only local player can pause
s32  netplay_should_allow_pause(s32 controllerIndex);

// State queries
s32  netplay_is_active(void);
s32  netplay_is_local_player(s32 playerIndex);
u8   netplay_get_local_player(void);
u8   netplay_get_player_count(void);

extern NetplayState gNetplayState;

#endif
