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
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

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

    // latest position + previous for drawing line
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
        // zahod najstarsi krok
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
                        float pU, float pD, float pL, float pR) {
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

        execl(server_path, server_path, sock_path, wbuf, hbuf, dbuf, rbuf, kbuf, pu, pd, pl, pr, (char*)NULL);
        perror("execl server");
        _exit(127);
    }
    usleep(150 * 1000);
    return 0;
}

int main(int argc, char **argv) {
    // 1) create new sim (spawns server): client --new /tmp/rwalk.sock
    // 2) join existing sim:           client --join /tmp/rwalk.sock
    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  %s --new  <sock_path>\n"
            "  %s --join <sock_path>\n"
            "Env: SERVER_BIN=./server (optional)\n",
            argv[0], argv[0]);
        return 2;
    }

    const char *mode = argv[1];
    const char *sock_path = argv[2];

    const char *server_bin = getenv("SERVER_BIN");
    if (!server_bin) server_bin = "./server";

    int world_w = 51, world_h = 51;
    int delay_ms = 10;
    int replications = 100;
    int max_steps    = 100;    
    float pU=0.25f, pD=0.25f, pL=0.25f, pR=0.25f;

    uint32_t last_step_index = 0;


    if (strcmp(mode, "--new") == 0) {
        if (spawn_server(server_bin, sock_path, world_w, world_h, delay_ms, replications, max_steps, pU, pD, pL, pR) != 0) {
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


    while (running && atomic_load(&C.running)) {
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
                    //send_stop(C.sockfd);
                    running = 0;
                }
                if (e.key.keysym.sym == SDLK_i) send_mode(C.sockfd, MODE_INTERACTIVE);
                if (e.key.keysym.sym == SDLK_s) send_mode(C.sockfd, MODE_SUMMARY);
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
    shutdown(C.sockfd, SHUT_RDWR);
    close(C.sockfd); 
    pthread_join(th, NULL);
    if (font) TTF_CloseFont(font);
    if (C.stats_font) TTF_CloseFont(C.stats_font);
    TTF_Quit();
    pthread_mutex_destroy(&C.stats_mtx);
    free(C.prob_to_center);
    free(C.avg_steps_to_center);
    if (C.stats_tex) SDL_DestroyTexture(C.stats_tex);
    SDL_DestroyTexture(canvas);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
