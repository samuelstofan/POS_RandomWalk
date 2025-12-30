#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "shared.h"
#include <math.h>

#define STEP_FIFO_CAP 4096
#define MENU_DEFAULT_SOCK "/tmp/rwalk.sock"


static int send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += (size_t)w; n -= (size_t)w;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t*)buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += (size_t)r; n -= (size_t)r;
    }
    return 1;
}

static void send_stop(int sockfd) {
    MsgHdr h = { MSG_STOP, 0 };
    send_all(sockfd, &h, sizeof(h));
}

static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

typedef enum {
    MENU_ACT_NONE = 0,
    MENU_ACT_NEW,
    MENU_ACT_JOIN,
    MENU_ACT_REPLAY,
    MENU_ACT_EXIT
} MenuAction;

typedef struct {
    SDL_Rect rect;
    const char *label;
    MenuAction action;
} MenuButton;

typedef struct {
    int world_w;
    int world_h;
    int replications;
    int max_steps;
    float pU, pD, pL, pR;
    char sock_path[256];
    char output_path[256];
} NewSimConfig;

typedef struct {
    int replications;
    char input_path[256];
    char output_path[256];
} ReplayConfig;

typedef struct {
    char sock_path[256];
} JoinConfig;

typedef struct {
    int x, y;
    uint32_t step_index;
} Step;

typedef struct {
    Step buf[STEP_FIFO_CAP];
    int head;   // zapis
    int tail;   // citanie
    int count;
    pthread_mutex_t mtx;
} StepFIFO;

typedef struct {
    int sockfd;
    atomic_int running;

    // world/config
    atomic_int have_welcome;
    int world_w, world_h;
    int max_steps;
    atomic_uint mode;
    int delay_ms;
    int replications;
    atomic_int current_replication;
    atomic_int progress_dirty;

    // position
    pthread_mutex_t pos_mtx;
    int have_pos;
    int x, y;
    int prev_x, prev_y;
    uint32_t step_index;

    // renderer scaling
    int win_w, win_h;

    StepFIFO fifo;

    pthread_mutex_t stats_mtx;
    float *prob_to_center;
    float *avg_steps_to_center;
    int stats_w, stats_h;
    int have_stats;
    int stats_dirty;
    SDL_Texture *stats_tex;
    int show_stats_numbers;
    int show_avg_steps;
    TTF_Font *stats_font;
    int stats_font_size;
    char font_path[256];
} ClientState;

static void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *text, int x, int y, SDL_Color color)
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

static void draw_text_centered(SDL_Renderer *ren, TTF_Font *font, const char *text, const SDL_Rect *rect, SDL_Color color)
{
    if (!font || !text || !rect) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text, &w, &h) != 0) return;
    int x = rect->x + (rect->w - w) / 2;
    int y = rect->y + (rect->h - h) / 2;
    draw_text(ren, font, text, x, y, color);
}

static void draw_text_left_vcenter(SDL_Renderer *ren, TTF_Font *font, const char *text,
                                   const SDL_Rect *rect, int padding_x, SDL_Color color)
{
    if (!font || !text || !rect) return;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text, &w, &h) != 0) return;
    int x = rect->x + padding_x;
    int y = rect->y + (rect->h - h) / 2;
    draw_text(ren, font, text, x, y, color);
}

static int point_in_rect(int x, int y, const SDL_Rect *r)
{
    return x >= r->x && x < (r->x + r->w) && y >= r->y && y < (r->y + r->h);
}

typedef struct {
    const char *label;
    char *buf;
    size_t cap;
    SDL_Rect rect;
    int numeric_only;
    int allow_dot;
} InputField;

static void input_backspace(char *buf)
{
    size_t len = strlen(buf);
    if (len == 0) return;
    buf[len - 1] = '\0';
}

static void input_append(char *buf, size_t cap, const char *text, int numeric_only, int allow_dot)
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

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || s[0] == '\0' || !end || *end != '\0') return 0;
    *out = (int)v;
    return 1;
}

static int parse_float(const char *s, float *out)
{
    char *end = NULL;
    float v = strtof(s, &end);
    if (!s || s[0] == '\0' || !end || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int copy_path(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0 || !src) return 0;
    size_t len = strnlen(src, cap);
    if (len >= cap) return 0;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 1;
}

static void compute_form_layout(int win_h, int num_fields, int base_row_h, int base_gap,
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

static void client_shutdown(ClientState *C, int sockfd, pthread_t net_th,
                            SDL_Texture *canvas, SDL_Renderer *ren,
                            SDL_Window *win, TTF_Font *font)
{
    if (C) {
        atomic_store(&C->running, 0);
    }
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }
    if (net_th) {
        pthread_join(net_th, NULL);
    }
    if (font) TTF_CloseFont(font);
    if (C && C->stats_font) TTF_CloseFont(C->stats_font);
    if (TTF_WasInit()) TTF_Quit();
    if (C) {
        pthread_mutex_destroy(&C->stats_mtx);
        free(C->prob_to_center);
        free(C->avg_steps_to_center);
        if (C->stats_tex) SDL_DestroyTexture(C->stats_tex);
    }
    if (canvas) SDL_DestroyTexture(canvas);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

static int run_new_sim_menu(NewSimConfig *cfg)
{
    if (!cfg) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(new_sim): %s\n", SDL_GetError());
        return 0;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 24);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(new_sim): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(new_sim): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk - New simulation",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(new_sim): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return 0;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(new_sim): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return 0;
    }

    char w_buf[32] = "51";
    char h_buf[32] = "51";
    char rep_buf[32] = "100";
    char k_buf[32] = "100";
    char pu_buf[32] = "0.25";
    char pd_buf[32] = "0.25";
    char pl_buf[32] = "0.25";
    char pr_buf[32] = "0.25";
    char sock_buf[256];
    char out_buf[256];
    snprintf(sock_buf, sizeof(sock_buf), "%s", MENU_DEFAULT_SOCK);
    snprintf(out_buf, sizeof(out_buf), "replication_results.csv");

    InputField fields[10];
    fields[0] = (InputField){ "Socket:", sock_buf, sizeof(sock_buf), {0}, 0, 0 };
    fields[1] = (InputField){ "World width:", w_buf, sizeof(w_buf), {0}, 1, 0 };
    fields[2] = (InputField){ "World height:", h_buf, sizeof(h_buf), {0}, 1, 0 };
    fields[3] = (InputField){ "Replications:", rep_buf, sizeof(rep_buf), {0}, 1, 0 };
    fields[4] = (InputField){ "K (max steps):", k_buf, sizeof(k_buf), {0}, 1, 0 };
    fields[5] = (InputField){ "pU:", pu_buf, sizeof(pu_buf), {0}, 1, 1 };
    fields[6] = (InputField){ "pD:", pd_buf, sizeof(pd_buf), {0}, 1, 1 };
    fields[7] = (InputField){ "pL:", pl_buf, sizeof(pl_buf), {0}, 1, 1 };
    fields[8] = (InputField){ "pR:", pr_buf, sizeof(pr_buf), {0}, 1, 1 };
    fields[9] = (InputField){ "Output file:", out_buf, sizeof(out_buf), {0}, 0, 0 };

    MenuButton buttons[2];
    buttons[0].label = "Launch";
    buttons[0].action = MENU_ACT_NEW;
    buttons[1].label = "Back";
    buttons[1].action = MENU_ACT_EXIT;

    int focus = 0;
    int running = 1;
    int accepted = 0;
    char error_msg[128] = {0};

    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
                    if (point_in_rect(mx, my, &fields[i].rect)) {
                        focus = i;
                        break;
                    }
                }
                for (int i = 0; i < 2; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        if (i == 0) {
                            accepted = 1;
                        } else {
                            accepted = 0;
                        }
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                int total = (int)(sizeof(fields) / sizeof(fields[0])) + 2;
                if (e.key.keysym.sym == SDLK_TAB) {
                    int dir = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                    focus = (focus + dir + total) % total;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    focus = (focus - 1 + total) % total;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    focus = (focus + 1) % total;
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                        input_backspace(fields[focus].buf);
                    }
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    if (focus == (int)(sizeof(fields) / sizeof(fields[0]))) {
                        accepted = 1;
                        running = 0;
                    } else if (focus == (int)(sizeof(fields) / sizeof(fields[0])) + 1) {
                        accepted = 0;
                        running = 0;
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    accepted = 0;
                    running = 0;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                    InputField *f = &fields[focus];
                    input_append(f->buf, f->cap, e.text.text, f->numeric_only, f->allow_dot);
                }
            }
        }

        int content_w = (int)(win_w * 0.6);
        int start_x = (win_w - content_w) / 2;
        int label_w = (int)(content_w * 0.45);
        int input_w = content_w - label_w - 24;
        int row_h = 46;
        int gap = 16;
        int btn_h = 56;
        int start_y = 0;
        int top_margin = (int)(win_h * 0.03);
        int bottom_margin = (int)(win_h * 0.03);
        compute_form_layout(win_h, (int)(sizeof(fields) / sizeof(fields[0])),
                            46, 16, 56, 32, top_margin, bottom_margin,
                            &row_h, &gap, &start_y, &btn_h);

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            fields[i].rect.x = start_x + label_w + 24;
            fields[i].rect.y = start_y + i * (row_h + gap);
            fields[i].rect.w = input_w;
            fields[i].rect.h = row_h;
        }

        int btn_y = fields[9].rect.y + row_h + 2 * gap;
        int btn_w = 220;
        buttons[0].rect.x = start_x + content_w - 2 * btn_w - gap;
        buttons[0].rect.y = btn_y;
        buttons[0].rect.w = btn_w;
        buttons[0].rect.h = btn_h;
        buttons[1].rect.x = start_x + content_w - btn_w;
        buttons[1].rect.y = btn_y;
        buttons[1].rect.w = btn_w;
        buttons[1].rect.h = btn_h;

        SDL_SetRenderDrawColor(ren, 18, 20, 24, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.03), win_w, 32 };
        draw_text_centered(ren, font, "New simulation", &title_rect, title_col);

        SDL_Color label_col = { 210, 210, 215, 255 };
        SDL_Color input_col = { 245, 245, 248, 255 };
        SDL_Color input_bg = { 40, 45, 55, 255 };
        SDL_Color input_bg_focus = { 60, 90, 140, 255 };
        SDL_Color error_col = { 230, 90, 90, 255 };

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            SDL_Rect label_rect = { start_x, fields[i].rect.y, label_w, row_h };
            draw_text_left_vcenter(ren, font, fields[i].label, &label_rect, 0, label_col);

            SDL_Color bg = (focus == i) ? input_bg_focus : input_bg;
            SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
            SDL_RenderFillRect(ren, &fields[i].rect);
            SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
            SDL_RenderDrawRect(ren, &fields[i].rect);
            draw_text_left_vcenter(ren, font, fields[i].buf, &fields[i].rect, 10, input_col);
        }

        for (int i = 0; i < 2; i++) {
            int idx = (int)(sizeof(fields) / sizeof(fields[0])) + i;
            if (focus == idx) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);
            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, input_col);
        }

        if (error_msg[0]) {
            SDL_Rect err_rect = { 0, btn_y + btn_h + 20, win_w, 28 };
            draw_text_centered(ren, font, error_msg, &err_rect, error_col);
        }

        SDL_RenderPresent(ren);

        if (!running && accepted) {
            int w = 0, h = 0, rep = 0, k = 0;
            float pU = 0.0f, pD = 0.0f, pL = 0.0f, pR = 0.0f;
            if (!parse_int(w_buf, &w) || !parse_int(h_buf, &h) ||
                !parse_int(rep_buf, &rep) || !parse_int(k_buf, &k) ||
                !parse_float(pu_buf, &pU) || !parse_float(pd_buf, &pD) ||
                !parse_float(pl_buf, &pL) || !parse_float(pr_buf, &pR)) {
                snprintf(error_msg, sizeof(error_msg), "Check input format.");
                running = 1;
                accepted = 0;
                continue;
            }
            float sum = pU + pD + pL + pR;
            if (w <= 2 || h <= 2 || rep <= 0 || k <= 0 || sum < 0.999f || sum > 1.001f) {
                snprintf(error_msg, sizeof(error_msg), "Invalid values (sum p* = 1).");
                running = 1;
                accepted = 0;
                continue;
            }
            if (sock_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Missing socket.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (out_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Missing output file.");
                running = 1;
                accepted = 0;
                continue;
            }
            cfg->world_w = w;
            cfg->world_h = h;
            cfg->replications = rep;
            cfg->max_steps = k;
            cfg->pU = pU;
            cfg->pD = pD;
            cfg->pL = pL;
            cfg->pR = pR;
            snprintf(cfg->sock_path, sizeof(cfg->sock_path), "%s", sock_buf);
            strncpy(cfg->output_path, out_buf, sizeof(cfg->output_path) - 1);
            cfg->output_path[sizeof(cfg->output_path) - 1] = '\0';
        }
    }

    SDL_StopTextInput();
    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);
    return accepted;
}

static int run_replay_menu(ReplayConfig *cfg)
{
    if (!cfg) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(replay): %s\n", SDL_GetError());
        return 0;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 24);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(replay): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(replay): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk - Relaunch simulation",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(replay): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return 0;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(replay): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return 0;
    }

    char in_buf[256];
    char rep_buf[32] = "100";
    char out_buf[256];
    snprintf(in_buf, sizeof(in_buf), "replication_results.csv");
    snprintf(out_buf, sizeof(out_buf), "replication_results.csv");

    InputField fields[3];
    fields[0] = (InputField){ "Input file:", in_buf, sizeof(in_buf), {0}, 0, 0 };
    fields[1] = (InputField){ "Replication count:", rep_buf, sizeof(rep_buf), {0}, 1, 0 };
    fields[2] = (InputField){ "Output file:", out_buf, sizeof(out_buf), {0}, 0, 0 };

    MenuButton buttons[2];
    buttons[0].label = "Launch";
    buttons[0].action = MENU_ACT_REPLAY;
    buttons[1].label = "Back";
    buttons[1].action = MENU_ACT_EXIT;

    int focus = 0;
    int running = 1;
    int accepted = 0;
    char error_msg[128] = {0};

    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
                    if (point_in_rect(mx, my, &fields[i].rect)) {
                        focus = i;
                        break;
                    }
                }
                for (int i = 0; i < 2; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        accepted = (i == 0);
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                int total = (int)(sizeof(fields) / sizeof(fields[0])) + 2;
                if (e.key.keysym.sym == SDLK_TAB) {
                    int dir = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                    focus = (focus + dir + total) % total;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    focus = (focus - 1 + total) % total;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    focus = (focus + 1) % total;
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                        input_backspace(fields[focus].buf);
                    }
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    if (focus == (int)(sizeof(fields) / sizeof(fields[0]))) {
                        accepted = 1;
                        running = 0;
                    } else if (focus == (int)(sizeof(fields) / sizeof(fields[0])) + 1) {
                        accepted = 0;
                        running = 0;
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    accepted = 0;
                    running = 0;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                    InputField *f = &fields[focus];
                    input_append(f->buf, f->cap, e.text.text, f->numeric_only, f->allow_dot);
                }
            }
        }

        int content_w = (int)(win_w * 0.6);
        int start_x = (win_w - content_w) / 2;
        int label_w = (int)(content_w * 0.45);
        int input_w = content_w - label_w - 24;
        int row_h = 46;
        int gap = 16;
        int btn_h = 56;
        int start_y = 0;
        int top_margin = (int)(win_h * 0.03);
        int bottom_margin = (int)(win_h * 0.03);
        compute_form_layout(win_h, (int)(sizeof(fields) / sizeof(fields[0])),
                            46, 16, 56, 32, top_margin, bottom_margin,
                            &row_h, &gap, &start_y, &btn_h);

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            fields[i].rect.x = start_x + label_w + 24;
            fields[i].rect.y = start_y + i * (row_h + gap);
            fields[i].rect.w = input_w;
            fields[i].rect.h = row_h;
        }

        int btn_y = fields[2].rect.y + row_h + 2 * gap;
        int btn_w = 220;
        buttons[0].rect.x = start_x + content_w - 2 * btn_w - gap;
        buttons[0].rect.y = btn_y;
        buttons[0].rect.w = btn_w;
        buttons[0].rect.h = btn_h;
        buttons[1].rect.x = start_x + content_w - btn_w;
        buttons[1].rect.y = btn_y;
        buttons[1].rect.w = btn_w;
        buttons[1].rect.h = btn_h;

        SDL_SetRenderDrawColor(ren, 18, 20, 24, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.03), win_w, 32 };
        draw_text_centered(ren, font, "Relaunch simulation", &title_rect, title_col);

        SDL_Color label_col = { 210, 210, 215, 255 };
        SDL_Color input_col = { 245, 245, 248, 255 };
        SDL_Color input_bg = { 40, 45, 55, 255 };
        SDL_Color input_bg_focus = { 60, 90, 140, 255 };
        SDL_Color error_col = { 230, 90, 90, 255 };

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            SDL_Rect label_rect = { start_x, fields[i].rect.y, label_w, row_h };
            draw_text_left_vcenter(ren, font, fields[i].label, &label_rect, 0, label_col);

            SDL_Color bg = (focus == i) ? input_bg_focus : input_bg;
            SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
            SDL_RenderFillRect(ren, &fields[i].rect);
            SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
            SDL_RenderDrawRect(ren, &fields[i].rect);
            draw_text_left_vcenter(ren, font, fields[i].buf, &fields[i].rect, 10, input_col);
        }

        for (int i = 0; i < 2; i++) {
            int idx = (int)(sizeof(fields) / sizeof(fields[0])) + i;
            if (focus == idx) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);
            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, input_col);
        }

        if (error_msg[0]) {
            SDL_Rect err_rect = { 0, btn_y + btn_h + 20, win_w, 28 };
            draw_text_centered(ren, font, error_msg, &err_rect, error_col);
        }

        SDL_RenderPresent(ren);

        if (!running && accepted) {
            int rep = 0;
            if (!parse_int(rep_buf, &rep)) {
                snprintf(error_msg, sizeof(error_msg), "Check input format.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (rep <= 0) {
                snprintf(error_msg, sizeof(error_msg), "Replication count > 0.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (in_buf[0] == '\0' || out_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Zadaj vstupny aj vystupny subor.");
                running = 1;
                accepted = 0;
                continue;
            }
            cfg->replications = rep;
            strncpy(cfg->input_path, in_buf, sizeof(cfg->input_path) - 1);
            cfg->input_path[sizeof(cfg->input_path) - 1] = '\0';
            strncpy(cfg->output_path, out_buf, sizeof(cfg->output_path) - 1);
            cfg->output_path[sizeof(cfg->output_path) - 1] = '\0';
        }
    }

    SDL_StopTextInput();
    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);
    return accepted;
}

static int run_join_menu(JoinConfig *cfg)
{
    if (!cfg) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(join): %s\n", SDL_GetError());
        return 0;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 24);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(join): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(join): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk - Pripojenie",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(join): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return 0;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(join): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return 0;
    }

    char sock_buf[256];
    snprintf(sock_buf, sizeof(sock_buf), "%s", MENU_DEFAULT_SOCK);

    InputField fields[1];
    fields[0] = (InputField){ "Socket:", sock_buf, sizeof(sock_buf), {0}, 0, 0 };

    MenuButton buttons[2];
    buttons[0].label = "Join";
    buttons[0].action = MENU_ACT_JOIN;
    buttons[1].label = "Back";
    buttons[1].action = MENU_ACT_EXIT;

    int focus = 0;
    int running = 1;
    int accepted = 0;
    char error_msg[128] = {0};

    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                if (point_in_rect(mx, my, &fields[0].rect)) {
                    focus = 0;
                }
                for (int i = 0; i < 2; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        accepted = (i == 0);
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                int total = 3;
                if (e.key.keysym.sym == SDLK_TAB) {
                    int dir = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                    focus = (focus + dir + total) % total;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    focus = (focus - 1 + total) % total;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    focus = (focus + 1) % total;
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    if (focus == 0) {
                        input_backspace(fields[0].buf);
                    }
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    if (focus == 1) {
                        accepted = 1;
                        running = 0;
                    } else if (focus == 2) {
                        accepted = 0;
                        running = 0;
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    accepted = 0;
                    running = 0;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                if (focus == 0) {
                    input_append(fields[0].buf, fields[0].cap, e.text.text, 0, 0);
                }
            }
        }

        int content_w = (int)(win_w * 0.6);
        int start_x = (win_w - content_w) / 2;
        int label_w = (int)(content_w * 0.35);
        int input_w = content_w - label_w - 24;
        int row_h = 46;
        int gap = 16;
        int btn_h = 56;
        int start_y = 0;
        int top_margin = (int)(win_h * 0.03);
        int bottom_margin = (int)(win_h * 0.03);
        compute_form_layout(win_h, 1, 46, 16, 56, 32, top_margin, bottom_margin,
                            &row_h, &gap, &start_y, &btn_h);

        fields[0].rect.x = start_x + label_w + 24;
        fields[0].rect.y = start_y;
        fields[0].rect.w = input_w;
        fields[0].rect.h = row_h;

        int btn_y = fields[0].rect.y + row_h + 2 * gap;
        int btn_w = 220;
        buttons[0].rect.x = start_x + content_w - 2 * btn_w - gap;
        buttons[0].rect.y = btn_y;
        buttons[0].rect.w = btn_w;
        buttons[0].rect.h = btn_h;
        buttons[1].rect.x = start_x + content_w - btn_w;
        buttons[1].rect.y = btn_y;
        buttons[1].rect.w = btn_w;
        buttons[1].rect.h = btn_h;

        SDL_SetRenderDrawColor(ren, 18, 20, 24, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.03), win_w, 32 };
        draw_text_centered(ren, font, "Pripojenie k simulacii", &title_rect, title_col);

        SDL_Color label_col = { 210, 210, 215, 255 };
        SDL_Color input_col = { 245, 245, 248, 255 };
        SDL_Color input_bg = { 40, 45, 55, 255 };
        SDL_Color input_bg_focus = { 60, 90, 140, 255 };
        SDL_Color error_col = { 230, 90, 90, 255 };

        SDL_Rect label_rect = { start_x, fields[0].rect.y, label_w, row_h };
        draw_text_left_vcenter(ren, font, fields[0].label, &label_rect, 0, label_col);

        SDL_Color bg = (focus == 0) ? input_bg_focus : input_bg;
        SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
        SDL_RenderFillRect(ren, &fields[0].rect);
        SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
        SDL_RenderDrawRect(ren, &fields[0].rect);
        draw_text_left_vcenter(ren, font, fields[0].buf, &fields[0].rect, 10, input_col);

        for (int i = 0; i < 2; i++) {
            int idx = 1 + i;
            if (focus == idx) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);
            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, input_col);
        }

        if (error_msg[0]) {
            SDL_Rect err_rect = { 0, btn_y + btn_h + 20, win_w, 28 };
            draw_text_centered(ren, font, error_msg, &err_rect, error_col);
        }

        SDL_RenderPresent(ren);

        if (!running && accepted) {
            if (sock_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Zadaj socket.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (!copy_path(cfg->sock_path, sizeof(cfg->sock_path), sock_buf)) {
                snprintf(error_msg, sizeof(error_msg), "Socket path too long.");
                running = 1;
                accepted = 0;
                continue;
            }
        }
    }

    SDL_StopTextInput();
    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);
    return accepted;
}

static MenuAction run_main_menu(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(menu): %s\n", SDL_GetError());
        return MENU_ACT_EXIT;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 28);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(menu): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(menu): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk Menu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(menu): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return MENU_ACT_EXIT;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(menu): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return MENU_ACT_EXIT;
    }

    MenuButton buttons[4];
    buttons[0].label = "New simulation";
    buttons[0].action = MENU_ACT_NEW;
    buttons[1].label = "Join simulation";
    buttons[1].action = MENU_ACT_JOIN;
    buttons[2].label = "Relaunch simulation";
    buttons[2].action = MENU_ACT_REPLAY;
    buttons[3].label = "Exit";
    buttons[3].action = MENU_ACT_EXIT;

    int selected = 0;
    int running = 1;
    MenuAction action = MENU_ACT_EXIT;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
                action = MENU_ACT_EXIT;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEMOTION) {
                int mx = e.motion.x;
                int my = e.motion.y;
                for (int i = 0; i < 4; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        selected = i;
                        break;
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                for (int i = 0; i < 4; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        action = buttons[i].action;
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_DOWN) {
                    selected = (selected + 1) % 4;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    selected = (selected + 3) % 4;
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    action = buttons[selected].action;
                    running = 0;
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    action = MENU_ACT_EXIT;
                    running = 0;
                }
            }
        }

        int btn_w = (int)(win_w * 0.6);
        int btn_h = 60;
        int spacing = 16;
        int total_h = 4 * btn_h + 3 * spacing;
        int start_x = (win_w - btn_w) / 2;
        int start_y = (win_h - total_h) / 2;

        for (int i = 0; i < 4; i++) {
            buttons[i].rect.x = start_x;
            buttons[i].rect.y = start_y + i * (btn_h + spacing);
            buttons[i].rect.w = btn_w;
            buttons[i].rect.h = btn_h;
        }

        SDL_SetRenderDrawColor(ren, 20, 22, 26, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.12), win_w, 40 };
        draw_text_centered(ren, font, "Random Walk Simulator", &title_rect, title_col);

        for (int i = 0; i < 4; i++) {
            if (i == selected) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);

            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);

            SDL_Color text_col = { 240, 240, 245, 255 };
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, text_col);
        }

        SDL_Rect hint_rect = { 0, (int)(win_h * 0.9), win_w, 30 };
        SDL_Color hint_col = { 160, 170, 180, 255 };
        draw_text_centered(ren, font, "Enter = confirm, Esc = back", &hint_rect, hint_col);

        SDL_RenderPresent(ren);
    }

    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);

    return action;
}

static void world_to_screen(const ClientState *S, int wx, int wy, int *sx, int *sy)
{
    *sx = (int)((double)wx * (S->win_w - 1) / (double)(S->world_w - 1));
    *sy = (int)((double)wy * (S->win_h - 1) / (double)(S->world_h - 1));
}

static void draw_big_point(SDL_Renderer *ren, int x, int y, int r)
{
    SDL_Rect rect = {
        x - r,
        y - r,
        2*r + 1,
        2*r + 1
    };
    SDL_RenderFillRect(ren, &rect);
}

static void fifo_push(StepFIFO *f, Step s)
{
    pthread_mutex_lock(&f->mtx);

    if (f->count == STEP_FIFO_CAP) {
        f->tail = (f->tail + 1) % STEP_FIFO_CAP;
        f->count--;
    }

    f->buf[f->head] = s;
    f->head = (f->head + 1) % STEP_FIFO_CAP;
    f->count++;

    pthread_mutex_unlock(&f->mtx);
}

static int fifo_pop(StepFIFO *f, Step *out)
{
    pthread_mutex_lock(&f->mtx);

    if (f->count == 0) {
        pthread_mutex_unlock(&f->mtx);
        return 0;
    }

    *out = f->buf[f->tail];
    f->tail = (f->tail + 1) % STEP_FIFO_CAP;
    f->count--;

    pthread_mutex_unlock(&f->mtx);
    return 1;
}

static void *net_thread(void *arg) {
    ClientState *C = (ClientState*)arg;

    while (atomic_load(&C->running)) {
        MsgHdr h;
        int rr = recv_all(C->sockfd, &h, sizeof(h));
        if (rr <= 0) break;

        if (h.len > (1024u * 1024u)) break;

        uint8_t *payload = NULL;
        if (h.len) {
            payload = (uint8_t*)malloc(h.len);
            if (!payload) break;
            rr = recv_all(C->sockfd, payload, h.len);
            if (rr <= 0) {
                free(payload);
                break;
            }
        }

        if (h.type == MSG_WELCOME && h.len == sizeof(MsgWelcome)) {
            MsgWelcome *w = (MsgWelcome*)payload;
            C->world_w = (int)w->world_w;
            C->world_h = (int)w->world_h;
            C->max_steps = (int)w->max_steps;
            C->delay_ms = (int)w->step_delay_ms;
            C->replications = (int)w->replications;
            atomic_store(&C->mode, w->mode);
            atomic_store(&C->have_welcome, 1);

            pthread_mutex_lock(&C->pos_mtx);
            C->have_pos = 0;
            pthread_mutex_unlock(&C->pos_mtx);
        } else if (h.type == MSG_STEP && h.len == sizeof(MsgStep)) {
            MsgStep *s = (MsgStep*)payload;

            Step st;
            st.x = s->x;
            st.y = s->y;
            st.step_index = s->step_index;

            fifo_push(&C->fifo, st);
        } else if (h.type == MSG_MODE && h.len == sizeof(MsgMode)) {
            MsgMode *m = (MsgMode*)payload;
            atomic_store(&C->mode, m->mode);
        } else if (h.type == MSG_PROGRESS && h.len == sizeof(MsgProgress)) {
            MsgProgress *p = (MsgProgress*)payload;
            atomic_store(&C->current_replication, (int)p->current_replication);
            atomic_store(&C->progress_dirty, 1);
        } else if (h.type == MSG_STATS && h.len >= sizeof(MsgStatsHdr)) {
            MsgStatsHdr *sh = (MsgStatsHdr*)payload;
            uint32_t w = sh->world_w;
            uint32_t hgt = sh->world_h;
            size_t count = (size_t)w * (size_t)hgt;
            size_t expected = sizeof(MsgStatsHdr) + count * sizeof(float) * 2u;
            if (w > 0 && hgt > 0 && h.len == expected) {
    float *prob = (float*)(payload + sizeof(MsgStatsHdr));
    float *avg = prob + count;
                pthread_mutex_lock(&C->stats_mtx);
                if (C->stats_w != (int)w || C->stats_h != (int)hgt || !C->prob_to_center || !C->avg_steps_to_center) {
                    free(C->prob_to_center);
                    free(C->avg_steps_to_center);
                    C->prob_to_center = (float*)malloc(count * sizeof(float));
                    C->avg_steps_to_center = (float*)malloc(count * sizeof(float));
                    C->stats_w = (int)w;
                    C->stats_h = (int)hgt;
                }
                if (C->prob_to_center && C->avg_steps_to_center) {
                    memcpy(C->prob_to_center, prob, count * sizeof(float));
                    memcpy(C->avg_steps_to_center, avg, count * sizeof(float));
                    C->have_stats = 1;
                    C->stats_dirty = 1;
                }
                pthread_mutex_unlock(&C->stats_mtx);
            }
        }

        if (payload) free(payload);
    }

    atomic_store(&C->running, 0);
    return NULL;
}

static void send_mode(int sockfd, uint32_t mode) {
    MsgMode m = { .mode = mode };
    MsgHdr h = { MSG_MODE, (uint32_t)sizeof(m) };
    (void)send_all(sockfd, &h, sizeof(h));
    (void)send_all(sockfd, &m, sizeof(m));
}

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

static SDL_Texture *build_stats_texture(SDL_Renderer *ren, ClientState *C)
{
    pthread_mutex_lock(&C->stats_mtx);
    if (!C->have_stats || !C->prob_to_center || !C->avg_steps_to_center || C->stats_w <= 0 || C->stats_h <= 0) {
        pthread_mutex_unlock(&C->stats_mtx);
        return NULL;
    }

    size_t count = (size_t)C->stats_w * (size_t)C->stats_h;
    float *prob = (float*)malloc(count * sizeof(float));
    float *avg = (float*)malloc(count * sizeof(float));
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
    pthread_mutex_unlock(&C->stats_mtx);

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, C->win_w, C->win_h);
    if (!tex) {
        free(prob);
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

    SDL_SetRenderTarget(ren, NULL);
    free(prob);
    free(avg);
    return tex;
}

static int spawn_server(const char *server_path, const char *sock_path,
                        int world_w, int world_h, int delay_ms, int replications, int max_steps,
                        float pU, float pD, float pL, float pR, const char *output_path) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char wbuf[32], hbuf[32], dbuf[32],rbuf[32], kbuf[32], pu[32], pd[32], pl[32], pr[32];
        snprintf(wbuf, sizeof(wbuf), "%d", world_w);
        snprintf(hbuf, sizeof(hbuf), "%d", world_h);
        snprintf(dbuf, sizeof(dbuf), "%d", delay_ms);
        snprintf(rbuf, sizeof(rbuf), "%d", replications);
        snprintf(kbuf, sizeof(kbuf), "%d", max_steps);
        snprintf(pu, sizeof(pu), "%.6f", pU);
        snprintf(pd, sizeof(pd), "%.6f", pD);
        snprintf(pl, sizeof(pl), "%.6f", pL);
        snprintf(pr, sizeof(pr), "%.6f", pR);

        if (output_path && output_path[0]) {
            execl(server_path, server_path, sock_path, wbuf, hbuf, dbuf, rbuf, kbuf, pu, pd, pl, pr, output_path, (char*)NULL);
        } else {
            execl(server_path, server_path, sock_path, wbuf, hbuf, dbuf, rbuf, kbuf, pu, pd, pl, pr, (char*)NULL);
        }
        perror("execl server");
        _exit(127);
    }
    usleep(150 * 1000);
    return 0;
}

int main(int argc, char **argv) {
    // 1) create new sim (spawns server): client --new /tmp/rwalk.sock
    // 2) join existing sim:           client --join /tmp/rwalk.sock
    const char *mode = NULL;
    const char *sock_path = NULL;
    char join_sock[256];
    char new_sock[256];

    int world_w = 51, world_h = 51;
    int delay_ms = 10;
    int replications = 100;
    int max_steps    = 100;
    float pU=0.25f, pD=0.25f, pL=0.25f, pR=0.25f;
    char output_path[256];
    snprintf(output_path, sizeof(output_path), "replication_results.csv");

    if (argc < 3) {
        while (1) {
            MenuAction act = run_main_menu();
            if (act == MENU_ACT_EXIT) return 0;
            if (act == MENU_ACT_REPLAY) {
                ReplayConfig rcfg;
                if (!run_replay_menu(&rcfg)) {
                    continue;
                }
                fprintf(stderr, "Replay mode is not wired yet.\n");
                continue;
            }
            if (act == MENU_ACT_NEW) {
                NewSimConfig cfg;
                if (!run_new_sim_menu(&cfg)) {
                    continue;
                }
                world_w = cfg.world_w;
                world_h = cfg.world_h;
                replications = cfg.replications;
                max_steps = cfg.max_steps;
                pU = cfg.pU;
                pD = cfg.pD;
                pL = cfg.pL;
                pR = cfg.pR;
            copy_path(new_sock, sizeof(new_sock), cfg.sock_path);
                strncpy(output_path, cfg.output_path, sizeof(output_path) - 1);
                output_path[sizeof(output_path) - 1] = '\0';
                mode = "--new";
                sock_path = new_sock;
                break;
            } else {
                JoinConfig jcfg;
                if (!run_join_menu(&jcfg)) {
                    continue;
                }
            copy_path(join_sock, sizeof(join_sock), jcfg.sock_path);
                mode = "--join";
                sock_path = join_sock;
                break;
            }
        }
    } else {
        mode = argv[1];
        sock_path = argv[2];
    }

    const char *server_bin = getenv("SERVER_BIN");
    if (!server_bin) server_bin = "./server";

    uint32_t last_step_index = 0;


    if (strcmp(mode, "--new") == 0) {
        if (spawn_server(server_bin, sock_path, world_w, world_h, delay_ms, replications, max_steps,
                         pU, pD, pL, pR, output_path) != 0) {
            perror("spawn_server");
            return 1;
        }
    } else if (strcmp(mode, "--join") != 0) {
        fprintf(stderr, "Unknown mode %s\n", mode);
        return 2;
    }

    int fd = connect_unix(sock_path);
    if (fd < 0) {
        perror("connect_unix");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        close(fd);
        return 1;
    }

    const char *font_path = NULL;
    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 14);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont: %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
    }

    ClientState C;
    memset(&C, 0, sizeof(C));
    pthread_mutex_init(&C.fifo.mtx, NULL);
    C.fifo.head = C.fifo.tail = C.fifo.count = 0;
    C.sockfd = fd;
    atomic_store(&C.running, 1);
    pthread_mutex_init(&C.pos_mtx, NULL);
    pthread_mutex_init(&C.stats_mtx, NULL);
    atomic_store(&C.mode, MODE_INTERACTIVE);
    atomic_store(&C.have_welcome, 0);
    atomic_store(&C.current_replication, 0);
    atomic_store(&C.progress_dirty, 0);
    C.prob_to_center = NULL;
    C.avg_steps_to_center = NULL;
    C.stats_w = C.stats_h = 0;
    C.have_stats = 0;
    C.stats_dirty = 0;
    C.stats_tex = NULL;
    C.show_stats_numbers = 1;
    C.show_avg_steps = 0;
    C.stats_font = NULL;
    C.stats_font_size = 0;
    C.font_path[0] = '\0';
    C.max_steps = 0;
    if (font_path) {
        strncpy(C.font_path, font_path, sizeof(C.font_path) - 1);
        C.font_path[sizeof(C.font_path) - 1] = '\0';
    }

    C.win_w = 2800; C.win_h = 2400;

    SDL_Window *win = SDL_CreateWindow("Random Walk Client", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, C.win_w, C.win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        close(fd);
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        close(fd);
        return 1;
    }

    int rw, rh;
    SDL_GetRendererOutputSize(ren, &rw, &rh);
    C.win_w = rw;
    C.win_h = rh;


    SDL_Texture *canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, C.win_w, C.win_h);

    if (!canvas) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        close(fd);
        return 1;
    }

    SDL_SetTextureBlendMode(canvas, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
    SDL_SetRenderTarget(ren, canvas);
    SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderTarget(ren, NULL);

    pthread_t th;
    pthread_create(&th, NULL, net_thread, &C);

    int running = 1;
    int start_point_drawn = 0;
    int last_mode = -1;
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "Random Walk Client");
    SDL_SetWindowTitle(win, title_buf);

    static int have_prev = 0;
    static int prev_x = 0, prev_y = 0;


    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)  {
                //send_stop(C.sockfd);    
                running = 0;
            }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                int new_w = e.window.data1;
                int new_h = e.window.data2;
                if (new_w > 0 && new_h > 0) {
                    C.win_w = new_w;
                    C.win_h = new_h;
                    if (canvas) SDL_DestroyTexture(canvas);
                    canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, C.win_w, C.win_h);
                    if (canvas) {
                        SDL_SetTextureBlendMode(canvas, SDL_BLENDMODE_BLEND);
                        SDL_SetRenderTarget(ren, canvas);
                        SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
                        SDL_RenderClear(ren);
                        SDL_SetRenderTarget(ren, NULL);
                    }
                    if (C.stats_tex) {
                        SDL_DestroyTexture(C.stats_tex);
                        C.stats_tex = NULL;
                    }
                    C.stats_dirty = 1;
                }
            }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = 0;
                }
                if (e.key.keysym.sym == SDLK_i) {
                    atomic_store(&C.mode, MODE_INTERACTIVE);
                    send_mode(C.sockfd, MODE_INTERACTIVE);
                }
                if (e.key.keysym.sym == SDLK_s) {
                    atomic_store(&C.mode, MODE_SUMMARY);
                    C.stats_dirty = 1;
                    send_mode(C.sockfd, MODE_SUMMARY);
                }
                if (e.key.keysym.sym == SDLK_n) {
                    C.show_stats_numbers = !C.show_stats_numbers;
                    C.stats_dirty = 1;
                }
                if (e.key.keysym.sym == SDLK_p) {
                    C.show_avg_steps = !C.show_avg_steps;
                    C.stats_dirty = 1;
                }
                if (e.key.keysym.sym == SDLK_c) {
                    SDL_SetRenderTarget(ren, canvas);
                    SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
                    SDL_RenderClear(ren);
                    SDL_SetRenderTarget(ren, NULL);

                    have_prev = 0;
                    start_point_drawn = 0;

                }
                if (e.key.keysym.sym == SDLK_q) {
                    send_stop(C.sockfd);
                    running = 0;
                }
            }
        }

        if (!atomic_load(&C.have_welcome)) {
            SDL_Delay(10);
            continue;
        }

        uint32_t mode_now = atomic_load(&C.mode);

        if ((int)mode_now != last_mode || atomic_exchange(&C.progress_dirty, 0)) {
            int cur = atomic_load(&C.current_replication);
            snprintf(title_buf, sizeof(title_buf),
                     "Random Walk Client - Replication: %d / %d", cur, C.replications);
            SDL_SetWindowTitle(win, title_buf);
            last_mode = (int)mode_now;
        }
        
        if (mode_now == MODE_INTERACTIVE) {
            
            Step st;
            SDL_SetRenderTarget(ren, canvas);

            while (fifo_pop(&C.fifo, &st)) {
                if (st.step_index == 0 || st.step_index < last_step_index) {
                    SDL_SetRenderTarget(ren, canvas);
                    SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
                    SDL_RenderClear(ren);
                    SDL_SetRenderTarget(ren, NULL);
                    have_prev = 0;
                    start_point_drawn = 0;
                }
                last_step_index = st.step_index;

                //printf("Step %u: (%d, %d)\n", st.step_index, st.x, st.y);

                if (!have_prev) {
                    prev_x = st.x;
                    prev_y = st.y;
                    have_prev = 1;
                    if (!start_point_drawn) {
                        int sx, sy;
                        world_to_screen(&C, prev_x, prev_y, &sx, &sy);

                        SDL_SetRenderTarget(ren, canvas);
                        SDL_SetRenderDrawColor(ren, 0, 0, 255, 255);
                        draw_big_point(ren, sx, sy, 4);

                        int cx, cy;
                        world_to_screen(&C, world_h/2, world_h/2, &cx, &cy);
                        SDL_SetRenderDrawColor(ren, 255, 0, 0, 255);
                        draw_big_point(ren, cx, cy, 4);


                        start_point_drawn = 1;
                    }
                    continue;
                }


                int x1, y1, x2, y2;
                world_to_screen(&C, prev_x, prev_y, &x1, &y1);
                world_to_screen(&C, st.x,   st.y,   &x2, &y2);

                if (abs(prev_x - st.x) == 1|| abs(prev_y - st.y) == 1) {
                    SDL_SetRenderDrawColor(ren, 230, 230, 240, 255);
                    SDL_RenderDrawLine(ren, x1, y1, x2, y2);
                }

                prev_x = st.x;
                prev_y = st.y;
            }

            SDL_SetRenderTarget(ren, NULL);
            SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
            SDL_RenderClear(ren);

            SDL_RenderCopy(ren, canvas, NULL, NULL);

            if (have_prev) {
                int sx, sy;
                world_to_screen(&C, prev_x, prev_y, &sx, &sy);

                SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
                draw_big_point(ren, sx, sy, 2);
            }

            SDL_RenderPresent(ren);
        } else if (mode_now == MODE_SUMMARY) {
            if (C.stats_dirty) {
                if (C.stats_tex) {
                    SDL_DestroyTexture(C.stats_tex);
                    C.stats_tex = NULL;
                }
                C.stats_tex = build_stats_texture(ren, &C);
                C.stats_dirty = 0;
            }
            SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
            SDL_RenderClear(ren);
            if (C.stats_tex) {
                SDL_RenderCopy(ren, C.stats_tex, NULL, NULL);
            }
            SDL_RenderPresent(ren);
        }

        
        SDL_Delay(1000/60);
    }

    atomic_store(&C.running, 0);
    client_shutdown(&C, C.sockfd, th, canvas, ren, win, font);
    return 0;
}
