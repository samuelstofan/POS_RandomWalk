#pragma once

#include "client_types.h"

int load_replay_file(const char *path, int *out_w, int *out_h, int *out_max_steps,
                     float *out_pU, float *out_pD, float *out_pL, float *out_pR,
                     int *out_replications, char *out_sock, size_t out_sock_cap,
                     float **out_prob, float **out_avg);
