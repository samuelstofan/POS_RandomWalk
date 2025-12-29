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
    atomic_uint mode;
    int delay_ms;

    // latest position + previous for drawing line
    pthread_mutex_t pos_mtx;
    int have_pos;
    int x, y;
    int prev_x, prev_y;
    uint32_t step_index;

    // renderer scaling
    int win_w, win_h;

    StepFIFO fifo;
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

        if (h.len > 4096) break;

        uint8_t payload[4096];
        if (h.len) {
            rr = recv_all(C->sockfd, payload, h.len);
            if (rr <= 0) break;
        }

        if (h.type == MSG_WELCOME && h.len == sizeof(MsgWelcome)) {
            MsgWelcome *w = (MsgWelcome*)payload;
            C->world_w = (int)w->world_w;
            C->world_h = (int)w->world_h;
            C->delay_ms = (int)w->step_delay_ms;
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
        }
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
    int replications = 1;
    int max_steps    = 10;    
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

    ClientState C;
    memset(&C, 0, sizeof(C));
    pthread_mutex_init(&C.fifo.mtx, NULL);
    C.fifo.head = C.fifo.tail = C.fifo.count = 0;
    C.sockfd = fd;
    atomic_store(&C.running, 1);
    pthread_mutex_init(&C.pos_mtx, NULL);
    atomic_store(&C.mode, MODE_INTERACTIVE);
    atomic_store(&C.have_welcome, 0);

    C.win_w = 1800; C.win_h = 1400;

    SDL_Window *win = SDL_CreateWindow("Random Walk Client", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, C.win_w, C.win_h, SDL_WINDOW_SHOWN);
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


    SDL_Texture *canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, C.win_w, C.win_h    );

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

    static int have_prev = 0;
    static int prev_x = 0, prev_y = 0;


    while (running && atomic_load(&C.running)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)  {
                //send_stop(C.sockfd);    
                running = 0;
            }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    //send_stop(C.sockfd);
                    running = 0;
                }
                if (e.key.keysym.sym == SDLK_i) send_mode(C.sockfd, MODE_INTERACTIVE);
                if (e.key.keysym.sym == SDLK_s) send_mode(C.sockfd, MODE_SUMMARY);
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
        } else if (mode_now == MODE_SUMMARY) {
            
        }

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
        SDL_Delay(1000/60);
    }

    atomic_store(&C.running, 0);
    shutdown(C.sockfd, SHUT_RDWR);
    close(C.sockfd); 
    pthread_join(th, NULL);
    SDL_DestroyTexture(canvas);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
