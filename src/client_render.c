#include "client_render.h"

void world_to_screen(const ClientState *S, int wx, int wy, int *sx, int *sy)
{
    *sx = (int)((double)wx * (S->win_w - 1) / (double)(S->world_w - 1));
    *sy = (int)((double)wy * (S->win_h - 1) / (double)(S->world_h - 1));
}

void draw_big_point(SDL_Renderer *ren, int x, int y, int r)
{
    for (int i = x-r; i < x+r ;++i)
    {
        for (int k = y-r; k < y+r;++k)
        {
            if ((x-i)*(x-i) + (y-k)*(y-k) <= r*r){
                SDL_RenderDrawPoint(ren,i,k);
            }
        }
    }
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
    draw_obstacles_loop(C, ren, C->obstacles, C->obs_w, C->obs_h, 5, 8);
    SDL_SetRenderTarget(ren, NULL);
    C->obstacles_drawn = 1;
    pthread_mutex_unlock(&C->stats_mtx);
}

void draw_obstacles_loop(ClientState *C, SDL_Renderer *ren,
                         const uint8_t *obs, int grid_w, int grid_h,
                         int offset, int size)
{
    if (!C || !ren || !obs) return;
    if (grid_w <= 0 || grid_h <= 0 || size <= 0) return;

    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    for (int oy = 0; oy < grid_h; oy++) {
        for (int ox = 0; ox < grid_w; ox++) {
            size_t idx = (size_t)oy * (size_t)grid_w + (size_t)ox;
            if (!obs[idx]) continue;
            int sx, sy;
            world_to_screen(C, ox, oy, &sx, &sy);
            SDL_Rect rect = { sx - offset, sy - offset, size, size };
            SDL_RenderDrawRect(ren, &rect);
        }
    }
}
