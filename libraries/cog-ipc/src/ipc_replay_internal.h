#ifndef COG_IPC_REPLAY_INTERNAL_H
#define COG_IPC_REPLAY_INTERNAL_H

#include <cog/ipc.h>

typedef struct replay_entry {
    size_t offset, len;
    uint16_t type;
} replay_entry_t;
struct cog_ipc_replay {
    char session[128];
    size_t session_len, max_entries, max_bytes, count, used;
    uint64_t generation, tail;
    replay_entry_t *entries;
    unsigned char *bytes;
};

#endif
