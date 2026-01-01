#include "server_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server_net.h"

static float get_random(void) { return (float)rand() / (float)RAND_MAX; }

static int is_obstacle(const Server *S, int x, int y) {
    if (!S->obstacles) return 0;
    size_t idx = (size_t)y * (size_t)S->world_w + (size_t)x;
    return S->obstacles[idx] != 0;
}

static void write_results(Server *S) {
    if (!S->results_fp) return;

    int reps = atomic_load(&S->current_replication);
    if (reps <= 0) return;

    const char *ob_file = (S->obstacle_file[0] != '\0') ? S->obstacle_file : "-";
    fprintf(S->results_fp, "%d,%d,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%.6f,%u,%s,%s\n",
            S->world_w, S->world_h, S->pU, S->pD, S->pL, S->pR,
            S->max_steps, S->base_replications + reps,
            S->obstacle_mode, S->obstacle_density, S->obstacle_seed,
            ob_file, S->sock_path);

    for (int y = 0; y < S->world_h; y++) {
        for (int x = 0; x < S->world_w; x++) {
            float prob = S->prob_to_center[y][x];
            if (prob <= 0.0f) continue;
            float avg = S->avg_steps_to_center[y][x];
            fprintf(S->results_fp, "%d,%d,%.6f,%.6f\n", x, y, prob, avg);
        }
    }
    fflush(S->results_fp);
}

static void compute_and_send_stats(Server *S, int current_replication) {
    if (current_replication <= 0) return;

    size_t count = (size_t)S->world_w * (size_t)S->world_h;
    size_t floats_bytes = count * sizeof(float);
    size_t total_len = sizeof(MsgStatsHdr) + floats_bytes * 2u;

    uint8_t *buf = (uint8_t*)malloc(total_len);
    if (!buf) return;

    MsgStatsHdr hdr = { .world_w = (uint32_t)S->world_w, .world_h = (uint32_t)S->world_h };
    memcpy(buf, &hdr, sizeof(hdr));

    float *prob = (float*)(buf + sizeof(hdr));
    float *avg = prob + count;

    for (int y = 0; y < S->world_h; y++) {
        for (int x = 0; x < S->world_w; x++) {
            int success = S->succesful_replications[y][x];
            int steps = S->steps_to_center[y][x];
            size_t idx = (size_t)y * (size_t)S->world_w + (size_t)x;

            prob[idx] = (float)success / (float)current_replication;
            if (success > 0) {
                avg[idx] = (float)steps / ((float)success);
            } else {
                avg[idx] = 0.0f;
            }

            S->prob_to_center[y][x] = prob[idx];
            S->avg_steps_to_center[y][x] = avg[idx];
        }
    }

    clients_broadcast(S, MSG_STATS, buf, (uint32_t)total_len);
    free(buf);
}

void *sim_thread(void *arg) {
    Server *S = (Server*)arg;

    int center_x = S->world_w / 2;
    int center_y = S->world_h / 2;
    for (int rep = 0; rep < S->replications && atomic_load(&S->running); rep++) {
        for (int x_spawn = 0; x_spawn < S->world_w && atomic_load(&S->running); x_spawn++) {
            for (int y_spawn = 0; y_spawn < S->world_h && atomic_load(&S->running); y_spawn++) {
                if (x_spawn == center_x && y_spawn == center_y) continue;
                if (is_obstacle(S, x_spawn, y_spawn)) continue;

                atomic_store(&S->current_replication, rep + 1);
                MsgProgress p = {
                    .current_replication = (uint32_t)(rep + 1),
                    .total_replications = (uint32_t)S->replications
                };
                clients_broadcast(S, MSG_PROGRESS, &p, sizeof(p));
                atomic_store(&S->current_step, 0);

                int x = x_spawn;
                int y = y_spawn;

                MsgStep st0 = { .x = x, .y = y, .step_index = 0 };
                clients_broadcast(S, MSG_STEP, &st0, sizeof(st0));

                pthread_mutex_lock(&S->hist_mtx);
                if (S->history && S->history_cap > 0) {
                    S->history[0] = st0;
                }
                pthread_mutex_unlock(&S->hist_mtx);

                for (int step = 0; step < S->max_steps && atomic_load(&S->running); step++) {
                    float r = get_random();
                    int dx = 0, dy = 0;

                    if (r < S->pU) dy = -1;
                    else if (r < S->pU + S->pD) dy = +1;
                    else if (r < S->pU + S->pD + S->pL) dx = -1;
                    else dx = +1;

                    int nx = (x + dx) % S->world_w;
                    int ny = (y + dy) % S->world_h;
                    if (nx < 0) nx += S->world_w;
                    if (ny < 0) ny += S->world_h;
                    if (!is_obstacle(S, nx, ny)) {
                        x = nx;
                        y = ny;
                    }

                    atomic_store(&S->current_step, step + 1);

                    MsgStep st = {
                        .x = x,
                        .y = y,
                        .step_index = step + 1
                    };
                    clients_broadcast(S, MSG_STEP, &st, sizeof(st));

                    pthread_mutex_lock(&S->hist_mtx);
                    if (S->history && (step + 1) < S->history_cap) {
                        S->history[step + 1] = st;
                    }
                    pthread_mutex_unlock(&S->hist_mtx);

                    if (S->steps_to_center && x == center_x && y == center_y) {
                        S->steps_to_center[y_spawn][x_spawn] += step;
                        S->succesful_replications[y_spawn][x_spawn]++;
                        break;
                    }

                    if (atomic_load(&S->mode) != MODE_SUMMARY) {
                        usleep((unsigned int)S->step_delay_ms * 1000u);
                    }
                }
            }
        }
        compute_and_send_stats(S, rep + 1);
    }
    compute_and_send_stats(S, atomic_load(&S->current_replication));
    write_results(S);

    MsgMode m = { .mode = MODE_SUMMARY };
    atomic_store(&S->mode, MODE_SUMMARY);
    clients_broadcast(S, MSG_MODE, &m, sizeof(m));
    atomic_store(&S->running, 0);
    return NULL;
}
