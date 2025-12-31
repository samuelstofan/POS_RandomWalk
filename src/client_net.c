#include "client_net.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "protocol.h"
#include "shared.h"
#include "client_fifo.h"

void send_stop(int sockfd) {
    MsgHdr h = { MSG_STOP, 0 };
    send_all(sockfd, &h, sizeof(h));
}

int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void *net_thread(void *arg) {
    ClientState *C = (ClientState*)arg;

    while (atomic_load(&C->running)) {
        MsgHdr h;
        int rr = recv_all(C->sockfd, &h, sizeof(h));
        if (rr <= 0) break;

        if (h.len > (1024u * 1024u)) break;

        uint8_t *payload = NULL;
        if (h.len) {
            payload = (uint8_t*)malloc(h.len);
            if (!payload) break;
            rr = recv_all(C->sockfd, payload, h.len);
            if (rr <= 0) {
                free(payload);
                break;
            }
        }

        if (h.type == MSG_WELCOME && h.len == sizeof(MsgWelcome)) {
            MsgWelcome *w = (MsgWelcome*)payload;
            C->world_w = (int)w->world_w;
            C->world_h = (int)w->world_h;
            C->max_steps = (int)w->max_steps;
            C->delay_ms = (int)w->step_delay_ms;
            C->replications = (int)w->replications;
            atomic_store(&C->mode, w->mode);
            atomic_store(&C->have_welcome, 1);

            pthread_mutex_lock(&C->pos_mtx);
            C->have_pos = 0;
            pthread_mutex_unlock(&C->pos_mtx);
        } else if (h.type == MSG_STEP && h.len == sizeof(MsgStep)) {
            MsgStep *s = (MsgStep*)payload;

            Step st;
            st.x = s->x;
            st.y = s->y;
            st.step_index = s->step_index;

            fifo_push(&C->fifo, st);
        } else if (h.type == MSG_MODE && h.len == sizeof(MsgMode)) {
            MsgMode *m = (MsgMode*)payload;
            atomic_store(&C->mode, m->mode);
        } else if (h.type == MSG_PROGRESS && h.len == sizeof(MsgProgress)) {
            MsgProgress *p = (MsgProgress*)payload;
            atomic_store(&C->current_replication, (int)p->current_replication);
            atomic_store(&C->progress_dirty, 1);
        } else if (h.type == MSG_STATS && h.len >= sizeof(MsgStatsHdr)) {
            MsgStatsHdr *sh = (MsgStatsHdr*)payload;
            uint32_t w = sh->world_w;
            uint32_t hgt = sh->world_h;
            size_t count = (size_t)w * (size_t)hgt;
            size_t expected = sizeof(MsgStatsHdr) + count * sizeof(float) * 2u;
            if (w > 0 && hgt > 0 && h.len == expected) {
                float *prob = (float*)(payload + sizeof(MsgStatsHdr));
                float *avg = prob + count;
                pthread_mutex_lock(&C->stats_mtx);
                if (C->have_base_stats && (C->base_w != (int)w || C->base_h != (int)hgt)) {
                    free(C->base_prob);
                    free(C->base_avg);
                    C->base_prob = NULL;
                    C->base_avg = NULL;
                    C->base_w = C->base_h = 0;
                    C->base_replications = 0;
                    C->have_base_stats = 0;
                }
                if (C->stats_w != (int)w || C->stats_h != (int)hgt || !C->prob_to_center || !C->avg_steps_to_center) {
                    free(C->prob_to_center);
                    free(C->avg_steps_to_center);
                    C->prob_to_center = (float*)malloc(count * sizeof(float));
                    C->avg_steps_to_center = (float*)malloc(count * sizeof(float));
                    C->stats_w = (int)w;
                    C->stats_h = (int)hgt;
                }
                if (C->prob_to_center && C->avg_steps_to_center) {
                    int new_reps = (int)atomic_load(&C->current_replication);
                    int base_reps = C->have_base_stats ? C->base_replications : 0;
                    if (C->have_base_stats && base_reps > 0 && new_reps > 0) {
                        int total_reps = base_reps + new_reps;
                        for (size_t i = 0; i < count; i++) {
                            float prob0 = C->base_prob[i];
                            float avg0 = C->base_avg[i];
                            float success0 = prob0 * (float)base_reps;
                            float steps0 = avg0 * success0;

                            float prob1 = prob[i];
                            float avg1 = avg[i];
                            float success1 = prob1 * (float)new_reps;
                            float steps1 = avg1 * success1;

                            float success = success0 + success1;
                            float steps = steps0 + steps1;

                            C->prob_to_center[i] = (total_reps > 0) ? (success / (float)total_reps) : 0.0f;
                            C->avg_steps_to_center[i] = (success > 0.0f) ? (steps / success) : 0.0f;
                        }
                    } else {
                        memcpy(C->prob_to_center, prob, count * sizeof(float));
                        memcpy(C->avg_steps_to_center, avg, count * sizeof(float));
                    }
                    C->have_stats = 1;
                    C->stats_dirty = 1;
                }
                pthread_mutex_unlock(&C->stats_mtx);
            }
        }

        if (payload) free(payload);
    }

    atomic_store(&C->running, 0);
    return NULL;
}

void send_mode(int sockfd, uint32_t mode) {
    MsgMode m = { .mode = mode };
    MsgHdr h = { MSG_MODE, (uint32_t)sizeof(m) };
    (void)send_all(sockfd, &h, sizeof(h));
    (void)send_all(sockfd, &m, sizeof(m));
}
