#include "client_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "client_render.h"

static SDL_Texture *make_text_texture(SDL_Renderer *ren, TTF_Font *font,
                                      const char *text, SDL_Color color,
                                      int *out_w, int *out_h)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return NULL;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    if (tex) {
        *out_w = surf->w;
        *out_h = surf->h;
    }
    SDL_FreeSurface(surf);
    return tex;
}

static TTF_Font *get_stats_font(ClientState *C, int size)
{
    if (!C->font_path[0]) return NULL;
    if (size < 8) size = 8;
    if (size > 48) size = 48;
    if (C->stats_font && C->stats_font_size == size) return C->stats_font;
    if (C->stats_font) {
        TTF_CloseFont(C->stats_font);
        C->stats_font = NULL;
    }
    C->stats_font = TTF_OpenFont(C->font_path, size);
    if (C->stats_font) {
        C->stats_font_size = size;
    }
    return C->stats_font;
}

SDL_Texture *build_stats_texture(SDL_Renderer *ren, ClientState *C)
{
    pthread_mutex_lock(&C->stats_mtx);
    if (!C->have_stats || !C->prob_to_center || !C->avg_steps_to_center || C->stats_w <= 0 || C->stats_h <= 0) {
        pthread_mutex_unlock(&C->stats_mtx);
        return NULL;
    }

    size_t count = (size_t)C->stats_w * (size_t)C->stats_h;
    float *prob = (float*)malloc(count * sizeof(float));
    float *avg = (float*)malloc(count * sizeof(float));
    uint8_t *obs = NULL;
    if (!prob || !avg) {
        if (prob) free(prob);
        if (avg) free(avg);
        pthread_mutex_unlock(&C->stats_mtx);
        return NULL;
    }
    memcpy(prob, C->prob_to_center, count * sizeof(float));
    memcpy(avg, C->avg_steps_to_center, count * sizeof(float));
    int grid_w = C->stats_w;
    int grid_h = C->stats_h;
    int show_avg = C->show_avg_steps;
    if (C->have_obstacles && C->obstacles &&
        C->obs_w == C->stats_w && C->obs_h == C->stats_h) {
        obs = (uint8_t*)malloc(count * sizeof(uint8_t));
        if (obs) {
            memcpy(obs, C->obstacles, count * sizeof(uint8_t));
        }
    }
    pthread_mutex_unlock(&C->stats_mtx);

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, C->win_w, C->win_h);
    if (!tex) {
        free(prob);
        free(avg);
        return NULL;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    SDL_SetRenderTarget(ren, tex);
    SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
    SDL_RenderClear(ren);

    float cell_w = (float)C->win_w / (float)grid_w;
    float cell_h = (float)C->win_h / (float)grid_h;
    int base_size = (int)(((cell_w < cell_h) ? cell_w : cell_h) * 0.6f);
    TTF_Font *stats_font = get_stats_font(C, base_size);
    int draw_text = (C->show_stats_numbers && stats_font);

    float max_avg_steps = (show_avg && C->max_steps > 0) ? (float)C->max_steps : 0.0f;

    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            size_t idx = (size_t)y * (size_t)grid_w + (size_t)x;
            float p = show_avg ? avg[idx] : prob[idx];
            if (p < 0.0f) p = 0.0f;
            if (!show_avg && p > 1.0f) p = 1.0f;
            uint8_t v = 0;
            if (show_avg) {
                if (p > 0.0f && max_avg_steps > 0.0f) {
                    float norm = p / max_avg_steps;
                    if (norm > 1.0f) norm = 1.0f;
                    v = (uint8_t)((1.0f - norm) * 255.0f);
                }
            } else {
                v = (uint8_t)(p * 255.0f);
            }
            SDL_SetRenderDrawColor(ren, v, v, v, 255);
            int x0 = (int)((float)x * cell_w);
            int y0 = (int)((float)y * cell_h);
            int x1 = (int)((float)(x + 1) * cell_w);
            int y1 = (int)((float)(y + 1) * cell_h);
            SDL_Rect r = { x0, y0, x1 - x0, y1 - y0 };
            SDL_RenderFillRect(ren, &r);

            if (draw_text && p > 0.0f) {
                char tbuf[8];
                snprintf(tbuf, sizeof(tbuf), show_avg ? "%.1f" : "%.2f", p);
                SDL_Color tc = (v > 128) ? (SDL_Color){0, 0, 0, 255} : (SDL_Color){255, 255, 255, 255};
                int tw = 0, th = 0;
                SDL_Texture *tt = make_text_texture(ren, stats_font, tbuf, tc, &tw, &th);
                if (tt) {
                    SDL_Rect td = { x0 + (int)(cell_w - tw) / 2, y0 + (int)(cell_h - th) / 2, tw, th };
                    SDL_RenderCopy(ren, tt, NULL, &td);
                    SDL_DestroyTexture(tt);
                }
            }
        }
    }

    if (obs) {
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        for (int oy = 0; oy < grid_h; oy++) {
            for (int ox = 0; ox < grid_w; ox++) {
                size_t idx = (size_t)oy * (size_t)grid_w + (size_t)ox;
                if (!obs[idx]) continue;
                int sx, sy;
                world_to_screen(C, ox, oy, &sx, &sy);
                draw_big_point(ren, sx, sy, 2);
            }
        }
    }

    SDL_SetRenderTarget(ren, NULL);
    free(prob);
    free(avg);
    free(obs);
    return tex;
}
