#ifndef SAVELOAD_H
#define SAVELOAD_H

#include <stddef.h>

typedef struct SaveLoadInfo SaveLoadInfo;
typedef void SaveLoadInfoFunc(SaveLoadInfo *info, void *data, size_t data_size);

struct SaveLoadInfo {
    SaveLoadInfoFunc *func;
};

#endif
