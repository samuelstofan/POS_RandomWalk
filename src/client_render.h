#pragma once

#include "client_types.h"

void world_to_screen(const ClientState *S, int wx, int wy, int *sx, int *sy);
void draw_big_point(SDL_Renderer *ren, int x, int y, int r);
