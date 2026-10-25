#ifndef _DYNAMIC_EXT_H_
#define _DYNAMIC_EXT_H_

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "de_shim.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

typedef struct DEBuildState {
  DEIndex *idx;
  Relation index;
  double indtuples;
} DEBuildState;

typedef struct DEScanOpaqueData {
  bool done;  // check if a scan was done.
  uint64 *tids;  // array of tids returned by de_range
  size_t count; // How many tids are in the tid array
  size_t pos;  // index of next TID to return
} DEScanOpaqueData;

typedef DEScanOpaqueData *DEScanOpaque;

extern Datum dynamic_ext(PG_FUNCTION_ARGS);

extern IndexBuildResult *debuild(Relation heap, Relation index,
                                 IndexInfo *indexInfo);
extern void debuildempty(Relation index);
extern bool deinsert(Relation index, Datum *values, bool *isnull,
                     ItemPointer ht_ctid, Relation heapRel,
                     IndexUniqueCheck checkUnique, bool indexUnchanged,
                     struct IndexInfo *indexInfo);
extern IndexBulkDeleteResult *debulkdelete(IndexVacuumInfo *info,
                                           IndexBulkDeleteResult *stats,
                                           IndexBulkDeleteCallback callback,
                                           void *callback_state);
extern IndexBulkDeleteResult *devacuumcleanup(IndexVacuumInfo *info,
                                              IndexBulkDeleteResult *stats);
extern void decostestimate(PlannerInfo *root, IndexPath *path,
                           double loop_count, Cost *indexStartupCost,
                           Cost *indexTotalCost, Selectivity *indexSelectivity,
                           double *indexCorrelation, double *indexPages);
extern bytea *deoptions(Datum reloptions, bool validate);
extern bool devalidate(Oid opclassoid);
extern IndexScanDesc debeginscan(Relation r, int nkeys, int norderbys);
extern void derescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
                     ScanKey orderbys, int norderbys);
extern bool degettuple(IndexScanDesc scan, ScanDirection dir);
extern void deendscan(IndexScanDesc scan);

#endif
