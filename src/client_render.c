#include "client_render.h"

void world_to_screen(const ClientState *S, int wx, int wy, int *sx, int *sy)
{
    *sx = (int)((double)wx * (S->win_w - 1) / (double)(S->world_w - 1));
    *sy = (int)((double)wy * (S->win_h - 1) / (double)(S->world_h - 1));
}

void draw_big_point(SDL_Renderer *ren, int x, int y, int r)
{
    SDL_Rect rect = {
        x - r,
        y - r,
        2*r + 1,
        2*r + 1
    };
    SDL_RenderFillRect(ren, &rect);
}
