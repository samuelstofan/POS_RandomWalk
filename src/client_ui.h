#pragma once

#include "client_types.h"

void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, SDL_Color color);
void draw_text_centered(SDL_Renderer *ren, TTF_Font *font, const char *text, const SDL_Rect *rect, SDL_Color color);
void draw_text_left_vcenter(SDL_Renderer *ren, TTF_Font *font, const char *text,
                            const SDL_Rect *rect, int padding_x, SDL_Color color);
int point_in_rect(int x, int y, const SDL_Rect *r);
void input_backspace(char *buf);
void input_append(char *buf, size_t cap, const char *text, int numeric_only, int allow_dot);
int parse_int(const char *s, int *out);
int parse_float(const char *s, float *out);
int copy_path(char *dst, size_t cap, const char *src);
void compute_form_layout(int win_h, int num_fields, int base_row_h, int base_gap,
                         int base_btn_h, int title_h, int top_margin, int bottom_margin,
                         int *row_h, int *gap, int *start_y, int *btn_h);
