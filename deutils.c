// This is currently a base that only prints out the functions calls, the
// arguments, and timestamp

#include "postgres.h"

#include "access/reloptions.h"
#include "dynamic_ext.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(dynamic_ext);

static relopt_kind de_relopt_kind;
static relopt_parse_elt de_relopt_tab[3];

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
void de_index_dir(char *path, Oid dbOid, RelFileNumber relNumber) {
  snprintf(path, MAXPGPATH, "%s/dynamic_ext/%u/%u", DataDir, dbOid, relNumber);
}

DEIndex *de_open_index(Relation index) {
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

void de_close_index(Relation index, DEIndex *idx) {
  de_close(idx);
  UnlockRelation(index, ShareUpdateExclusiveLock);
}

/*
 * Parse reloptions for index.
 */
bytea *deoptions(Datum reloptions, bool validate) {
  DEOptions *rdopts;

  rdopts = (DEOptions *)build_reloptions(reloptions, validate, de_relopt_kind,
                                         sizeof(DEOptions), de_relopt_tab,
                                         lengthof(de_relopt_tab));

  if (validate && rdopts != NULL && rdopts->buffer_lwm > rdopts->buffer_hwm) {
    ereport(
        ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg(
             "dynamic_ext: buffer_lwm must be less or equal to buffer_hwm")));
  }

  return (bytea *)rdopts;
}
