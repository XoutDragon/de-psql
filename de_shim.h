#ifndef DE_SHIM_H
#define DE_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DEIndex DEIndex;

int de_create(const char *dir);
DEIndex *de_open(const char *dir, size_t buffer_lwm, size_t buffer_hwm, size_t scale_factor);
void de_close(DEIndex *idx);
int de_insert(DEIndex *idx, int64_t key, uint64_t tid);
int de_scan_all(DEIndex *idx, int64_t **keys, uint64_t **tids, size_t *count);
int de_erase(DEIndex * idx, int64_t key, uint64_t tid);
int de_lookup(DEIndex *idx, int64_t key, uint64_t *tid, bool *found);
int de_range(DEIndex *idx, int64_t low, int64_t high, uint64_t **tids, size_t *count);
const char *de_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
