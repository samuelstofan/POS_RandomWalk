#include "server_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "protocol.h"
#include "server_sim.h"

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

static void send_obstacles(Server *S, int fd) {
    if (!S->obstacles) return;
    size_t count = (size_t)S->world_w * (size_t)S->world_h;
    size_t total_len = sizeof(MsgObstaclesHdr) + count * sizeof(uint8_t);
    uint8_t *buf = (uint8_t*)malloc(total_len);
    if (!buf) return;
    MsgObstaclesHdr hdr = { .world_w = (uint32_t)S->world_w, .world_h = (uint32_t)S->world_h };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), S->obstacles, count * sizeof(uint8_t));
    MsgHdr h = { MSG_OBSTACLES, (uint32_t)total_len };
    (void)send_all(fd, &h, sizeof(h));
    (void)send_all(fd, buf, (size_t)total_len);
    free(buf);
}

void clients_broadcast(Server *S, uint32_t type, const void *payload, uint32_t len) {
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

void *accept_thread(void *arg) {
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

        int no_clients = (atomic_load(&S->active_clients) == 0);
        int sim_started = atomic_load(&S->sim_started);
        if (no_clients && sim_started) {
            atomic_store(&S->mode, MODE_SUMMARY);
        }

        send_welcome(S, cfd);
        send_progress(S, cfd);
        send_history(S, cfd);
        send_obstacles(S, cfd);

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

int make_listen_socket(Server *S) {
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
