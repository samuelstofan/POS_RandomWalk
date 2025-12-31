#pragma once

#include <stddef.h>

int send_all(int fd, const void *buf, size_t n);
int recv_all(int fd, void *buf, size_t n);
