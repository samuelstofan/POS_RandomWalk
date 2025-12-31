#include "client_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_ui.h"

int load_replay_file(const char *path, int *out_w, int *out_h, int *out_max_steps,
                     float *out_pU, float *out_pD, float *out_pL, float *out_pR,
                     int *out_replications, char *out_sock, size_t out_sock_cap,
                     float **out_prob, float **out_avg)
{
    if (!path || !out_w || !out_h || !out_max_steps ||
        !out_pU || !out_pD || !out_pL || !out_pR ||
        !out_replications || !out_sock || out_sock_cap == 0 ||
        !out_prob || !out_avg) {
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    int w = 0, h = 0, k = 0, reps = 0;
    char sock_buf[256] = {0};
    float pU = 0.0f, pD = 0.0f, pL = 0.0f, pR = 0.0f;
    int scanned = sscanf(line, "%d,%d,%f,%f,%f,%f,%d,%d,%255[^\n]",
                         &w, &h, &pU, &pD, &pL, &pR, &k, &reps, sock_buf);
    if (scanned < 7) {
        fclose(fp);
        return 0;
    }
    if (scanned < 8) reps = 0;
    if (w <= 0 || h <= 0) {
        fclose(fp);
        return 0;
    }

    size_t count = (size_t)w * (size_t)h;
    float *prob = (float*)calloc(count, sizeof(float));
    float *avg = (float*)calloc(count, sizeof(float));
    if (!prob || !avg) {
        free(prob);
        free(avg);
        fclose(fp);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        int x = 0, y = 0;
        float pr = 0.0f, av = 0.0f;
        if (sscanf(line, "%d,%d,%f,%f", &x, &y, &pr, &av) == 4) {
            if (x >= 0 && x < w && y >= 0 && y < h) {
                size_t idx = (size_t)y * (size_t)w + (size_t)x;
                prob[idx] = pr;
                avg[idx] = av;
            }
        }
    }

    fclose(fp);
    *out_w = w;
    *out_h = h;
    *out_max_steps = k;
    *out_pU = pU;
    *out_pD = pD;
    *out_pL = pL;
    *out_pR = pR;
    *out_replications = reps;
    if (scanned >= 9 && sock_buf[0] != '\0') {
        copy_path(out_sock, out_sock_cap, sock_buf);
    } else {
        out_sock[0] = '\0';
    }
    *out_prob = prob;
    *out_avg = avg;
    return 1;
}
