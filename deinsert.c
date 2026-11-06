#include "postgres.h"

#include "access/tableam.h"
#include "dynamic_ext.h"
#include "storage/itemptr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(.name = "dynamic_ext", .version = PG_VERSION);

static void debuildcallback(Relation index, ItemPointer tid, Datum *values,
                            bool *isnull, bool tupleIsAlive, void *state) {
  DEBuildState *bs = (DEBuildState *)state;
  int32 key;
  uint64 packed_tid;

  if (isnull[0])
    return;

  key = DatumGetInt32(values[0]);
  // Stores each row's tuple ID (int,32, int 16) into a 64 bit sized block
  packed_tid = ((uint64)ItemPointerGetBlockNumber(tid) << 16) |
               ItemPointerGetOffsetNumber(tid);

  de_insert(bs->idx, key, packed_tid);
  bs->indtuples += 1;
}

/*
 * Build a new index.
 */
IndexBuildResult *debuild(Relation heap, Relation index, IndexInfo *indexInfo) {
  IndexBuildResult *result;
  DEBuildState bs;
  double reltuples;

  char path[MAXPGPATH];

  de_index_dir(path, index->rd_locator.dbOid, index->rd_locator.relNumber);

  de_create(path);
  bs.idx = de_open_index(index);
  bs.index = index;
  bs.indtuples = 0;

  // Scan Table
  reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
                                     debuildcallback, (void *)&bs, NULL);
  // Get callback to insert from the stupid table

  de_close_index(index, bs.idx);
  result = palloc_object(IndexBuildResult);
  result->heap_tuples = reltuples;
  result->index_tuples = bs.indtuples;

  return result;
}

/*
 * Called only for UNLOGGED indexes, right after debuild().  Our data lives
 * in the dynamic_ext folder, not in Postgres pages, so there is nothing to
 * write into the init fork.  Do not call de_create() here: it would wipe
 * the index debuild() just built.
 */
void debuildempty(Relation index) {}

/*
 * Insert new tuple to the index.
 */
bool deinsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
              Relation heapRel, IndexUniqueCheck checkUnique,
              bool indexUnchanged, IndexInfo *indexInfo) {
  DEIndex *idx;
  int32 key;
  uint64 packed_tid;

  if (isnull[0])
    return false;

  idx = de_open_index(index);
  key = DatumGetInt32(values[0]);

  packed_tid = ((uint64)ItemPointerGetBlockNumber(ht_ctid) << 16) |
               ItemPointerGetOffsetNumber(ht_ctid);
  if (de_insert(idx, key, packed_tid) != 0) {
    de_close_index(index, idx);

    ereport(ERROR, errmsg("Insert into index failed"));
  }
  de_close_index(index, idx);
  /* The function's Boolean result value is significant only when checkUnique is
  UNIQUE_CHECK_PARTIAL. In this case a true result means the new entry is known
  unique, whereas false means it might be non-unique (and a deferred uniqueness
  check must be scheduled). For other cases a constant false result is
  recommended.
  */
  return false;
}
