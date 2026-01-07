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

void draw_obstacles(ClientState *C, SDL_Renderer *ren, SDL_Texture *canvas)
{
    if (!C || !ren || !canvas) return;
    if (!C->have_obstacles || C->obstacles_drawn) return;

    pthread_mutex_lock(&C->stats_mtx);
    if (!C->have_obstacles || !C->obstacles ||
        C->obs_w != C->world_w || C->obs_h != C->world_h) {
        pthread_mutex_unlock(&C->stats_mtx);
        return;
    }

    SDL_SetRenderTarget(ren, canvas);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    for (int oy = 0; oy < C->obs_h; oy++) {
        for (int ox = 0; ox < C->obs_w; ox++) {
            size_t idx = (size_t)oy * (size_t)C->obs_w + (size_t)ox;
            if (!C->obstacles[idx]) continue;
            int sx, sy;
            world_to_screen(C, ox, oy, &sx, &sy);
            draw_big_point(ren, sx, sy, 2);
        }
    }
    SDL_SetRenderTarget(ren, NULL);
    C->obstacles_drawn = 1;
    pthread_mutex_unlock(&C->stats_mtx);
}
