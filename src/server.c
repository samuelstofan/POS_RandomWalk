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

int main(int argc, char **argv) {
    if (argc < 11) {
        fprintf(stderr,
            "Usage: %s <sock_path> <world_w> <world_h> <delay_ms> <replications> <max_steps> <pU> <pD> <pL> <pR> [output_file] [base_replications]\n"
            "Example: %s /tmp/rwalk.sock 101 101 10 5 100 0.25 0.25 0.25 0.25 results.csv 50\n",
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
    if (S.base_replications < 0) S.base_replications = 0;

    S.results_fp = fopen(results_path, "w");
    if (!S.results_fp) {
        perror(results_path);
        return 1;
    }
    float psum = S.pU + S.pD + S.pL + S.pR;
    if (S.world_w <= 2 || S.world_h <= 2 || S.step_delay_ms < 0 || (psum < 0.999f || psum > 1.001f)) {
        fprintf(stderr, "Invalid args (world sizes >2, delay>=0, probabilities sum ~ 1).\n");
        fclose(S.results_fp);
        return 2;
    }
    if (S.replications <= 0 || S.max_steps <= 0) {
        fprintf(stderr, "replications and max_steps must be > 0\n");
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
    free(S.history);

    fclose(S.results_fp);
    return 0;
}
