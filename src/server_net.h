#pragma once

#include "server_types.h"

void clients_broadcast(Server *S, uint32_t type, const void *payload, uint32_t len);
int make_listen_socket(Server *S);
void *accept_thread(void *arg);
