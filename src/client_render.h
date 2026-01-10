#pragma once

#include "client_types.h"

void world_to_screen(const ClientState *S, int wx, int wy, int *sx, int *sy);
void draw_big_point(SDL_Renderer *ren, int x, int y, int r);
void draw_obstacles(ClientState *C, SDL_Renderer *ren, SDL_Texture *canvas);
void draw_obstacles_loop(ClientState *C, SDL_Renderer *ren,
                         const uint8_t *obs, int grid_w, int grid_h,
                         int offset, int size);
