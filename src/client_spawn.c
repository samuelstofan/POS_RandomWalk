#include "client_spawn.h"

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

int spawn_server(const char *server_path, const char *sock_path,
                 int world_w, int world_h, int delay_ms, int replications, int max_steps,
                 float pU, float pD, float pL, float pR, const char *output_path,
                 int base_replications, int obstacle_mode, float obstacle_density,
                 const char *obstacle_file,
                 int start_on_client)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        (void)setsid();
        char wbuf[32], hbuf[32], dbuf[32], rbuf[32], kbuf[32], pu[32], pd[32], pl[32], pr[32];
        char basebuf[32], obbuf[32], obdens[32], startbuf[32];
        snprintf(wbuf, sizeof(wbuf), "%d", world_w);
        snprintf(hbuf, sizeof(hbuf), "%d", world_h);
        snprintf(dbuf, sizeof(dbuf), "%d", delay_ms);
        snprintf(rbuf, sizeof(rbuf), "%d", replications);
        snprintf(kbuf, sizeof(kbuf), "%d", max_steps);
        snprintf(pu, sizeof(pu), "%.6f", pU);
        snprintf(pd, sizeof(pd), "%.6f", pD);
        snprintf(pl, sizeof(pl), "%.6f", pL);
        snprintf(pr, sizeof(pr), "%.6f", pR);
        snprintf(basebuf, sizeof(basebuf), "%d", base_replications);
        snprintf(obbuf, sizeof(obbuf), "%d", obstacle_mode);
        snprintf(obdens, sizeof(obdens), "%.6f", obstacle_density);
        snprintf(startbuf, sizeof(startbuf), "%d", start_on_client);

        if (output_path && output_path[0]) {
            execl(server_path, server_path, sock_path, wbuf, hbuf, dbuf, rbuf, kbuf,
                  pu, pd, pl, pr, output_path, basebuf, obbuf, obdens,
                  obstacle_file ? obstacle_file : "", startbuf, (char*)NULL);
        } else {
            execl(server_path, server_path, sock_path, wbuf, hbuf, dbuf, rbuf, kbuf, pu, pd, pl, pr, (char*)NULL);
        }
        perror("execl server");
        _exit(127);
    }
    usleep(150 * 1000);
    return 0;
}
