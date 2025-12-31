#pragma once

#include "client_types.h"

int run_new_sim_menu(NewSimConfig *cfg);
int run_replay_menu(ReplayConfig *cfg);
int run_join_menu(JoinConfig *cfg);
MenuAction run_main_menu(void);
void client_shutdown(ClientState *C, int sockfd, pthread_t net_th,
                     SDL_Texture *canvas, SDL_Renderer *ren,
                     SDL_Window *win, TTF_Font *font);
