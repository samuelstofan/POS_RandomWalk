#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "client_types.h"
#include "client_ui.h"
#include "client_menu.h"
#include "client_replay.h"
#include "client_spawn.h"
#include "client_net.h"
#include "client_replay.h"
#include "client_fifo.h"
#include "client_render.h"
#include "client_stats.h"
#include "shared.h"

int main(int argc, char **argv) {
    // 1) create new sim (spawns server): client --new /tmp/rwalk.sock
    // 2) join existing sim:           client --join /tmp/rwalk.sock
    int force_menu = 0;
    for (;;) {
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
        float *replay_prob = NULL;
        float *replay_avg = NULL;
        int replay_w = 0;
        int replay_h = 0;
        int replay_replications = 0;
        int have_replay_stats = 0;
        float *display_prob = NULL;
        float *display_avg = NULL;
        int restart_to_menu = 0;

        if (argc < 3 || force_menu) {
            while (1) {
                MenuAction act = run_main_menu();
                if (act == MENU_ACT_EXIT) return 0;
                if (act == MENU_ACT_REPLAY) {
                    ReplayConfig rcfg;
                    if (!run_replay_menu(&rcfg)) {
                        continue;
                    }
                int file_w = 0, file_h = 0, file_k = 0, file_reps = 0;
                float file_pU = 0.0f, file_pD = 0.0f, file_pL = 0.0f, file_pR = 0.0f;
                float *file_prob = NULL;
                float *file_avg = NULL;
                if (!load_replay_file(rcfg.input_path, &file_w, &file_h, &file_k,
                                      &file_pU, &file_pD, &file_pL, &file_pR,
                                      &file_reps, new_sock, sizeof(new_sock),
                                      &file_prob, &file_avg)) {
                    fprintf(stderr, "Replay: failed to load %s\n", rcfg.input_path);
                    continue;
                }
                    world_w = file_w;
                    world_h = file_h;
                    max_steps = file_k;
                    pU = file_pU;
                    pD = file_pD;
                    pL = file_pL;
                    pR = file_pR;
                    replications = rcfg.replications;
                    strncpy(output_path, rcfg.output_path, sizeof(output_path) - 1);
                    output_path[sizeof(output_path) - 1] = '\0';
                if (new_sock[0] == '\0') {
                    copy_path(new_sock, sizeof(new_sock), MENU_DEFAULT_SOCK);
                }
                mode = "--new";
                sock_path = new_sock;
                    replay_prob = file_prob;
                    replay_avg = file_avg;
                    replay_w = file_w;
                    replay_h = file_h;
                    replay_replications = file_reps;
                    have_replay_stats = 1;
                    break;
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
                         pU, pD, pL, pR, output_path, replay_replications) != 0) {
            perror("spawn_server");
            free(replay_prob);
            free(replay_avg);
            return 1;
        }
    } else if (strcmp(mode, "--join") != 0) {
        fprintf(stderr, "Unknown mode %s\n", mode);
        free(replay_prob);
        free(replay_avg);
        return 2;
    }

    int fd = connect_unix(sock_path);
    if (fd < 0) {
        perror("connect_unix");
        free(replay_prob);
        free(replay_avg);
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
    C.base_prob = NULL;
    C.base_avg = NULL;
    C.base_w = C.base_h = 0;
    C.base_replications = 0;
    C.have_base_stats = 0;
    if (font_path) {
        strncpy(C.font_path, font_path, sizeof(C.font_path) - 1);
        C.font_path[sizeof(C.font_path) - 1] = '\0';
    }

    if (have_replay_stats) {
        size_t count = (size_t)replay_w * (size_t)replay_h;
        display_prob = (float*)malloc(count * sizeof(float));
        display_avg = (float*)malloc(count * sizeof(float));
        if (display_prob && display_avg) {
            memcpy(display_prob, replay_prob, count * sizeof(float));
            memcpy(display_avg, replay_avg, count * sizeof(float));
        } else {
            free(display_prob);
            free(display_avg);
            display_prob = NULL;
            display_avg = NULL;
            free(replay_prob);
            free(replay_avg);
            replay_prob = NULL;
            replay_avg = NULL;
            have_replay_stats = 0;
        }
    }

    if (have_replay_stats) {
        pthread_mutex_lock(&C.stats_mtx);
        C.prob_to_center = display_prob;
        C.avg_steps_to_center = display_avg;
        C.stats_w = replay_w;
        C.stats_h = replay_h;
        C.have_stats = 1;
        C.stats_dirty = 1;
        C.base_prob = replay_prob;
        C.base_avg = replay_avg;
        C.base_w = replay_w;
        C.base_h = replay_h;
        C.base_replications = replay_replications;
        C.have_base_stats = (replay_replications > 0);
        pthread_mutex_unlock(&C.stats_mtx);
        C.max_steps = max_steps;
        replay_prob = NULL;
        replay_avg = NULL;
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
                if (e.key.keysym.sym == SDLK_e) {
                    uint32_t mode_now = atomic_load(&C.mode);
                    int cur = atomic_load(&C.current_replication);
                    if (mode_now == MODE_SUMMARY && cur >= C.replications) {
                        restart_to_menu = 1;
                        running = 0;
                    }
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
            int base = C.have_base_stats ? C.base_replications : 0;
            snprintf(title_buf, sizeof(title_buf),
                     "Random Walk Client - Replication: %d / %d",
                     cur + base, C.replications + base);
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
    if (restart_to_menu) {
        force_menu = 1;
        continue;
    }
    return 0;
    }
}
