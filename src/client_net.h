#pragma once

#include "client_types.h"

int connect_unix(const char *path);
void send_stop(int sockfd);
void send_mode(int sockfd, uint32_t mode);
void *net_thread(void *arg);
