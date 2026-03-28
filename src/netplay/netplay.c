/**
 * netplay.c — MK64 netplay wrappers.
 *
 * Thin wrappers that call the game-specific implementations in
 * netplay_mk64.c. These are the functions referenced from
 * main.c, code_800029B0.c, and race_logic.c.
 */

#include "netplay.h"

void netplay_setup_game(void) {
    if (!netplay_is_active()) return;
    netplay_game_setup();
}

void netplay_seed_rng(void) {
    if (!netplay_is_active()) return;
    netplay_game_seed_rng();
}

void netplay_send_game_config(void) {
    if (!netplay_is_active()) return;
    netplay_game_send_config();
}
