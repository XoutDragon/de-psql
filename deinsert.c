#include "c.h"
#include "de_shim.h"
#include "postgres.h"

#include "access/tableam.h"
#include "dynamic_ext.h"
#include "storage/itemptr.h"
#include "utils/palloc.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(.name = "dynamic_ext", .version = PG_VERSION);

static void debuildcallback(Relation index, ItemPointer tid, Datum *values,
                            bool *isnull, bool tupleIsAlive, void *state) {
  DEBuildState *bs = (DEBuildState *)state;
  int32 key;
  uint64 packed_tid;
  uint64 seq;

  if (isnull[0])
    return;

  key = DatumGetInt32(values[0]);
  // Stores each row's tuple ID (int,32, int 16) into a 64 bit sized block
  packed_tid = ((uint64)ItemPointerGetBlockNumber(tid) << 16) |
               ItemPointerGetOffsetNumber(tid);

  if (de_insert(bs->idx, key, packed_tid) != 0)
    ereport(ERROR,
            errmsg("dynamic_ext: build insert failed: %s", de_last_error()));
  bs->indtuples += 1;

  seq = de_flush_seq(bs->idx);
  if (seq != bs->last_seq) {
    bs->npending = 0;
    bs->last_seq = seq;
  }

  if (bs->npending == bs->cap) {
    bs->cap *= 2;
    bs->pending = repalloc(bs->pending, bs->cap * sizeof(DEBufEntry));
  }
  bs->pending[bs->npending].key = key;
  bs->pending[bs->npending].tid = packed_tid;
  bs->pending[bs->npending].tombstone = false;
  bs->npending++;
}

/*
 * Build a new index.
 */
IndexBuildResult *debuild(Relation heap, Relation index, IndexInfo *indexInfo) {
  IndexBuildResult *result;
  DEBuildState bs;
  double reltuples;
  char path[MAXPGPATH];

  de_remove_orphan_dirs();
  if (index->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("dynamic_ext does not support UNLOGGED indexes")));

  de_index_dir(path, index->rd_locator.dbOid, index->rd_locator.relNumber);
  de_create(path);
  DEInitMetapage(index);

  bs.idx = de_acquire_index(index);
  bs.index = index;
  bs.indtuples = 0;
  bs.cap = 1024;
  bs.npending = 0;
  bs.pending = palloc(bs.cap * sizeof(DEBufEntry));
  bs.last_seq = de_flush_seq(bs.idx);

  // Scan Table
  reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
                                     debuildcallback, (void *)&bs, NULL);

  DEBufferAppendMany(index, bs.pending, bs.npending, bs.last_seq);
  de_cache_sync(index);

  pfree(bs.pending);

  de_release_index(index, bs.idx);
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
  DEBufEntry e;
  DEPending *p;
  uint64 seq;

  if (isnull[0])
    return false;

  p = (DEPending *)indexInfo->ii_AmCache;

  if (p == NULL) {
    p = MemoryContextAllocZero(indexInfo->ii_Context, sizeof(DEPending));
    p->idx = de_acquire_index(index);
    p->cap = 1024;
    p->entries =
        MemoryContextAlloc(indexInfo->ii_Context, p->cap * sizeof(DEBufEntry));
    p->seq = de_flush_seq(p->idx);
    indexInfo->ii_AmCache = p;
  }
  idx = p->idx;

  key = DatumGetInt32(values[0]);

  packed_tid = ((uint64)ItemPointerGetBlockNumber(ht_ctid) << 16) |
               ItemPointerGetOffsetNumber(ht_ctid);
  if (de_insert(idx, key, packed_tid) != 0) {
    de_discard_index(index, idx);
    p->idx = NULL;
    ereport(ERROR, errmsg("Insert into index failed"));
  }

  seq = de_flush_seq(idx);
  e.key = key;
  e.tid = packed_tid;
  e.tombstone = false;

  if (seq != p->seq) {
    p->n = 0;
    p->seq = seq;
    DEBufferAppendMany(index, &e, 1, seq);
    de_cache_sync(index);
  } else {
    if (p->n == p->cap) {
      p->cap *= 2;
      p->entries = repalloc(p->entries, p->cap * sizeof(DEBufEntry));
    }
    p->entries[p->n++] = e;
  }

  /* The function's Boolean result value is significant only when checkUnique is
  UNIQUE_CHECK_PARTIAL. In this case a true result means the new entry is known
  unique, whereas false means it might be non-unique (and a deferred uniqueness
  check must be scheduled). For other cases a constant false result is
  recommended.
  */
  return false;
}

void deinsertcleanup(Relation index, IndexInfo *indexInfo) {
  DEPending *p = (DEPending *)indexInfo->ii_AmCache;

  if (p != NULL && p->n > 0) {
    DEBufferAppendMany(index, p->entries, p->n, p->seq);
    de_cache_sync(index);
    p->n = 0;
  }

  if (p != NULL && p->idx != NULL) {
    de_release_index(index, p->idx);
    p->idx = NULL;
  }
}