#pragma once

#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>

#include "shared.h"

typedef struct Client {
    int fd;
    struct Client *next;
} Client;

typedef struct {
    int world_w, world_h;
    int step_delay_ms;
    int replications;
    int max_steps;
    float pU, pD, pL, pR;
    int base_replications;
    FILE *results_fp;
    int **steps_to_center;
    int **succesful_replications;
    float **prob_to_center;
    float **avg_steps_to_center;
    uint8_t *obstacles;
    int obstacle_mode;
    float obstacle_density;
    uint32_t obstacle_seed;
    char obstacle_file[256];

    atomic_uint mode;

    atomic_int current_replication;
    atomic_int current_step;

    pthread_mutex_t hist_mtx;
    MsgStep *history;
    int history_cap;

    pthread_mutex_t clients_mtx;
    Client *clients;

    int listen_fd;
    char sock_path[RWALK_SOCK_MAX];

    atomic_int running;
    atomic_int sim_started;
    atomic_int active_clients;
    pthread_t accept_th, sim_th;
} Server;
