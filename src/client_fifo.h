#pragma once

#include "client_types.h"

void fifo_push(StepFIFO *f, Step s);
int fifo_pop(StepFIFO *f, Step *out);
