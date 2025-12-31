#include "client_ui.h"

#include <string.h>
#include <stdlib.h>

void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, SDL_Color color)
{
    if (!font || !text) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return;
    }
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

void draw_text_centered(SDL_Renderer *ren, TTF_Font *font, const char *text, const SDL_Rect *rect, SDL_Color color)
{
    if (!font || !text || !rect) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text, &w, &h) != 0) return;
    int x = rect->x + (rect->w - w) / 2;
    int y = rect->y + (rect->h - h) / 2;
    draw_text(ren, font, text, x, y, color);
}

void draw_text_left_vcenter(SDL_Renderer *ren, TTF_Font *font, const char *text,
                            const SDL_Rect *rect, int padding_x, SDL_Color color)
{
    if (!font || !text || !rect) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text, &w, &h) != 0) return;
    int x = rect->x + padding_x;
    int y = rect->y + (rect->h - h) / 2;
    draw_text(ren, font, text, x, y, color);
}

int point_in_rect(int x, int y, const SDL_Rect *r)
{
    return x >= r->x && x < (r->x + r->w) && y >= r->y && y < (r->y + r->h);
}

void input_backspace(char *buf)
{
    size_t len = strlen(buf);
    if (len == 0) return;
    buf[len - 1] = '\0';
}

void input_append(char *buf, size_t cap, const char *text, int numeric_only, int allow_dot)
{
    size_t len = strlen(buf);
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) continue;
        if (numeric_only) {
            if (c < '0' || c > '9') {
                if (!(allow_dot && c == '.')) continue;
            }
        }
        if (len + 1 >= cap) break;
        buf[len++] = (char)c;
        buf[len] = '\0';
    }
}

int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || s[0] == '\0' || !end || *end != '\0') return 0;
    *out = (int)v;
    return 1;
}

int parse_float(const char *s, float *out)
{
    char *end = NULL;
    float v = strtof(s, &end);
    if (!s || s[0] == '\0' || !end || *end != '\0') return 0;
    *out = v;
    return 1;
}

int copy_path(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0 || !src) return 0;
    size_t len = strnlen(src, cap);
    if (len >= cap) return 0;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 1;
}

void compute_form_layout(int win_h, int num_fields, int base_row_h, int base_gap,
                         int base_btn_h, int title_h, int top_margin, int bottom_margin,
                         int *row_h, int *gap, int *start_y, int *btn_h)
{
    int r = base_row_h;
    int g = base_gap;
    int b = base_btn_h;
    int top = top_margin;
    int bottom = bottom_margin;

    int total = top + title_h + g + num_fields * r + (num_fields - 1) * g + b + 2 * g + bottom;
    if (total > win_h) {
        int min_r = 24;
        int min_g = 6;
        int min_b = 40;
        int min_top = 8;
        int min_bottom = 8;

        if (top + bottom > min_top + min_bottom) {
            int shrink = (top + bottom) - (min_top + min_bottom);
            int take_top = (top > min_top) ? (top - min_top) : 0;
            int take_bottom = (bottom > min_bottom) ? (bottom - min_bottom) : 0;
            int take_total = take_top + take_bottom;
            if (take_total > 0) {
                top -= (take_top * shrink) / take_total;
                bottom -= (take_bottom * shrink) / take_total;
                if (top < min_top) top = min_top;
                if (bottom < min_bottom) bottom = min_bottom;
            }
        }

        int avail = win_h - (top + title_h + bottom + b + 2 * g);
        int base_block = num_fields * base_row_h + (num_fields - 1) * base_gap;
        if (avail > 0 && base_block > 0) {
            double scale = (double)avail / (double)base_block;
            r = (int)(base_row_h * scale);
            g = (int)(base_gap * scale);
        }
        if (r < min_r) r = min_r;
        if (g < min_g) g = min_g;
        if (b > (int)(base_btn_h * 0.9)) b = (int)(base_btn_h * 0.9);
        if (b < min_b) b = min_b;

        total = top + title_h + g + num_fields * r + (num_fields - 1) * g + b + 2 * g + bottom;
        if (total > win_h) {
            int overflow = total - win_h;
            if (b > min_b) {
                int reduce = b - min_b;
                int take = (overflow < reduce) ? overflow : reduce;
                b -= take;
                overflow -= take;
            }
            if (overflow > 0 && r > min_r) {
                int reduce = r - min_r;
                int take = (overflow / num_fields) + 1;
                if (take > reduce) take = reduce;
                r -= take;
            }
        }
    }

    *row_h = r;
    *gap = g;
    *btn_h = b;
    *start_y = top + title_h + g;
    if (*start_y < 12) *start_y = 12;
}
