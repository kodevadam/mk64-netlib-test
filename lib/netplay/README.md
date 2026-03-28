# N64 Netplay Library

Game-agnostic netplay engine for N64 decomp projects. Supports online multiplayer via USB flashcarts (SC64, 64Drive, EverDrive) using [N64-NetLib](https://github.com/buu342/N64-NetLib).

## Features

- **8 players online**, up to 4 local per console
- **Dynamic split-screen**: local players get viewports, remote players don't
- **Frame-locked input sync** with configurable input delay
- **Automatic flashcart detection** (SC64, 64Drive, EverDrive)
- **SC64 shared memory fallback** for bridge-based setups
- **Disconnect handling** with neutral input fill
- **Pause coordination** (only local players can pause)

## Files

| File | Description | Modify? |
|------|-------------|---------|
| `netplay_core.c` | Engine: connection, input relay, frame sync, overrides | No |
| `netplay_core.h` | API, state struct, packet IDs, constants | No |
| `netlib.c` | N64-NetLib packet library | No |
| `netlib.h` | N64-NetLib API header | No |
| `usb.c` | UNFLoader USB driver (SC64/64Drive/EverDrive) | No |
| `usb.h` | USB API header | No |

## Integration (5 steps)

### 1. Copy files into your decomp

```
cp lib/netplay/*.c lib/netplay/*.h your_decomp/src/netplay/
```

Add `src/netplay` to your Makefile's source directories.

### 2. Create your game hook file (`netplay_yourgame.c`)

Implement 4 callbacks (see `src/netplay/netplay_mk64.c` as reference):

```c
#include "netplay_core.h"
#include "netlib.h"

// Parse game config from server packet
void netplay_game_parse_config(size_t size) {
    uint8_t mode;
    uint16_t level;
    uint32_t seed;

    netlib_readbyte(&mode);
    netlib_readword(&level);
    // ... read your game's config fields
    netlib_readdword(&seed);

    gNetplayState.cfgMode = mode;
    gNetplayState.cfgCourse = (s16)level;
    gNetplayState.rngSeed = seed;
    gNetplayState.configReceived = TRUE;
}

// Apply config to your game's globals
void netplay_game_setup(void) {
    your_game_player_count = gNetplayState.playerCount;
    your_game_level = gNetplayState.cfgCourse;
    // Set screen mode based on localPlayerCount, not total playerCount
    if (gNetplayState.localPlayerCount == 1)
        your_game_screen_mode = FULLSCREEN;
    // ...
}

// Seed your game's RNG
void netplay_game_seed_rng(void) {
    your_game_rng_seed = gNetplayState.rngSeed;
}

// Host sends config from menu selections
void netplay_game_send_config(void) {
    if (gNetplayState.localPlayer != 0) return; // host only
    netlib_start(PKTID_GAME_CONFIG);
    netlib_writebyte((uint8_t)your_game_mode);
    netlib_writeword((uint16_t)your_game_level);
    netlib_writedword((u32)osGetCount()); // RNG seed
    netlib_sendtoserver();
}
```

### 3. Hook into your game loop

```c
// In your game's init (after osContInit):
netplay_init();
netplay_detect();
if (netplay_is_active()) {
    gControllerBits |= (u8)((1 << netplay_get_player_count()) - 1);
}

// In your controller read function (after osContGetReadData):
if (netplay_is_active()) {
    netplay_update();
    netplay_apply_controller_overrides(gControllerPads);
}

// In your game loop (before game state update, during gameplay):
if (netplay_is_active() && in_gameplay) {
    netplay_wait_for_remote_inputs();
}
```

### 4. Hook game-specific points

```c
// In your match/race setup function:
if (netplay_is_active()) {
    netplay_game_send_config();  // host sends
    netplay_game_setup();        // all apply
    netplay_game_seed_rng();     // sync RNG
}

// In your match start gate/countdown:
if (netplay_is_active() && !netplay_wait_for_race_start()) {
    return; // hold until all players synced
}

// In your pause handler:
if (netplay_is_active() && !netplay_should_allow_pause(controller_index)) {
    // skip — remote player can't pause locally
}
```

### 5. Set up the server

```bash
cd server/
# Edit GamePacketHandler.java packet IDs to match your game
ant
java -jar build/jar/N64NetplayServer.jar --port 6464 --max-players 8
```

## Architecture

```
Console A (1-4 local)          Server              Console B (1-4 local)
  N64-NetLib USB  ──────────►  Java  ◄──────────  N64-NetLib USB
  netplay_core.c               routes              netplay_core.c
  netplay_game.c               packets             netplay_game.c
  controller override          between             controller override
  frame sync                   clients             frame sync
```

Each console:
- Reads physical controllers for its local players
- Sends packed inputs to server via `netlib_broadcast()`
- Receives remote inputs via `pkt_remote_input` callback
- Fills `gControllerPads[]` with local + remote data
- Sets screen mode based on local player count only
