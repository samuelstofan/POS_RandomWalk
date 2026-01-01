#define _GNU_SOURCE
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
#include <signal.h>
#include <time.h>

#include "server_types.h"
#include "server_net.h"
#include "server_sim.h"

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static int check_connectivity(int w, int h, const uint8_t *obs) {
    if (!obs) return 1;
    int cx = w / 2;
    int cy = h / 2;
    size_t start = (size_t)cy * (size_t)w + (size_t)cx;
    if (obs[start]) return 0;

    size_t count = (size_t)w * (size_t)h;
    uint8_t *visited = (uint8_t*)calloc(count, sizeof(uint8_t));
    int *queue = (int*)malloc(count * sizeof(int));
    if (!visited || !queue) {
        free(visited);
        free(queue);
        return 0;
    }

    int head = 0, tail = 0;
    queue[tail++] = (int)start;
    visited[start] = 1;

    while (head < tail) {
        int idx = queue[head++];
        int x = idx % w;
        int y = idx / w;

        int nx[4] = { (x + 1) % w, (x - 1 + w) % w, x, x };
        int ny[4] = { y, y, (y + 1) % h, (y - 1 + h) % h };
        for (int i = 0; i < 4; i++) {
            int nidx = ny[i] * w + nx[i];
            if (obs[nidx] || visited[nidx]) continue;
            visited[nidx] = 1;
            queue[tail++] = nidx;
        }
    }

    size_t reachable = 0;
    size_t free_cells = 0;
    for (size_t i = 0; i < count; i++) {
        if (!obs[i]) {
            free_cells++;
            if (visited[i]) reachable++;
        }
    }

    free(visited);
    free(queue);
    return reachable == free_cells;
}

static uint32_t next_rand(uint32_t *state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static float rand_float(uint32_t *state) {
    return (float)(next_rand(state) & 0xFFFFFFu) / (float)0x1000000u;
}

static int load_obstacles_file(const char *path, int w, int h, uint8_t **out_obs) {
    if (!path || !out_obs || w <= 0 || h <= 0) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    size_t count = (size_t)w * (size_t)h;
    uint8_t *obs = (uint8_t*)calloc(count, sizeof(uint8_t));
    if (!obs) {
        fclose(fp);
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        int x = 0, y = 0;
        int scanned = sscanf(line, " %d , %d ", &x, &y);
        if (scanned != 2) {
            size_t len = strcspn(line, "\r\n");
            if (len == 0) continue;
            fclose(fp);
            free(obs);
            return 0;
        }
        if (x < 0 || x >= w || y < 0 || y >= h) {
            fclose(fp);
            free(obs);
            return 0;
        }
        obs[(size_t)y * (size_t)w + (size_t)x] = 1;
    }

    fclose(fp);
    *out_obs = obs;
    return 1;
}

static int generate_random_obstacles(Server *S) {
    if (!S->obstacle_mode) return 1;
    if (S->world_w < 3 || S->world_h < 3) return 0;

    size_t count = (size_t)S->world_w * (size_t)S->world_h;
    S->obstacles = (uint8_t*)calloc(count, sizeof(uint8_t));
    if (!S->obstacles) return 0;

    uint32_t seed = S->obstacle_seed;
    if (seed == 0) seed = (uint32_t)time(NULL);
    uint32_t start_seed = seed;

    float density = S->obstacle_density;
    if (density < 0.0f) density = 0.0f;
    if (density > 0.8f) density = 0.8f;

    int cx = S->world_w / 2;
    int cy = S->world_h / 2;

    for (int attempt = 0; attempt < 200; attempt++) {
        memset(S->obstacles, 0, count);
        for (int y = 0; y < S->world_h; y++) {
            for (int x = 0; x < S->world_w; x++) {
                if ((x == 0 && y == 0) || (x == cx && y == cy)) continue;
                if (rand_float(&seed) < density) {
                    size_t idx = (size_t)y * (size_t)S->world_w + (size_t)x;
                    S->obstacles[idx] = 1;
                }
            }
        }
        if (check_connectivity(S->world_w, S->world_h, S->obstacles)) {
            S->obstacle_seed = start_seed;
            return 1;
        }
    }
    free(S->obstacles);
    S->obstacles = NULL;
    return 0;
}

static int init_obstacles(Server *S) {
    if (!S->obstacle_mode) return 1;

    if (S->obstacle_mode == 2) {
        uint8_t *obs = NULL;
        if (!load_obstacles_file(S->obstacle_file, S->world_w, S->world_h, &obs)) {
            fprintf(stderr, "Failed to load obstacle file %s\n", S->obstacle_file);
            return 0;
        }
        int cx = S->world_w / 2;
        int cy = S->world_h / 2;
        if (obs[(size_t)cy * (size_t)S->world_w + (size_t)cx]) {
            fprintf(stderr, "Obstacle at center is not allowed.\n");
            free(obs);
            return 0;
        }
        if (!check_connectivity(S->world_w, S->world_h, obs)) {
            fprintf(stderr, "Obstacle world is not fully reachable from [0,0].\n");
            free(obs);
            return 0;
        }
        S->obstacles = obs;
        S->obstacle_density = 0.0f;
        S->obstacle_seed = 0;
        return 1;
    }

    return generate_random_obstacles(S);
}

int main(int argc, char **argv) {
    if (argc < 11) {
        fprintf(stderr,
            "Usage: %s <sock_path> <world_w> <world_h> <delay_ms> <replications> <max_steps> <pU> <pD> <pL> <pR> [output_file] [base_replications] [obstacle_mode] [obstacle_density] [obstacle_seed] [obstacle_file]\n"
            "Example: %s /tmp/rwalk.sock 101 101 10 5 100 0.25 0.25 0.25 0.25 results.csv 50 1 0.2 12345\n",
            argv[0], argv[0]);
        return 2;
    }

    srand((unsigned)time(NULL));

    Server S;
    memset(&S, 0, sizeof(S));
    pthread_mutex_init(&S.clients_mtx, NULL);
    pthread_mutex_init(&S.hist_mtx, NULL);

    strncpy(S.sock_path, argv[1], sizeof(S.sock_path) - 1);
    S.world_w = atoi(argv[2]);
    S.world_h = atoi(argv[3]);
    S.step_delay_ms = atoi(argv[4]);
    S.replications = atoi(argv[5]);    
    S.max_steps    = atoi(argv[6]);
    S.pU = strtof(argv[7], NULL);
    S.pD = strtof(argv[8], NULL);
    S.pL = strtof(argv[9], NULL);
    S.pR = strtof(argv[10], NULL);
    const char *results_path = (argc >= 12) ? argv[11] : "replication_results.csv";
    S.base_replications = (argc >= 13) ? atoi(argv[12]) : 0;
    S.obstacle_mode = (argc >= 14) ? atoi(argv[13]) : 0;
    S.obstacle_density = (argc >= 15) ? strtof(argv[14], NULL) : 0.0f;
    S.obstacle_seed = (argc >= 16) ? (uint32_t)strtoul(argv[15], NULL, 10) : 0;
    if (argc >= 17) {
        strncpy(S.obstacle_file, argv[16], sizeof(S.obstacle_file) - 1);
        S.obstacle_file[sizeof(S.obstacle_file) - 1] = '\0';
    } else {
        S.obstacle_file[0] = '\0';
    }
    if (S.obstacle_mode != 2) {
        S.obstacle_file[0] = '\0';
    }
    if (S.obstacle_mode == 0) {
        S.obstacle_density = 0.0f;
        S.obstacle_seed = 0;
    }
    if (S.base_replications < 0) S.base_replications = 0;

    S.results_fp = fopen(results_path, "w");
    if (!S.results_fp) {
        perror(results_path);
        return 1;
    }
    float psum = S.pU + S.pD + S.pL + S.pR;
    if (S.step_delay_ms < 0 || (psum < 0.999f || psum > 1.001f)) {
        fprintf(stderr, "Invalid args (delay>=0, probabilities sum ~ 1).\n");
        fclose(S.results_fp);
        return 2;
    }
    if (S.obstacle_mode != 2 && (S.world_w <= 2 || S.world_h <= 2)) {
        fprintf(stderr, "Invalid args (world sizes >2).\n");
        fclose(S.results_fp);
        return 2;
    }
    if (S.replications <= 0 || S.max_steps <= 0) {
        fprintf(stderr, "replications and max_steps must be > 0\n");
        fclose(S.results_fp);
        return 2;
    }
    if (S.obstacle_mode < 0 || S.obstacle_mode > 2) {
        fprintf(stderr, "obstacle_mode must be 0, 1, or 2\n");
        fclose(S.results_fp);
        return 2;
    }
    if (S.obstacle_mode == 2 && S.obstacle_file[0] == '\0') {
        fprintf(stderr, "obstacle_file is required when obstacle_mode=2\n");
        fclose(S.results_fp);
        return 2;
    }

    if (!init_obstacles(&S)) {
        fclose(S.results_fp);
        return 2;
    }
    if (S.world_w <= 2 || S.world_h <= 2) {
        fprintf(stderr, "Invalid world size (must be > 2).\n");
        fclose(S.results_fp);
        return 2;
    }

    atomic_store(&S.mode, MODE_INTERACTIVE);
    atomic_store(&S.current_replication, 0);
    atomic_store(&S.current_step, 0);
    atomic_store(&S.running, 1);
    atomic_store(&S.sim_started, 0);
    atomic_store(&S.waiting_before_shutdown, 0);
    atomic_store(&S.active_clients, 0);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    S.history_cap = S.max_steps + 1;
    S.history = (MsgStep*)calloc((size_t)S.history_cap, sizeof(*S.history));
    if (!S.history) {
        perror("history alloc");
        fclose(S.results_fp);
        return 1;
    }
    S.history[0].x = S.world_w / 2;
    S.history[0].y = S.world_h / 2;
    S.history[0].step_index = 0;

    S.steps_to_center = (int**)calloc((size_t)S.world_h, sizeof(*S.steps_to_center));
    S.succesful_replications = (int**)calloc((size_t)S.world_h, sizeof(*S.succesful_replications));
    S.prob_to_center = (float**)calloc((size_t)S.world_h, sizeof(*S.prob_to_center));
    S.avg_steps_to_center = (float**)calloc((size_t)S.world_h, sizeof(*S.avg_steps_to_center));
    if (!S.steps_to_center || !S.succesful_replications || !S.prob_to_center || !S.avg_steps_to_center) {
        perror("steps_to_center || succesful_replications  rows alloc");
        if (S.steps_to_center) free(S.steps_to_center);
        if (S.succesful_replications) free(S.succesful_replications);
        if (S.prob_to_center) free(S.prob_to_center);
        if (S.avg_steps_to_center) free(S.avg_steps_to_center);
        free(S.history);
        fclose(S.results_fp);
        return 1;
    }
    S.steps_to_center[0] = (int*)calloc((size_t)S.world_w * (size_t)S.world_h,sizeof(**S.steps_to_center));
    S.succesful_replications[0] = (int*)calloc((size_t)S.world_w * (size_t)S.world_h,sizeof(**S.succesful_replications));
    S.prob_to_center[0] = (float*)calloc((size_t)S.world_w * (size_t)S.world_h, sizeof(**S.prob_to_center));
    S.avg_steps_to_center[0] = (float*)calloc((size_t)S.world_w * (size_t)S.world_h, sizeof(**S.avg_steps_to_center));
    if (!S.steps_to_center[0] || !S.succesful_replications[0] || !S.prob_to_center[0] || !S.avg_steps_to_center[0]) {
        perror("steps_to_center || S.succesful_replicationsdata alloc");
        if (S.steps_to_center[0]) free(S.steps_to_center[0]);
        if (S.succesful_replications[0]) free(S.succesful_replications[0]);
        if (S.prob_to_center[0]) free(S.prob_to_center[0]);
        if (S.avg_steps_to_center[0]) free(S.avg_steps_to_center[0]);
        free(S.succesful_replications);
        free(S.steps_to_center);
        free(S.prob_to_center);
        free(S.avg_steps_to_center);
        free(S.history);
        fclose(S.results_fp);
        return 1;
    }
    for (int y = 1; y < S.world_h; y++) {
        S.steps_to_center[y] = S.steps_to_center[0] + (size_t)y * (size_t)S.world_w;
        S.succesful_replications[y] = S.succesful_replications[0] + (size_t)y * (size_t)S.world_w;
        S.prob_to_center[y] = S.prob_to_center[0] + (size_t)y * (size_t)S.world_w;
        S.avg_steps_to_center[y] = S.avg_steps_to_center[0] + (size_t)y * (size_t)S.world_w;
    }

    
    if (make_listen_socket(&S) != 0) {
        perror("server socket");
        fclose(S.results_fp);
        return 1;
    }

    fprintf(stdout, "SERVER READY: %s\n", S.sock_path);
    fflush(stdout);

    pthread_create(&S.accept_th, NULL, accept_thread, &S);

    while (!g_stop && atomic_load(&S.running)) {
        sleep(1);
        atomic_store(&S.waiting_before_shutdown, 1);
    }

    while (atomic_load(&S.active_clients) > 0) {
        sleep(1);
    }
    atomic_store(&S.running, 0);
    shutdown(S.listen_fd, SHUT_RDWR);
    pthread_join(S.accept_th, NULL);
    if (atomic_load(&S.sim_started)) {
        pthread_join(S.sim_th, NULL);
    }
    close(S.listen_fd);
    unlink(S.sock_path);

    if (S.steps_to_center) {
        free(S.steps_to_center[0]);
        free(S.steps_to_center);
    }
    if (S.succesful_replications) {
        free(S.succesful_replications[0]);
        free(S.succesful_replications);
    }
    if (S.prob_to_center) {
        free(S.prob_to_center[0]);
        free(S.prob_to_center);
    }
    if (S.avg_steps_to_center) {
        free(S.avg_steps_to_center[0]);
        free(S.avg_steps_to_center);
    }
    if (S.obstacles) {
        free(S.obstacles);
    }
    free(S.history);

    fclose(S.results_fp);
    return 0;
}
