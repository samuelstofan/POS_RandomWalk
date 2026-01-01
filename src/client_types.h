#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define STEP_FIFO_CAP 4096
#define MENU_DEFAULT_SOCK "/tmp/rwalk.sock"

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
    int obstacle_mode;
    float obstacle_density;
    uint32_t obstacle_seed;
    char obstacle_file[256];
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
    int head;
    int tail;
    int count;
    pthread_mutex_t mtx;
} StepFIFO;

typedef struct {
    const char *label;
    char *buf;
    size_t cap;
    SDL_Rect rect;
    int numeric_only;
    int allow_dot;
} InputField;

typedef struct {
    int sockfd;
    atomic_int running;

    atomic_int have_welcome;
    int world_w, world_h;
    int max_steps;
    atomic_uint mode;
    int delay_ms;
    int replications;
    atomic_int current_replication;
    atomic_int progress_dirty;

    pthread_mutex_t pos_mtx;
    int have_pos;
    int x, y;
    int prev_x, prev_y;
    uint32_t step_index;

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

    float *base_prob;
    float *base_avg;
    int base_w, base_h;
    int base_replications;
    int have_base_stats;

    uint8_t *obstacles;
    int obs_w, obs_h;
    int have_obstacles;
    int obstacles_drawn;
} ClientState;
