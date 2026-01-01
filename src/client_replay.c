#include "client_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_ui.h"

static int token_is_number(const char *s) {
    if (!s || *s == '\0') return 0;
    char *end = NULL;
    strtof(s, &end);
    return end && *end == '\0';
}

int load_replay_file(const char *path, int *out_w, int *out_h, int *out_max_steps,
                     float *out_pU, float *out_pD, float *out_pL, float *out_pR,
                     int *out_replications, int *out_obstacle_mode,
                     float *out_obstacle_density, uint32_t *out_obstacle_seed,
                     char *out_obstacle_file, size_t out_obstacle_file_cap,
                     char *out_sock, size_t out_sock_cap,
                     float **out_prob, float **out_avg)
{
    if (!path || !out_w || !out_h || !out_max_steps ||
        !out_pU || !out_pD || !out_pL || !out_pR ||
        !out_replications || !out_obstacle_mode || !out_obstacle_density ||
        !out_obstacle_seed || !out_obstacle_file || out_obstacle_file_cap == 0 ||
        !out_sock || out_sock_cap == 0 ||
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

    int w = 0, h = 0, k = 0, reps = 0, obstacles = 0;
    float ob_density = 0.0f;
    uint32_t ob_seed = 0;
    char ob_file[256] = {0};
    char sock_buf[256] = {0};
    float pU = 0.0f, pD = 0.0f, pL = 0.0f, pR = 0.0f;
    char tmp[256];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    tmp[strcspn(tmp, "\r\n")] = '\0';

    char *tokens[16] = {0};
    int token_count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save);
         tok && token_count < (int)(sizeof(tokens) / sizeof(tokens[0]));
         tok = strtok_r(NULL, ",", &save)) {
        tokens[token_count++] = tok;
    }

    if (token_count < 7) {
        fclose(fp);
        return 0;
    }
    w = atoi(tokens[0]);
    h = atoi(tokens[1]);
    pU = strtof(tokens[2], NULL);
    pD = strtof(tokens[3], NULL);
    pL = strtof(tokens[4], NULL);
    pR = strtof(tokens[5], NULL);
    k = atoi(tokens[6]);
    reps = (token_count > 7) ? atoi(tokens[7]) : 0;

    int next = 8;
    if (token_count > next) {
        char *end = NULL;
        long v = strtol(tokens[next], &end, 10);
        if (end && *end == '\0') {
            obstacles = (int)v;
            next++;
        } else {
            snprintf(sock_buf, sizeof(sock_buf), "%s", tokens[next]);
            next = token_count;
        }
    }

    if (token_count > next && token_is_number(tokens[next])) {
        ob_density = strtof(tokens[next], NULL);
        next++;
        if (token_count > next && token_is_number(tokens[next])) {
            ob_seed = (uint32_t)strtoul(tokens[next], NULL, 10);
            next++;
        }
        if (token_count > next) {
            snprintf(ob_file, sizeof(ob_file), "%s", tokens[next]);
            next++;
        }
        if (token_count > next) {
            snprintf(sock_buf, sizeof(sock_buf), "%s", tokens[next]);
        }
    } else if (token_count > next) {
        snprintf(sock_buf, sizeof(sock_buf), "%s", tokens[next]);
    }
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
    *out_obstacle_mode = obstacles;
    *out_obstacle_density = ob_density;
    *out_obstacle_seed = ob_seed;
    if (strcmp(ob_file, "-") != 0 && ob_file[0] != '\0') {
        copy_path(out_obstacle_file, out_obstacle_file_cap, ob_file);
    } else {
        out_obstacle_file[0] = '\0';
    }
    if (sock_buf[0] != '\0') {
        copy_path(out_sock, out_sock_cap, sock_buf);
    } else {
        out_sock[0] = '\0';
    }
    *out_prob = prob;
    *out_avg = avg;
    return 1;
}
