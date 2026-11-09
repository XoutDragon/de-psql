#include "de_shim.h"
#include "postgres.h"

#include "commands/vacuum.h"
#include "dynamic_ext.h"
#include "storage/itemptr.h"

static void de_unpack_tid(uint64 packed, ItemPointer tid) {
  ItemPointerSet(tid, (BlockNumber)(packed >> 16),
                 (OffsetNumber)(packed & 0xFFFF));
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 */
IndexBulkDeleteResult *debulkdelete(IndexVacuumInfo *info,
                                    IndexBulkDeleteResult *stats,
                                    IndexBulkDeleteCallback callback,
                                    void *callback_state) {
  DEIndex *idx;
  int64_t *keys;
  uint64_t *tids;
  size_t count;
  size_t ndead = 0;
  DEBufEntry e;

  if (stats == NULL)
    stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));

  idx = de_open_index(info->index);
  if (de_scan_all(idx, &keys, &tids, &count) != 0) {
    de_close_index(info->index, idx);
    ereport(ERROR, (errmsg("could not read from index")));
  }
  de_close_index(info->index, idx);

  for (int i = 0; i < count; i++) {
    ItemPointerData tid;

    vacuum_delay_point(false);

    de_unpack_tid(tids[i], &tid);
    if (callback(&tid, callback_state)) {
      keys[ndead] = keys[i];
      tids[ndead] = tids[i];
      ndead++;
    }
  }

  if (ndead > 0) {
    idx = de_open_index(info->index);
    for (int i = 0; i < ndead; i++) {
      if (de_erase(idx, keys[i], tids[i]) != 0) {
        de_close_index(info->index, idx);
        free(keys);
        free(tids);
        ereport(ERROR, (errmsg("could not delete from index")));
      }
      e.key = (int32)keys[i];
      e.tid = tids[i];
      e.tombstone = true;
      DEBufferAppend(info->index, &e, de_flush_seq(idx));
    }
    de_close_index(info->index, idx);
  }

  free(keys);
  free(tids);

  stats->tuples_removed += ndead;
  stats->num_index_tuples = count - ndead;
  stats->num_pages = 0;
  stats->estimated_count = false;

  return stats;
}

/*
 * Post-VACUUM cleanup.
 */
IndexBulkDeleteResult *devacuumcleanup(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats) {
  DEIndex *idx;
  int64_t *keys;
  uint64_t *tids;
  size_t count;

  if (info->analyze_only)
    return stats;

  if (stats != NULL)
    return stats;

  idx = de_open_index(info->index);
  if (de_scan_all(idx, &keys, &tids, &count) != 0) {
    de_close_index(info->index, idx);
    ereport(ERROR, (errmsg("could not read index")));
  }

  de_close_index(info->index, idx);
  free(keys);
  free(tids);

  stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
  stats->num_index_tuples = count;
  stats->num_pages = 0;
  stats->estimated_count = false;

  return stats;
}
