#ifndef GAME_H
#define GAME_H

#include "input.h"

/* Loads assets, sets the initial state (title screen). Returns 0 on ok. */
int game_init(void);
void game_seed(unsigned seed);
/* Re-read the tilesets (honoring the current assets override) and refresh
 * the video bank in place — a live skin switch. Game state is untouched. */
int game_reload_tiles(void);
/* One 59.73Hz tick: advance state machine, update video bank/BG/sprites. */
void game_update(Input in);

#endif
