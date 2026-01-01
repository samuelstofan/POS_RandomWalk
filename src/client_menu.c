#include "client_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_ui.h"

int run_new_sim_menu(NewSimConfig *cfg)
{
    if (!cfg) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(new_sim): %s\n", SDL_GetError());
        return 0;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 24);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(new_sim): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(new_sim): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk - New simulation",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(new_sim): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return 0;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(new_sim): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return 0;
    }

    char w_buf[32] = "51";
    char h_buf[32] = "51";
    char obs_mode_buf[32] = "0";
    char obs_density_buf[32] = "0.20";
    char obs_file_buf[256] = "";
    char rep_buf[32] = "100";
    char k_buf[32] = "100";
    char pu_buf[32] = "0.25";
    char pd_buf[32] = "0.25";
    char pl_buf[32] = "0.25";
    char pr_buf[32] = "0.25";
    char sock_buf[256];
    char out_buf[256];
    snprintf(sock_buf, sizeof(sock_buf), "%s", MENU_DEFAULT_SOCK);
    snprintf(out_buf, sizeof(out_buf), "replication_results.csv");

    InputField fields[13];
    fields[0] = (InputField){ "Socket:", sock_buf, sizeof(sock_buf), {0}, 0, 0 };
    fields[1] = (InputField){ "World width:", w_buf, sizeof(w_buf), {0}, 1, 0 };
    fields[2] = (InputField){ "World height:", h_buf, sizeof(h_buf), {0}, 1, 0 };
    fields[3] = (InputField){ "Obstacle mode (0/1/2):", obs_mode_buf, sizeof(obs_mode_buf), {0}, 1, 0 };
    fields[4] = (InputField){ "Obstacle density:", obs_density_buf, sizeof(obs_density_buf), {0}, 1, 1 };
    fields[5] = (InputField){ "Obstacle file:", obs_file_buf, sizeof(obs_file_buf), {0}, 0, 0 };
    fields[6] = (InputField){ "Replications:", rep_buf, sizeof(rep_buf), {0}, 1, 0 };
    fields[7] = (InputField){ "K (max steps):", k_buf, sizeof(k_buf), {0}, 1, 0 };
    fields[8] = (InputField){ "pU:", pu_buf, sizeof(pu_buf), {0}, 1, 1 };
    fields[9] = (InputField){ "pD:", pd_buf, sizeof(pd_buf), {0}, 1, 1 };
    fields[10] = (InputField){ "pL:", pl_buf, sizeof(pl_buf), {0}, 1, 1 };
    fields[11] = (InputField){ "pR:", pr_buf, sizeof(pr_buf), {0}, 1, 1 };
    fields[12] = (InputField){ "Output file:", out_buf, sizeof(out_buf), {0}, 0, 0 };

    MenuButton buttons[2];
    buttons[0].label = "Launch";
    buttons[0].action = MENU_ACT_NEW;
    buttons[1].label = "Back";
    buttons[1].action = MENU_ACT_EXIT;

    int focus = 0;
    int running = 1;
    int accepted = 0;
    char error_msg[128] = {0};

    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
                    if (point_in_rect(mx, my, &fields[i].rect)) {
                        focus = i;
                        break;
                    }
                }
                for (int i = 0; i < 2; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        if (i == 0) {
                            accepted = 1;
                        } else {
                            accepted = 0;
                        }
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                int total = (int)(sizeof(fields) / sizeof(fields[0])) + 2;
                if (e.key.keysym.sym == SDLK_TAB) {
                    int dir = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                    focus = (focus + dir + total) % total;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    focus = (focus - 1 + total) % total;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    focus = (focus + 1) % total;
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                        input_backspace(fields[focus].buf);
                    }
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    if (focus == (int)(sizeof(fields) / sizeof(fields[0]))) {
                        accepted = 1;
                        running = 0;
                    } else if (focus == (int)(sizeof(fields) / sizeof(fields[0])) + 1) {
                        accepted = 0;
                        running = 0;
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    accepted = 0;
                    running = 0;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                    InputField *f = &fields[focus];
                    input_append(f->buf, f->cap, e.text.text, f->numeric_only, f->allow_dot);
                }
            }
        }

        int content_w = (int)(win_w * 0.6);
        int start_x = (win_w - content_w) / 2;
        int label_w = (int)(content_w * 0.45);
        int input_w = content_w - label_w - 24;
        int row_h = 46;
        int gap = 16;
        int btn_h = 56;
        int start_y = 0;
        int top_margin = (int)(win_h * 0.03);
        int bottom_margin = (int)(win_h * 0.03);
        compute_form_layout(win_h, (int)(sizeof(fields) / sizeof(fields[0])),
                            46, 16, 56, 32, top_margin, bottom_margin,
                            &row_h, &gap, &start_y, &btn_h);

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            fields[i].rect.x = start_x + label_w + 24;
            fields[i].rect.y = start_y + i * (row_h + gap);
            fields[i].rect.w = input_w;
            fields[i].rect.h = row_h;
        }

        int btn_y = fields[(int)(sizeof(fields) / sizeof(fields[0])) - 1].rect.y + row_h + 2 * gap;
        int btn_w = 220;
        buttons[0].rect.x = start_x + content_w - 2 * btn_w - gap;
        buttons[0].rect.y = btn_y;
        buttons[0].rect.w = btn_w;
        buttons[0].rect.h = btn_h;
        buttons[1].rect.x = start_x + content_w - btn_w;
        buttons[1].rect.y = btn_y;
        buttons[1].rect.w = btn_w;
        buttons[1].rect.h = btn_h;

        SDL_SetRenderDrawColor(ren, 18, 20, 24, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.03), win_w, 32 };
        draw_text_centered(ren, font, "New simulation", &title_rect, title_col);

        SDL_Color label_col = { 210, 210, 215, 255 };
        SDL_Color input_col = { 245, 245, 248, 255 };
        SDL_Color input_bg = { 40, 45, 55, 255 };
        SDL_Color input_bg_focus = { 60, 90, 140, 255 };
        SDL_Color error_col = { 230, 90, 90, 255 };

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            SDL_Rect label_rect = { start_x, fields[i].rect.y, label_w, row_h };
            draw_text_left_vcenter(ren, font, fields[i].label, &label_rect, 0, label_col);

            SDL_Color bg = (focus == i) ? input_bg_focus : input_bg;
            SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
            SDL_RenderFillRect(ren, &fields[i].rect);
            SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
            SDL_RenderDrawRect(ren, &fields[i].rect);
            draw_text_left_vcenter(ren, font, fields[i].buf, &fields[i].rect, 10, input_col);
        }

        for (int i = 0; i < 2; i++) {
            int idx = (int)(sizeof(fields) / sizeof(fields[0])) + i;
            if (focus == idx) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);
            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, input_col);
        }

        if (error_msg[0]) {
            SDL_Rect err_rect = { 0, btn_y + btn_h + 20, win_w, 28 };
            draw_text_centered(ren, font, error_msg, &err_rect, error_col);
        }

        SDL_RenderPresent(ren);

        if (!running && accepted) {
            int w = 0, h = 0, rep = 0, k = 0, obs_mode = 0;
            float pU = 0.0f, pD = 0.0f, pL = 0.0f, pR = 0.0f, obs_density = 0.0f;
            if (!parse_int(w_buf, &w) || !parse_int(h_buf, &h) ||
                !parse_int(obs_mode_buf, &obs_mode) || !parse_float(obs_density_buf, &obs_density) ||
                !parse_int(rep_buf, &rep) || !parse_int(k_buf, &k) ||
                !parse_float(pu_buf, &pU) || !parse_float(pd_buf, &pD) ||
                !parse_float(pl_buf, &pL) || !parse_float(pr_buf, &pR)) {
                snprintf(error_msg, sizeof(error_msg), "Check input format.");
                running = 1;
                accepted = 0;
                continue;
            }
            float sum = pU + pD + pL + pR;
            if (w <= 2 || h <= 2 || rep <= 0 || k <= 0 ||
                obs_mode < 0 || obs_mode > 2 ||
                obs_density < 0.0f || obs_density > 0.8f ||
                sum < 0.999f || sum > 1.001f) {
                snprintf(error_msg, sizeof(error_msg), "Invalid values (sum p* = 1).");
                running = 1;
                accepted = 0;
                continue;
            }
            if (obs_mode == 2 && obs_file_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Missing obstacle file.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (sock_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Missing socket.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (out_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Missing output file.");
                running = 1;
                accepted = 0;
                continue;
            }
            cfg->world_w = w;
            cfg->world_h = h;
            cfg->obstacle_mode = obs_mode;
            cfg->obstacle_density = obs_density;
            cfg->obstacle_seed = 0;
            cfg->replications = rep;
            cfg->max_steps = k;
            cfg->pU = pU;
            cfg->pD = pD;
            cfg->pL = pL;
            cfg->pR = pR;
            snprintf(cfg->sock_path, sizeof(cfg->sock_path), "%s", sock_buf);
            strncpy(cfg->obstacle_file, obs_file_buf, sizeof(cfg->obstacle_file) - 1);
            cfg->obstacle_file[sizeof(cfg->obstacle_file) - 1] = '\0';
            strncpy(cfg->output_path, out_buf, sizeof(cfg->output_path) - 1);
            cfg->output_path[sizeof(cfg->output_path) - 1] = '\0';
        }
    }

    SDL_StopTextInput();
    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);
    return accepted;
}

int run_replay_menu(ReplayConfig *cfg)
{
    if (!cfg) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(replay): %s\n", SDL_GetError());
        return 0;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 24);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(replay): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(replay): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk - Relaunch simulation",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(replay): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return 0;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(replay): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return 0;
    }

    char in_buf[256];
    char rep_buf[32] = "100";
    char out_buf[256];
    snprintf(in_buf, sizeof(in_buf), "replication_results.csv");
    snprintf(out_buf, sizeof(out_buf), "replication_results.csv");

    InputField fields[3];
    fields[0] = (InputField){ "Input file:", in_buf, sizeof(in_buf), {0}, 0, 0 };
    fields[1] = (InputField){ "Replication count:", rep_buf, sizeof(rep_buf), {0}, 1, 0 };
    fields[2] = (InputField){ "Output file:", out_buf, sizeof(out_buf), {0}, 0, 0 };

    MenuButton buttons[2];
    buttons[0].label = "Launch";
    buttons[0].action = MENU_ACT_REPLAY;
    buttons[1].label = "Back";
    buttons[1].action = MENU_ACT_EXIT;

    int focus = 0;
    int running = 1;
    int accepted = 0;
    char error_msg[128] = {0};

    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
                    if (point_in_rect(mx, my, &fields[i].rect)) {
                        focus = i;
                        break;
                    }
                }
                for (int i = 0; i < 2; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        accepted = (i == 0);
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                int total = (int)(sizeof(fields) / sizeof(fields[0])) + 2;
                if (e.key.keysym.sym == SDLK_TAB) {
                    int dir = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                    focus = (focus + dir + total) % total;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    focus = (focus - 1 + total) % total;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    focus = (focus + 1) % total;
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                        input_backspace(fields[focus].buf);
                    }
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    if (focus == (int)(sizeof(fields) / sizeof(fields[0]))) {
                        accepted = 1;
                        running = 0;
                    } else if (focus == (int)(sizeof(fields) / sizeof(fields[0])) + 1) {
                        accepted = 0;
                        running = 0;
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    accepted = 0;
                    running = 0;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                if (focus < (int)(sizeof(fields) / sizeof(fields[0]))) {
                    InputField *f = &fields[focus];
                    input_append(f->buf, f->cap, e.text.text, f->numeric_only, f->allow_dot);
                }
            }
        }

        int content_w = (int)(win_w * 0.6);
        int start_x = (win_w - content_w) / 2;
        int label_w = (int)(content_w * 0.45);
        int input_w = content_w - label_w - 24;
        int row_h = 46;
        int gap = 16;
        int btn_h = 56;
        int start_y = 0;
        int top_margin = (int)(win_h * 0.03);
        int bottom_margin = (int)(win_h * 0.03);
        compute_form_layout(win_h, (int)(sizeof(fields) / sizeof(fields[0])),
                            46, 16, 56, 32, top_margin, bottom_margin,
                            &row_h, &gap, &start_y, &btn_h);

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            fields[i].rect.x = start_x + label_w + 24;
            fields[i].rect.y = start_y + i * (row_h + gap);
            fields[i].rect.w = input_w;
            fields[i].rect.h = row_h;
        }

        int btn_y = fields[2].rect.y + row_h + 2 * gap;
        int btn_w = 220;
        buttons[0].rect.x = start_x + content_w - 2 * btn_w - gap;
        buttons[0].rect.y = btn_y;
        buttons[0].rect.w = btn_w;
        buttons[0].rect.h = btn_h;
        buttons[1].rect.x = start_x + content_w - btn_w;
        buttons[1].rect.y = btn_y;
        buttons[1].rect.w = btn_w;
        buttons[1].rect.h = btn_h;

        SDL_SetRenderDrawColor(ren, 18, 20, 24, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.03), win_w, 32 };
        draw_text_centered(ren, font, "Relaunch simulation", &title_rect, title_col);

        SDL_Color label_col = { 210, 210, 215, 255 };
        SDL_Color input_col = { 245, 245, 248, 255 };
        SDL_Color input_bg = { 40, 45, 55, 255 };
        SDL_Color input_bg_focus = { 60, 90, 140, 255 };
        SDL_Color error_col = { 230, 90, 90, 255 };

        for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
            SDL_Rect label_rect = { start_x, fields[i].rect.y, label_w, row_h };
            draw_text_left_vcenter(ren, font, fields[i].label, &label_rect, 0, label_col);

            SDL_Color bg = (focus == i) ? input_bg_focus : input_bg;
            SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
            SDL_RenderFillRect(ren, &fields[i].rect);
            SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
            SDL_RenderDrawRect(ren, &fields[i].rect);
            draw_text_left_vcenter(ren, font, fields[i].buf, &fields[i].rect, 10, input_col);
        }

        for (int i = 0; i < 2; i++) {
            int idx = (int)(sizeof(fields) / sizeof(fields[0])) + i;
            if (focus == idx) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);
            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, input_col);
        }

        if (error_msg[0]) {
            SDL_Rect err_rect = { 0, btn_y + btn_h + 20, win_w, 28 };
            draw_text_centered(ren, font, error_msg, &err_rect, error_col);
        }

        SDL_RenderPresent(ren);

        if (!running && accepted) {
            int rep = 0;
            if (!parse_int(rep_buf, &rep)) {
                snprintf(error_msg, sizeof(error_msg), "Check input format.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (rep <= 0) {
                snprintf(error_msg, sizeof(error_msg), "Replication count > 0.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (in_buf[0] == '\0' || out_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Zadaj vstupny aj vystupny subor.");
                running = 1;
                accepted = 0;
                continue;
            }
            cfg->replications = rep;
            strncpy(cfg->input_path, in_buf, sizeof(cfg->input_path) - 1);
            cfg->input_path[sizeof(cfg->input_path) - 1] = '\0';
            strncpy(cfg->output_path, out_buf, sizeof(cfg->output_path) - 1);
            cfg->output_path[sizeof(cfg->output_path) - 1] = '\0';
        }
    }

    SDL_StopTextInput();
    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);
    return accepted;
}

int run_join_menu(JoinConfig *cfg)
{
    if (!cfg) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(join): %s\n", SDL_GetError());
        return 0;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 24);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(join): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(join): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk - Pripojenie",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(join): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return 0;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(join): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return 0;
    }

    char sock_buf[256];
    snprintf(sock_buf, sizeof(sock_buf), "%s", MENU_DEFAULT_SOCK);

    InputField fields[1];
    fields[0] = (InputField){ "Socket:", sock_buf, sizeof(sock_buf), {0}, 0, 0 };

    MenuButton buttons[2];
    buttons[0].label = "Join";
    buttons[0].action = MENU_ACT_JOIN;
    buttons[1].label = "Back";
    buttons[1].action = MENU_ACT_EXIT;

    int focus = 0;
    int running = 1;
    int accepted = 0;
    char error_msg[128] = {0};

    SDL_StartTextInput();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                if (point_in_rect(mx, my, &fields[0].rect)) {
                    focus = 0;
                }
                for (int i = 0; i < 2; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        accepted = (i == 0);
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                int total = 3;
                if (e.key.keysym.sym == SDLK_TAB) {
                    int dir = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                    focus = (focus + dir + total) % total;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    focus = (focus - 1 + total) % total;
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    focus = (focus + 1) % total;
                } else if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    if (focus == 0) {
                        input_backspace(fields[0].buf);
                    }
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    if (focus == 1) {
                        accepted = 1;
                        running = 0;
                    } else if (focus == 2) {
                        accepted = 0;
                        running = 0;
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    accepted = 0;
                    running = 0;
                }
            } else if (e.type == SDL_TEXTINPUT) {
                if (focus == 0) {
                    input_append(fields[0].buf, fields[0].cap, e.text.text, 0, 0);
                }
            }
        }

        int content_w = (int)(win_w * 0.6);
        int start_x = (win_w - content_w) / 2;
        int label_w = (int)(content_w * 0.35);
        int input_w = content_w - label_w - 24;
        int row_h = 46;
        int gap = 16;
        int btn_h = 56;
        int start_y = 0;
        int top_margin = (int)(win_h * 0.03);
        int bottom_margin = (int)(win_h * 0.03);
        compute_form_layout(win_h, 1, 46, 16, 56, 32, top_margin, bottom_margin,
                            &row_h, &gap, &start_y, &btn_h);

        fields[0].rect.x = start_x + label_w + 24;
        fields[0].rect.y = start_y;
        fields[0].rect.w = input_w;
        fields[0].rect.h = row_h;

        int btn_y = fields[0].rect.y + row_h + 2 * gap;
        int btn_w = 220;
        buttons[0].rect.x = start_x + content_w - 2 * btn_w - gap;
        buttons[0].rect.y = btn_y;
        buttons[0].rect.w = btn_w;
        buttons[0].rect.h = btn_h;
        buttons[1].rect.x = start_x + content_w - btn_w;
        buttons[1].rect.y = btn_y;
        buttons[1].rect.w = btn_w;
        buttons[1].rect.h = btn_h;

        SDL_SetRenderDrawColor(ren, 18, 20, 24, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.03), win_w, 32 };
        draw_text_centered(ren, font, "Join simulation", &title_rect, title_col);

        SDL_Color label_col = { 210, 210, 215, 255 };
        SDL_Color input_col = { 245, 245, 248, 255 };
        SDL_Color input_bg = { 40, 45, 55, 255 };
        SDL_Color input_bg_focus = { 60, 90, 140, 255 };
        SDL_Color error_col = { 230, 90, 90, 255 };

        SDL_Rect label_rect = { start_x, fields[0].rect.y, label_w, row_h };
        draw_text_left_vcenter(ren, font, fields[0].label, &label_rect, 0, label_col);

        SDL_Color bg = (focus == 0) ? input_bg_focus : input_bg;
        SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
        SDL_RenderFillRect(ren, &fields[0].rect);
        SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
        SDL_RenderDrawRect(ren, &fields[0].rect);
        draw_text_left_vcenter(ren, font, fields[0].buf, &fields[0].rect, 10, input_col);

        for (int i = 0; i < 2; i++) {
            int idx = 1 + i;
            if (focus == idx) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);
            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, input_col);
        }

        if (error_msg[0]) {
            SDL_Rect err_rect = { 0, btn_y + btn_h + 20, win_w, 28 };
            draw_text_centered(ren, font, error_msg, &err_rect, error_col);
        }

        SDL_RenderPresent(ren);

        if (!running && accepted) {
            if (sock_buf[0] == '\0') {
                snprintf(error_msg, sizeof(error_msg), "Zadaj socket.");
                running = 1;
                accepted = 0;
                continue;
            }
            if (!copy_path(cfg->sock_path, sizeof(cfg->sock_path), sock_buf)) {
                snprintf(error_msg, sizeof(error_msg), "Socket path too long.");
                running = 1;
                accepted = 0;
                continue;
            }
        }
    }

    SDL_StopTextInput();
    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);
    return accepted;
}

MenuAction run_main_menu(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init(menu): %s\n", SDL_GetError());
        return MENU_ACT_EXIT;
    }

    TTF_Font *font = NULL;
    if (TTF_Init() == 0) {
        const char *font_path = getenv("RW_FONT");
        if (!font_path) font_path = "/usr/share/fonts/open-sans/OpenSans-Regular.ttf";
        font = TTF_OpenFont(font_path, 28);
        if (!font) {
            fprintf(stderr, "TTF_OpenFont(menu): %s\n", TTF_GetError());
        }
    } else {
        fprintf(stderr, "TTF_Init(menu): %s\n", TTF_GetError());
    }

    int win_w = 2800, win_h = 2400;
    SDL_Window *win = SDL_CreateWindow("Random Walk Menu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow(menu): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, NULL, font);
        return MENU_ACT_EXIT;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer(menu): %s\n", SDL_GetError());
        client_shutdown(NULL, -1, (pthread_t)0, NULL, NULL, win, font);
        return MENU_ACT_EXIT;
    }

    MenuButton buttons[4];
    buttons[0].label = "New simulation";
    buttons[0].action = MENU_ACT_NEW;
    buttons[1].label = "Join simulation";
    buttons[1].action = MENU_ACT_JOIN;
    buttons[2].label = "Relaunch simulation";
    buttons[2].action = MENU_ACT_REPLAY;
    buttons[3].label = "Exit";
    buttons[3].action = MENU_ACT_EXIT;

    int selected = 0;
    int running = 1;
    MenuAction action = MENU_ACT_EXIT;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
                action = MENU_ACT_EXIT;
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            } else if (e.type == SDL_MOUSEMOTION) {
                int mx = e.motion.x;
                int my = e.motion.y;
                for (int i = 0; i < 4; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        selected = i;
                        break;
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x;
                int my = e.button.y;
                for (int i = 0; i < 4; i++) {
                    if (point_in_rect(mx, my, &buttons[i].rect)) {
                        action = buttons[i].action;
                        running = 0;
                        break;
                    }
                }
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_DOWN) {
                    selected = (selected + 1) % 4;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    selected = (selected + 3) % 4;
                } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    action = buttons[selected].action;
                    running = 0;
                } else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    action = MENU_ACT_EXIT;
                    running = 0;
                }
            }
        }

        int btn_w = (int)(win_w * 0.6);
        int btn_h = 60;
        int spacing = 16;
        int total_h = 4 * btn_h + 3 * spacing;
        int start_x = (win_w - btn_w) / 2;
        int start_y = (win_h - total_h) / 2;

        for (int i = 0; i < 4; i++) {
            buttons[i].rect.x = start_x;
            buttons[i].rect.y = start_y + i * (btn_h + spacing);
            buttons[i].rect.w = btn_w;
            buttons[i].rect.h = btn_h;
        }

        SDL_SetRenderDrawColor(ren, 20, 22, 26, 255);
        SDL_RenderClear(ren);

        SDL_Color title_col = { 230, 230, 235, 255 };
        SDL_Rect title_rect = { 0, (int)(win_h * 0.12), win_w, 40 };
        draw_text_centered(ren, font, "Random Walk Simulator", &title_rect, title_col);

        for (int i = 0; i < 4; i++) {
            if (i == selected) {
                SDL_SetRenderDrawColor(ren, 70, 120, 200, 255);
            } else {
                SDL_SetRenderDrawColor(ren, 45, 50, 60, 255);
            }
            SDL_RenderFillRect(ren, &buttons[i].rect);

            SDL_SetRenderDrawColor(ren, 15, 15, 18, 255);
            SDL_RenderDrawRect(ren, &buttons[i].rect);

            SDL_Color text_col = { 240, 240, 245, 255 };
            draw_text_centered(ren, font, buttons[i].label, &buttons[i].rect, text_col);
        }

        SDL_Rect hint_rect = { 0, (int)(win_h * 0.9), win_w, 30 };
        SDL_Color hint_col = { 160, 170, 180, 255 };
        draw_text_centered(ren, font, "Enter = confirm, Esc = back", &hint_rect, hint_col);

        SDL_RenderPresent(ren);
    }

    client_shutdown(NULL, -1, (pthread_t)0, NULL, ren, win, font);

    return action;
}

void client_shutdown(ClientState *C, int sockfd, pthread_t net_th,
                     SDL_Texture *canvas, SDL_Renderer *ren,
                     SDL_Window *win, TTF_Font *font)
{
    if (C) {
        atomic_store(&C->running, 0);
    }
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }
    if (net_th) {
        pthread_join(net_th, NULL);
    }
    if (font) TTF_CloseFont(font);
    if (C && C->stats_font) TTF_CloseFont(C->stats_font);
    if (TTF_WasInit()) TTF_Quit();
    if (C) {
        pthread_mutex_destroy(&C->stats_mtx);
        free(C->prob_to_center);
        free(C->avg_steps_to_center);
        free(C->base_prob);
        free(C->base_avg);
        free(C->obstacles);
        if (C->stats_tex) SDL_DestroyTexture(C->stats_tex);
    }
    if (canvas) SDL_DestroyTexture(canvas);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}
