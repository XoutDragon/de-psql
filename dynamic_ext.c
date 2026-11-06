// This is currently a base that only prints out the functions calls, the
// arguments, and timestamp

/* postgres.h must come first; keep it in its own block so include sorting can't
 * move it. */
#include "postgres.h"

#include "dynamic_ext.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#include "access/reloptions.h"
#include "access/tableam.h"
#include "commands/vacuum.h"
#include "de_shim.h"
#include "miscadmin.h"
#include "storage/block.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "storage/off.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

PG_MODULE_MAGIC_EXT(.name = "dynamic_ext", .version = PG_VERSION);

PG_FUNCTION_INFO_V1(dynamic_ext);

static relopt_kind de_relopt_kind;
static relopt_parse_elt de_relopt_tab[INDEX_MAX_KEYS + 1];

static void de_unpack_tid(uint64 packed, ItemPointer tid) {
  ItemPointerSet(tid, (BlockNumber)(packed >> 16),
                 (OffsetNumber)(packed & 0xFFFF));
}

void _PG_init(void) {
  de_relopt_kind = add_reloption_kind();

  add_int_reloption(de_relopt_kind, "buffer_lwm",
                    "The number of records that can be inserted before a "
                    "buffer flush is initiated",
                    DEFAULT_DE_BUFFER_LWM, MIN_DE_BUFFER_LWM, MAX_DE_BUFFER_LWM,
                    AccessExclusiveLock);
  add_int_reloption(de_relopt_kind, "buffer_hwm",
                    "The maximum buffer capacity, inserts will begin to fail "
                    "once this number is reached, until the buffer flush has "
                    "completed. Has no effect in single-threaded operation",
                    DEFAULT_DE_BUFFER_HWM, MIN_DE_BUFFER_HWM, MAX_DE_BUFFER_HWM,
                    AccessExclusiveLock);
  add_int_reloption(de_relopt_kind, "scale_factor",
                    "The rate at which the capacity of levels grows; should be "
                    "at least 2 for reasonable performance",
                    DEFAULT_DE_SCALE_FACTOR, MIN_DE_SCALE_FACTOR,
                    MAX_DE_SCALE_FACTOR, AccessExclusiveLock);

  de_relopt_tab[0].optname = "buffer_lwm";
  de_relopt_tab[0].opttype = RELOPT_TYPE_INT;
  de_relopt_tab[0].offset = offsetof(DEOptions, buffer_lwm);

  de_relopt_tab[1].optname = "buffer_hwm";
  de_relopt_tab[1].opttype = RELOPT_TYPE_INT;
  de_relopt_tab[1].offset = offsetof(DEOptions, buffer_hwm);

  de_relopt_tab[2].optname = "scale_factor";
  de_relopt_tab[2].opttype = RELOPT_TYPE_INT;
  de_relopt_tab[2].offset = offsetof(DEOptions, scale_factor);
}

static void de_index_dir(char *path, Oid dbOid, RelFileNumber relNumber) {
  snprintf(path, MAXPGPATH, "%s/dynamic_ext/%u/%u", DataDir, dbOid, relNumber);
}

static DEIndex *de_open_index(Relation index) {
  DEOptions *rdopts = (DEOptions *)index->rd_options;
  size_t lwm = DEFAULT_DE_BUFFER_LWM;
  size_t hwm = DEFAULT_DE_BUFFER_HWM;
  size_t sf = DEFAULT_DE_SCALE_FACTOR;
  char dir[MAXPGPATH];
  DEIndex *idx;

  LockRelation(index, ShareUpdateExclusiveLock);

  if (rdopts != NULL) {
    lwm = rdopts->buffer_lwm;
    hwm = rdopts->buffer_hwm;
    sf = rdopts->scale_factor;
  }

  de_index_dir(dir, index->rd_locator.dbOid, index->rd_locator.relNumber);
  idx = de_open(dir, lwm, hwm, sf);

  if (idx == NULL) {
    ereport(ERROR, errmsg("Could not open index"));
  }

  elog(NOTICE, "de_open lwm=%d hwm=%d sf=%d", lwm, hwm, sf);

  return idx;
}

static void de_close_index(Relation index, DEIndex *idx) {
  de_close(idx);
  UnlockRelation(index, ShareUpdateExclusiveLock);
}

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

  if (stats == NULL)
    stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));

  idx = de_open_index(info->index);
  if (de_scan_all(idx, &keys, &tids, &count) != 0) {
    de_close_index(info->index, idx);
    ereport(ERROR, (errmsg("could not read from index")));
  }

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

/*
 * Estimate cost of index scan.
 */
void decostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                    Cost *indexStartupCost, Cost *indexTotalCost,
                    Selectivity *indexSelectivity, double *indexCorrelation,
                    double *indexPages) {
  GenericCosts costs = {0};
  IndexOptInfo *index = path->indexinfo;

  costs.numIndexTuples = index->tuples;

  genericcostestimate(root, path, loop_count, &costs);

  *indexStartupCost = costs.indexStartupCost;
  *indexTotalCost = costs.indexTotalCost;
  *indexSelectivity = costs.indexSelectivity;
  *indexCorrelation = costs.indexCorrelation;
  *indexPages = costs.numIndexPages;
}

/*
 * Parse reloptions for index.
 */
bytea *deoptions(Datum reloptions, bool validate) {
  DEOptions *rdopts;

  rdopts = (DEOptions *)build_reloptions(reloptions, validate, de_relopt_kind,
                                         sizeof(DEOptions), de_relopt_tab,
                                         sizeof(de_relopt_tab));

  if (validate && rdopts != NULL && rdopts->buffer_lwm > rdopts->buffer_hwm) {
    ereport(
        ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg(
             "dynamic_ext: buffer_lwm must be less or equal to buffer_hwm")));
  }

  return (bytea *)rdopts;
}

/*
 * Validator for opclass.
 */
bool devalidate(Oid opclassoid) {

  fprintf(stderr, "devalidate called at %ld\n", (long)time(NULL));
  return true;
}

/*
 * Begin scan of index.
 */
IndexScanDesc debeginscan(Relation r, int nkeys, int norderbys) {
  fprintf(stderr, "debeginscan\n");
  IndexScanDesc scan;
  DEScanOpaque so;

  so = palloc0_object(DEScanOpaqueData);
  so->done = false;

  scan = RelationGetIndexScan(r, nkeys, norderbys);

  scan->opaque = so;

  return scan;
}

/*
 * Rescan an index.
 */
void derescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
              ScanKey orderbys, int norderbys) {
  fprintf(stderr, "rescan\n");
  DEScanOpaque so = ((DEScanOpaque)scan->opaque);

  if (scankey && nscankeys > 0)
    memmove(scan->keyData, scankey, nscankeys * sizeof(ScanKeyData));

  free(so->tids);
  so->tids = NULL;
  so->count = 0;
  so->pos = 0;
  so->done = false;
}

bool degettuple(IndexScanDesc scan, ScanDirection dir) {
  fprintf(stderr, "gettuple start\n");
  DEScanOpaque so = ((DEScanOpaque)scan->opaque);
  int64 low;
  int64 high;

  if (!so->done) {
    low = PG_INT32_MIN;
    high = PG_INT32_MAX;

    for (int i = 0; i < scan->numberOfKeys; i++) {
      int v;
      v = DatumGetInt32(scan->keyData[i].sk_argument);
      switch (scan->keyData[i].sk_strategy) {
      case 1:
        high = Min(high, v - 1);
        break;
      case 2:
        high = Min(high, v);
        break;
      case 3:
        low = Max(low, v);
        high = Min(high, v);
        break;
      case 4:
        low = Max(low, v);
        break;
      case 5:
        low = Max(low, v + 1);
        break;
      }
    }

    if (low <= high) {
      DEIndex *idx;
      Relation index = scan->indexRelation;

      idx = de_open_index(index);
      de_range(idx, low, high, &so->tids, &so->count);
      de_close_index(scan->indexRelation, idx);
    }

    so->pos = 0;
    so->done = true;
  }

  if (so->pos >= so->count)
    return false;

  fprintf(stderr, "pos=%zu\n", so->pos);
  ItemPointerSet(&scan->xs_heaptid, so->tids[so->pos] >> 16,
                 so->tids[so->pos] & 0xFFFF);
  so->pos++;
  scan->xs_recheck = true;
  return true;
}

/*
 * End scan of index.
 */
void deendscan(IndexScanDesc scan) {
  fprintf(stderr, "endscan\n");
  DEScanOpaque so = ((DEScanOpaque)scan->opaque);

  free(so->tids);
  pfree(so);
}

Datum dynamic_ext(PG_FUNCTION_ARGS) {
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  /*
   * Total number of strategies (operators) by which we can traverse/search
   * this AM.  Zero if AM does not have a fixed set of strategy assignments.
   */
  amroutine->amstrategies = 5;
  /* total number of support functions that this AM uses */
  amroutine->amsupport = 5;
  /* opclass options support function number or 0 */
  amroutine->amoptsprocnum = 0;
  /* does AM support ORDER BY indexed column's value? */
  amroutine->amcanorder = false;
  /* does AM support ORDER BY result of an operator on indexed column? */
  amroutine->amcanorderbyop = false;
  /* does AM support hashing using API consistent with the hash AM? */
  amroutine->amcanhash = false;
  /* do operators within an opfamily have consistent equality semantics? */
  amroutine->amconsistentequality = false;
  /* do operators within an opfamily have consistent ordering semantics? */
  amroutine->amconsistentordering = false;
  /* does AM support backward scanning? */
  amroutine->amcanbackward = false;
  /* does AM support UNIQUE indexes? */
  amroutine->amcanunique = false;
  /* does AM support multi-column indexes? */
  amroutine->amcanmulticol = false;
  /* does AM require scans to have a constraint on the first index column? */
  amroutine->amoptionalkey = false;
  /* does AM handle ScalarArrayOpExpr quals? */
  amroutine->amsearcharray = false;
  /* does AM handle IS NULL/IS NOT NULL quals? */
  amroutine->amsearchnulls = false;
  /* can index storage data type differ from column data type? */
  amroutine->amstorage = false;
  /* can an index of this type be clustered on? */
  amroutine->amclusterable = false;
  /* does AM handle predicate locks? */
  amroutine->ampredlocks = false;
  /* does AM support parallel scan? */
  amroutine->amcanparallel = false;
  /* does AM support parallel build? */
  amroutine->amcanbuildparallel = false;
  /* does AM support columns included with clause INCLUDE? */
  amroutine->amcaninclude = false;
  /* does AM use maintenance_work_mem? */
  amroutine->amusemaintenanceworkmem = false;
  /* does AM summarize tuples, with at least all tuples in the block
   * summarized in one summary */
  amroutine->amsummarizing = false;
  /* OR of parallel vacuum flags */
  amroutine->amparallelvacuumoptions = 0;
  /* type of data stored in index, or InvalidOid if variable */
  amroutine->amkeytype = InvalidOid;

  /* interface functions */
  amroutine->ambuild = debuild;
  amroutine->ambuildempty = debuildempty;
  amroutine->aminsert = deinsert;
  amroutine->ambulkdelete = debulkdelete;
  amroutine->amvacuumcleanup = devacuumcleanup;
  amroutine->amcostestimate = decostestimate;
  amroutine->amoptions = deoptions;
  amroutine->amvalidate = devalidate;
  amroutine->ambeginscan = debeginscan;
  amroutine->amrescan = derescan;
  amroutine->amgettuple = degettuple;
  amroutine->amendscan = deendscan;

  PG_RETURN_POINTER(amroutine);
}
