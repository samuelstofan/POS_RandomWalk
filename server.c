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
    float pU, pD, pL, pR;

    // state
    atomic_uint mode; // SimMode

    // clients
    pthread_mutex_t clients_mtx;
    Client *clients;

    // server socket
    int listen_fd;
    char sock_path[RWALK_SOCK_MAX];

    atomic_int running;
    atomic_int sim_started;
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
            continue;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&S->clients_mtx);
}

static void send_welcome(Server *S, int fd) {
    MsgWelcome w = {
        .world_w = (uint32_t)S->world_w,
        .world_h = (uint32_t)S->world_h,
        .mode = atomic_load(&S->mode),
        .step_delay_ms = (uint32_t)S->step_delay_ms,
        .pU = S->pU, .pD = S->pD, .pL = S->pL, .pR = S->pR
    };
    MsgHdr h = { MSG_WELCOME, (uint32_t)sizeof(w) };
    (void)send_all(fd, &h, sizeof(h));
    (void)send_all(fd, &w, sizeof(w));
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
            close(S->listen_fd);
            unlink(S->sock_path);
            g_stop = 1;
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
        send_initial_step(S, cfd);

        pthread_mutex_lock(&S->clients_mtx);
        c->next = S->clients;
        S->clients = c;
        pthread_mutex_unlock(&S->clients_mtx);

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

    int x = S->world_w / 2;
    int y = S->world_h / 2;
    uint32_t step_index = 0;

    while (atomic_load(&S->running)) {
        float r = get_random();
        int dx = 0, dy = 0;
        if (r < S->pU) { dy = -1; }
        else if (r < S->pU + S->pD) { dy = +1; }
        else if (r < S->pU + S->pD + S->pL) { dx = -1; }
        else { dx = +1; }

        x = (x + dx) % S->world_w;
        y = (y + dy) % S->world_h;

        if (x < 0) x += S->world_w;
        if (y < 0) y += S->world_h;

        MsgStep st = { .x = x, .y = y, .step_index = ++step_index };
        clients_broadcast(S, MSG_STEP, &st, sizeof(st));

        usleep((useconds_t)S->step_delay_ms * 1000u);
    }
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
    if (argc < 9) {
        fprintf(stderr,
            "Usage: %s <sock_path> <world_w> <world_h> <delay_ms> <pU> <pD> <pL> <pR>\n"
            "Example: %s /tmp/rwalk.sock 101 101 10 0.25 0.25 0.25 0.25\n",
            argv[0], argv[0]);
        return 2;
    }

    srand((unsigned)time(NULL));

    Server S;
    memset(&S, 0, sizeof(S));
    pthread_mutex_init(&S.clients_mtx, NULL);

    strncpy(S.sock_path, argv[1], sizeof(S.sock_path) - 1);
    S.world_w = atoi(argv[2]);
    S.world_h = atoi(argv[3]);
    S.step_delay_ms = atoi(argv[4]);
    S.pU = strtof(argv[5], NULL);
    S.pD = strtof(argv[6], NULL);
    S.pL = strtof(argv[7], NULL);
    S.pR = strtof(argv[8], NULL);

    float psum = S.pU + S.pD + S.pL + S.pR;
    if (S.world_w <= 2 || S.world_h <= 2 || S.step_delay_ms < 0 || (psum < 0.999f || psum > 1.001f)) {
        fprintf(stderr, "Invalid args (world sizes >2, delay>=0, probabilities sum ~ 1).\n");
        return 2;
    }

    atomic_store(&S.mode, MODE_INTERACTIVE);
    atomic_store(&S.running, 1);
    atomic_store(&S.sim_started, 0);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (make_listen_socket(&S) != 0) {
        perror("server socket");
        return 1;
    }

    fprintf(stdout, "SERVER READY: %s\n", S.sock_path);
    fflush(stdout);

    pthread_create(&S.accept_th, NULL, accept_thread, &S);

    while (!g_stop && atomic_load(&S.running)) {
        sleep(1);
    }

    atomic_store(&S.running, 0);
    pthread_join(S.accept_th, NULL);
    if (atomic_load(&S.sim_started)) {
        pthread_join(S.sim_th, NULL);
    }
    close(S.listen_fd);
    unlink(S.sock_path);
    return 0;
}
