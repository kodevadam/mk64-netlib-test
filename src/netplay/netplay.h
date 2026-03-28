/**
 * netplay.h — MK64 netplay header.
 *
 * Thin wrapper over netplay_core.h that adds MK64-specific declarations.
 * Other decomp games would have their own netplay.h with their own
 * game-specific functions wrapping the same core.
 */

#ifndef _NETPLAY_H_
#define _NETPLAY_H_

#include "netplay_core.h"

// MK64-specific: called from setup_race() in code_800029B0.c
void netplay_setup_game(void);
void netplay_seed_rng(void);
void netplay_send_game_config(void);

// Remap camera/player pointers so viewports follow local players.
// Call after setup_race() completes (after spawn_players).
void netplay_remap_cameras(void);

// Write network inputs into extended controllers for players 5-8.
// Call once per frame after read_controllers.
void netplay_update_extended_controllers(void);

// Get the Controller struct for any player slot (0-7).
// Slots 0-3 return gControllers[], slots 4-7 return netplay extended.
struct Controller* netplay_get_controller(s32 slot);

// Silent matchmaking — auto create/join room on boot.
void netplay_auto_matchmake(void);

// Simple text results overlay for all players.
void netplay_render_results(void);

#endif
