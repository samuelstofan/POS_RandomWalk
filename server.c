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
#include "shared.h"

static volatile sig_atomic_t g_stop = 0;

static int send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t*)buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return 0;           
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 1;
}

static float get_random(void) { return (float)rand() / (float)RAND_MAX; }

static void *sim_thread(void *arg);

typedef struct Client {
    int fd;
    struct Client *next;
} Client;

typedef struct {
    // config
    int world_w, world_h;
    int step_delay_ms;
    int replications;
    int max_steps;
    float pU, pD, pL, pR;
    FILE *results_fp;
    int **steps_to_center;
    int **succesful_replications;
    float **prob_to_center;
    float **avg_steps_to_center;

    // state
    atomic_uint mode;

    atomic_int current_replication;
    atomic_int current_step;

    // step history for late joiners (current replication)
    pthread_mutex_t hist_mtx;
    MsgStep *history;
    int history_cap;

    // clients
    pthread_mutex_t clients_mtx;
    Client *clients;

    // server socket
    int listen_fd;
    char sock_path[RWALK_SOCK_MAX];

    atomic_int running;
    atomic_int sim_started;
    atomic_int waiting_before_shutdown;
    atomic_int active_clients;
    pthread_t accept_th, sim_th;
} Server;

static void clients_broadcast(Server *S, uint32_t type, const void *payload, uint32_t len) {
    MsgHdr h = { type, len };

    pthread_mutex_lock(&S->clients_mtx);
    Client **pp = &S->clients;
    while (*pp) {
        Client *c = *pp;
        if (send_all(c->fd, &h, sizeof(h)) != 0 || (len && send_all(c->fd, payload, len) != 0)) {
            close(c->fd);
            *pp = c->next;
            free(c);
            atomic_fetch_sub(&S->active_clients, 1);
            continue;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&S->clients_mtx);
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

static void send_welcome(Server *S, int fd) {
    MsgWelcome w = {
        .world_w = (uint32_t)S->world_w,
        .world_h = (uint32_t)S->world_h,
        .step_delay_ms = (uint32_t)S->step_delay_ms,
        .replications = (uint32_t)S->replications,
        .max_steps = (uint32_t)S->max_steps,
        .mode = atomic_load(&S->mode),
        .pU = S->pU, .pD = S->pD, .pL = S->pL, .pR = S->pR
    };
    MsgHdr h = { MSG_WELCOME, (uint32_t)sizeof(w) };
    (void)send_all(fd, &h, sizeof(h));
    (void)send_all(fd, &w, sizeof(w));
}

static void send_progress(Server *S, int fd) {
    MsgProgress p = {
        .current_replication = (uint32_t)atomic_load(&S->current_replication),
        .total_replications = (uint32_t)S->replications
    };
    MsgHdr h = { MSG_PROGRESS, (uint32_t)sizeof(p) };
    (void)send_all(fd, &h, sizeof(h));
    (void)send_all(fd, &p, sizeof(p));
}

static void send_initial_step(Server *S, int fd) {
    MsgStep st = {
        .x = S->world_w / 2,
        .y = S->world_h / 2,
        .step_index = 0
    };
    MsgHdr h = { MSG_STEP, (uint32_t)sizeof(st) };
    (void)send_all(fd, &h, sizeof(h));
    (void)send_all(fd, &st, sizeof(st));
}

static void send_history(Server *S, int fd) {
    int count = 0;
    MsgStep *tmp = NULL;

    pthread_mutex_lock(&S->hist_mtx);
    int current = atomic_load(&S->current_step);
    if (current < 0) current = 0;
    if (current >= S->history_cap) current = S->history_cap - 1;
    count = current + 1;
    if (count > 0) {
        tmp = (MsgStep*)malloc((size_t)count * sizeof(*tmp));
        if (tmp) {
            memcpy(tmp, S->history, (size_t)count * sizeof(*tmp));
        }
    }
    pthread_mutex_unlock(&S->hist_mtx);

    if (!tmp) {
        send_initial_step(S, fd);
        return;
    }

    for (int i = 0; i < count; i++) {
        MsgHdr h = { MSG_STEP, (uint32_t)sizeof(tmp[i]) };
        (void)send_all(fd, &h, sizeof(h));
        (void)send_all(fd, &tmp[i], sizeof(tmp[i]));
    }
    free(tmp);
}

static void *client_reader_thread(void *arg) {
    struct { Server *S; int fd; } *ctx = arg;
    Server *S = ctx->S;
    int fd = ctx->fd;
    free(ctx);

    for (;;) {
        MsgHdr h;
        int rr = recv_all(fd, &h, sizeof(h));
        if (rr <= 0) break;

        if (h.len > 1024) { 
            break;
        }

        uint8_t payload[1024];
        if (h.len) {
            rr = recv_all(fd, payload, h.len);
            if (rr <= 0) break;
        }
        
        if (h.type == MSG_STOP && h.len == 0) {
            fprintf(stderr, "Server: STOP received\n");
            atomic_store(&S->running, 0);
            atomic_store(&S->waiting_before_shutdown, 0);
            break;
        }

        if (h.type == MSG_MODE && h.len == sizeof(MsgMode)) {
            MsgMode *m = (MsgMode*)payload;
            if (m->mode == MODE_INTERACTIVE || m->mode == MODE_SUMMARY) {
                atomic_store(&S->mode, m->mode);
                clients_broadcast(S, MSG_MODE, m, sizeof(*m));
            }
        }
    }

    pthread_mutex_lock(&S->clients_mtx);
    Client **pp = &S->clients;
    while (*pp) {
        if ((*pp)->fd == fd) {
            Client *victim = *pp;
            *pp = victim->next;
            close(victim->fd);
            free(victim);
            atomic_fetch_sub(&S->active_clients, 1);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&S->clients_mtx);
    return NULL;
}

static void *accept_thread(void *arg) {
    Server *S = (Server*)arg;
    while (atomic_load(&S->running)) {
        int cfd = accept(S->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!atomic_load(&S->running)) break;
            perror("accept");
            break;
        }

        Client *c = (Client*)calloc(1, sizeof(Client));
        c->fd = cfd;

        send_welcome(S, cfd);
        send_progress(S, cfd);
        send_history(S, cfd);

        pthread_mutex_lock(&S->clients_mtx);
        c->next = S->clients;
        S->clients = c;
        pthread_mutex_unlock(&S->clients_mtx);
        atomic_fetch_add(&S->active_clients, 1);

        int expected = 0;
        if (atomic_compare_exchange_strong(&S->sim_started, &expected, 1)) {
            pthread_create(&S->sim_th, NULL, sim_thread, S);
        }

        pthread_t th;
        struct { Server *S; int fd; } *ctx = malloc(sizeof(*ctx));
        ctx->S = S; ctx->fd = cfd;
        pthread_create(&th, NULL, client_reader_thread, ctx);
        pthread_detach(th);
    }
    return NULL;
}

static void *sim_thread(void *arg) {
    Server *S = (Server*)arg;

    int center_x = S->world_w / 2;
    int center_y = S->world_h / 2;
    for (int rep = 0; rep < S->replications && atomic_load(&S->running); rep++) {
        for (int x_spawn = 0; x_spawn < S->world_w && atomic_load(&S->running); x_spawn++) {
            for (int y_spawn = 0; y_spawn < S->world_h && atomic_load(&S->running); y_spawn++) {
                if (x_spawn == center_x && y_spawn == center_y) continue;

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

                for (int step = 0;step < S->max_steps && atomic_load(&S->running);step++) {
                    float r = get_random();
                    int dx = 0, dy = 0;

                    if (r < S->pU) dy = -1;
                    else if (r < S->pU + S->pD) dy = +1;
                    else if (r < S->pU + S->pD + S->pL) dx = -1;
                    else dx = +1;

                    x = (x + dx) % S->world_w;
                    y = (y + dy) % S->world_h;
                    if (x < 0) x += S->world_w;
                    if (y < 0) y += S->world_h;

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
                        usleep((useconds_t)S->step_delay_ms * 1000u);
                    } 
                }

                /*if (S->results_fp) {
                    int manhattan = abs(x - center_x) + abs(y - center_y);
                    fprintf(S->results_fp, "%d,%d\n", rep, manhattan);
                    fflush(S->results_fp);
                }*/
            }
        }
        compute_and_send_stats(S, rep + 1);
    }
    MsgMode m = { .mode = MODE_SUMMARY };
    atomic_store(&S->mode, MODE_SUMMARY);
    clients_broadcast(S, MSG_MODE, &m, sizeof(m));
    atomic_store(&S->running, 0);
    return NULL;
}

static int make_listen_socket(Server *S) {
    S->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (S->listen_fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, S->sock_path, sizeof(addr.sun_path) - 1);

    unlink(S->sock_path);
    if (bind(S->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return -1;
    if (listen(S->listen_fd, 32) < 0) return -1;
    return 0;
}

static void on_sigint(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv) {
    if (argc < 11) {
        fprintf(stderr,
            "Usage: %s <sock_path> <world_w> <world_h> <delay_ms> <replications> <max_steps> <pU> <pD> <pL> <pR> [output_file]\n"
            "Example: %s /tmp/rwalk.sock 101 101 10 5 100 0.25 0.25 0.25 0.25 results.csv\n",
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

    S.results_fp = fopen(results_path, "w");
    if (!S.results_fp) {
        perror(results_path);
        return 1;
    }
    fprintf(S.results_fp, "replication,manhattan_distance\n");
    fflush(S.results_fp);

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
