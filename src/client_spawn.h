#pragma once

#include <stdint.h>

int spawn_server(const char *server_path, const char *sock_path,
                 int world_w, int world_h, int delay_ms, int replications, int max_steps,
                 float pU, float pD, float pL, float pR, const char *output_path,
                 int base_replications, int obstacle_mode, float obstacle_density,
                 uint32_t obstacle_seed, const char *obstacle_file);
