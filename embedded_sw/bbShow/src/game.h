/******************************************************************************
*
* @file game.h: "STARFALL" - the high-load danmaku demo.
*
* The RISC-V core only keeps the game state (entities, patterns, collision) and
* submits one BitBlt task per visible sprite.  Every pixel on screen - the
* scrolling sky, the ships, all bullets, the explosions and the translucent HUD
* panels - is written by the BitBlt engine.
*
******************************************************************************/
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

void game_init(void);          /* after art_build(), builds the star field */
void game_reset(void);         /* new run                                  */
void game_update(void);        /* one frame of logic                       */
void game_draw(void);          /* submit one frame of BitBlt tasks         */
void game_key(int ch);         /* UART key while the scene is active       */
void game_tick(void);          /* once per second (frame-rate governor)    */
extern int game_bisect_mode;   /* 0 normal, 1 bg fills only, 2 sprites only, 3 grey full page */
/* out[] = { player hull, boss hull (-1 = no boss), deaths, foes } */
void game_status(int *out);

/* 1 once, after the STAGE CLEAR card has been shown to the end */
int  game_take_scene_request(void);

#endif
