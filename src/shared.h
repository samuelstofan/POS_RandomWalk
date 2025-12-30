#pragma once
#include <stdint.h>

#define RWALK_SOCK_MAX 107

typedef enum {
    MSG_WELCOME = 1,
    MSG_STEP    = 2,
    MSG_MODE    = 3,
    MSG_PROGRESS= 4,
    MSG_STOP    = 5,
    MSG_ERROR   = 6,
    MSG_STATS   = 7,
} MsgType;

typedef enum {
    MODE_INTERACTIVE = 1,
    MODE_SUMMARY     = 2,
} SimMode;

#pragma pack(push, 1)
typedef struct {
    uint32_t type;   // MsgType
    uint32_t len;    // payload bytes
} MsgHdr;

typedef struct {
    uint32_t world_w;
    uint32_t world_h;
    uint32_t mode;      // SimMode
    uint32_t replications;
    uint32_t max_steps;
    uint32_t step_delay_ms;
    float pU, pD, pL, pR; // sum = 1
} MsgWelcome;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t step_index;
} MsgStep;

typedef struct {
    uint32_t mode; // SimMode
} MsgMode;

typedef struct {
    uint32_t current_replication;
    uint32_t total_replications;
} MsgProgress;

typedef struct {
    uint32_t world_w;
    uint32_t world_h;
} MsgStatsHdr;
#pragma pack(pop)
