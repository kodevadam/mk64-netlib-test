#ifndef _NETPLAY_H_
#define _NETPLAY_H_

#include <ultra64.h>
#include <common_structs.h>

/*********************************
        SC64 Shared Memory
    Register Map for Netplay
*********************************/

// SC64 shared memory base address (N64 side, uncached via PI bus)
// FPGA address: 0x05001F80, accessible from N64 at 0x1FFE1F80
#define NP_SHM_BASE         0x1FFE1F80
#define NP_SHM_SIZE         128

// Register offsets from NP_SHM_BASE
#define NP_REG_MAGIC        0x00  // R   - 0x4E455450 ("NETP") when bridge is ready
#define NP_REG_ENABLE       0x04  // RW  - 0=disabled, 1=enabled
#define NP_REG_STATUS       0x08  // RW  - Status flags
#define NP_REG_FRAME        0x0C  // RW  - Current frame counter
#define NP_REG_OVERRIDE_0   0x10  // R   - Remote controller slot 0
#define NP_REG_OVERRIDE_1   0x14  // R   - Remote controller slot 1
#define NP_REG_OVERRIDE_2   0x18  // R   - Remote controller slot 2
#define NP_REG_OVERRIDE_3   0x1C  // R   - Remote controller slot 3
#define NP_REG_LOCAL_0      0x20  // W   - Local controller slot 0 input
#define NP_REG_LOCAL_1      0x24  // W   - Local controller slot 1 input
#define NP_REG_LOCAL_2      0x28  // W   - Local controller slot 2 input
#define NP_REG_LOCAL_3      0x2C  // W   - Local controller slot 3 input
#define NP_REG_CTRL_MASK    0x30  // R   - Bitmask: which slots are remote-controlled
#define NP_REG_INPUT_DELAY  0x34  // R   - Frame delay for input sync
#define NP_REG_LOCAL_PLAYER 0x38  // R   - Which slot is the local player (0-3)
#define NP_REG_RNG_SEED     0x3C  // R   - RNG seed from server
#define NP_REG_PLAYER_COUNT 0x40  // R   - Number of players in session (1-4)
#define NP_REG_MCU_FRAME_RDY 0x44 // R   - Latest frame number with remote data ready
#define NP_REG_GAME_MODE    0x48  // R   - Game mode (VERSUS=2, BATTLE=3)
#define NP_REG_COURSE_ID    0x4C  // R   - Course ID to play
#define NP_REG_CHAR_SEL_0   0x50  // R   - Character selection for player 0
#define NP_REG_CHAR_SEL_1   0x54  // R   - Character selection for player 1
#define NP_REG_CHAR_SEL_2   0x58  // R   - Character selection for player 2
#define NP_REG_CHAR_SEL_3   0x5C  // R   - Character selection for player 3
#define NP_REG_RACE_START   0x60  // RW  - Race start sync flag
#define NP_REG_DISCONNECT   0x64  // R   - Bitmask of disconnected players
#define NP_REG_CC_SELECT    0x68  // R   - CC selection (0=50cc, 1=100cc, 2=150cc)

// Magic value written by bridge when ready
#define NP_MAGIC            0x4E455450  // "NETP"

// Status flags (NP_REG_STATUS)
#define NP_STATUS_N64_READY     0x01  // N64 has initialized netplay
#define NP_STATUS_IN_RACE       0x02  // N64 is currently in a race
#define NP_STATUS_FRAME_DONE    0x04  // N64 finished processing current frame
#define NP_STATUS_MENU_READY    0x08  // N64 is ready to start (menu bypassed)

// Race start sync values (NP_REG_RACE_START)
#define NP_RACE_NOT_STARTED  0
#define NP_RACE_ALL_READY    1  // Bridge sets when all players ready
#define NP_RACE_COUNTDOWN    2  // Race countdown in progress
#define NP_RACE_RUNNING      3  // Race is active

// Input format: [buttons:16 | stick_x:8 | stick_y:8] (N64 native big-endian)
#define NP_PACK_INPUT(buttons, stick_x, stick_y) \
    (((u32)(buttons) << 16) | (((u32)(u8)(stick_x)) << 8) | ((u32)(u8)(stick_y)))

#define NP_UNPACK_BUTTONS(packed)  ((u16)((packed) >> 16))
#define NP_UNPACK_STICK_X(packed)  ((s8)(((packed) >> 8) & 0xFF))
#define NP_UNPACK_STICK_Y(packed)  ((s8)((packed) & 0xFF))

// Frame sync timeout (in game loop iterations, ~50ms at 30fps)
#define NP_FRAME_SYNC_TIMEOUT 3

// Max players supported over netplay
#define NP_MAX_PLAYERS 4

/*********************************
          Netplay State
*********************************/

typedef enum {
    NP_MODE_DISABLED = 0, // No netplay, normal local play
    NP_MODE_SC64_SHM,     // SC64 shared memory mode (bridge handles network)
    NP_MODE_NETLIB        // Direct N64-NetLib USB packets (future)
} NetplayMode;

// Input delay ring buffer size (max frames of delay)
#define NP_INPUT_DELAY_MAX 8

typedef struct {
    NetplayMode mode;
    u8 localPlayer;       // Which player slot is ours (0-3)
    u8 playerCount;       // Total number of players in session
    u8 ctrlMask;          // Bitmask of remote-controlled slots
    u8 enabled;           // Whether netplay is currently active
    u8 inRace;            // Whether we're currently racing
    u32 frameCounter;     // Current frame number
    u32 rngSeed;          // Synchronized RNG seed
    u8 inputDelay;        // Input delay frames
    u8 disconnectMask;    // Bitmask of disconnected players
    u8 raceStartSync;     // Race start sync state
    u32 remoteInputs[NP_MAX_PLAYERS]; // Cached remote controller inputs
    // Input delay ring buffer for local input
    u32 inputDelayBuffer[NP_INPUT_DELAY_MAX];
    u8 inputDelayHead;    // Write position in ring buffer
} NetplayState;

/*********************************
       Function Declarations
*********************************/

// Initialization
void netplay_init(void);
s32  netplay_detect(void);

// Per-frame update (call before controller processing)
void netplay_update(void);

// Controller override - called from read_controllers
void netplay_apply_controller_overrides(OSContPad *pads);

// Frame sync - returns 1 when remote inputs are ready, 0 if timed out
s32  netplay_wait_for_remote_inputs(void);

// Send local input to bridge via shared memory
void netplay_send_local_input(u16 buttons, s8 stick_x, s8 stick_y);

// RNG seed for synchronization
u32  netplay_get_rng_seed(void);
void netplay_seed_rng(void);

// Force game setup from bridge config (player count, mode, characters, course)
void netplay_setup_game(void);

// Race start sync - returns 1 when all players ready
s32  netplay_wait_for_race_start(void);

// Disconnect check - fills disconnected slots with neutral input
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
